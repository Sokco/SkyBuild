#include "navmesh/runtime/NativeSynchronization.h"
#include "navmesh/cuts/NativeCutAudit.h"
#include "navmesh/native/NativeFormDraft.h"
#include "navmesh/native/NativePathing.h"
#include "navmesh/native/NativeSearchAudit.h"
#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/runtime/GenerationGameplayPause.h"
#include "navmesh/runtime/PreparationHandoff.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "navmesh/runtime/StreamingPause.h"
#include <atomic>
#include <chrono>
#if defined(_MSC_VER) && !defined(_CPPUNWIND)
#error Native navigation transactions require C++ exception unwinding (/EHsc).
#endif

namespace SkyBuild::Navmesh::NativeSynchronization {
namespace {
using GetCoordinator = void *(*)();
GetCoordinator coordinator{};
SearchPauseAPI api{};
PauseProbe &Probe() {
  // Native engine singletons may already be gone during DLL static teardown.
  // Scene transitions explicitly cancel ownership while they are alive. Keep
  // this one small process-lifetime object out of static destructor ordering.
  static auto *probe = new PauseProbe;
  return *probe;
}
enum class Action {
  kNone,
  kRefreshLoadedPaths,
  kCaptureBaseline,
  kInstallReplacement,
  kRestoreCommitted,
  kReconcileMembership,
  kRestoreOriginal
};
std::atomic_bool available{}, taskQueued{};
std::atomic<Action> requested{Action::kNone};
std::atomic<PauseProbeState> reported{PauseProbeState::kIdle};
std::atomic_bool sceneLoaded{};
std::atomic_bool deferredStreamingRefresh{};
std::atomic<PauseProbe::Tick> nextMembershipCheck{};
PreparationHandoff<Action> preparationHandoff;

auto Now() {
  return static_cast<PauseProbe::Tick>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
void Publish() {
  const auto state = Probe().State();
  if (reported.exchange(state) != state) {
    spdlog::info("Navigation synchronization: {}", StatusText());
  }
}
} // namespace

void Initialize() {
  NativeSearchAudit::Initialize();
  NativeCutAudit::Initialize();

  // The plugin-wide minimum runtime is enforced at load. Resolve native
  // functions through Address Library; executable prefixes are not an ABI.
  const REL::Relocation<std::uintptr_t> get{REL::RelocationID(29765, 30584)};
  const REL::Relocation<std::uintptr_t> request{REL::RelocationID(88946, 91401)};
  const REL::Relocation<std::uintptr_t> ready{REL::RelocationID(88948, 91403)};
  const REL::Relocation<std::uintptr_t> release{REL::RelocationID(88949, 91404)};
  const REL::Relocation<std::uintptr_t> idle{REL::RelocationID(88950, 91405)};
  coordinator = reinterpret_cast<GetCoordinator>(get.address());
  api.request = reinterpret_cast<decltype(api.request)>(request.address());
  api.ready = reinterpret_cast<decltype(api.ready)>(ready.address());
  api.release = reinterpret_cast<decltype(api.release)>(release.address());
  api.idle = reinterpret_cast<decltype(api.idle)>(idle.address());
  available.store(true);
  spdlog::info("Bound native navmesh search pause; runtime publication is "
               "available through the navigation commands.");
}
void SetSceneLoaded(bool loaded) {
  nextMembershipCheck.store(0);
  deferredStreamingRefresh.store(false);
  sceneLoaded = loaded;
  requested.store(Action::kNone);
  preparationHandoff.Cancel();
  if (!loaded) {
    NativeReplacement::SceneReset();
    Probe().Cancel();
    preparationHandoff.Cancel();
    NativeFormPreparation::Clear();
    Publish();
  } else
    NativeReplacement::SceneLoaded();
}
bool PendingAction() {
  return requested.load() != Action::kNone ||
         reported.load() == PauseProbeState::kWaiting;
}
void CancelPending() {
  nextMembershipCheck.store(0);
  deferredStreamingRefresh.store(false);
  requested.store(Action::kNone);
  Probe().Cancel();
  preparationHandoff.Cancel();
  NativeFormPreparation::Clear();
  Publish();
}
bool RetireBeforeStreaming() {
  if (!NativeReplacement::Installed())
    return true;
  if (!available.load() || NativeSearchAudit::InSearchOnCurrentThread())
    return false;
  requested.store(Action::kNone);
  // Do not release an existing task-owned request before acquiring this one.
  auto calls = api;
  calls.instance = coordinator();
  using Pump = void (*)(void *, void *);
  const REL::Relocation<Pump> pump{REL::RelocationID(88974, 91429)};
  const bool restored = WithStreamingPause(
      calls,
      [&] {
        // 91401/91403/91404 wrap this internal coordinator. 91404 itself
        // pumps 91429(internal, nullptr) while releasing the final owner.
        pump(*static_cast<void **>(calls.instance), nullptr);
      },
      Now, [] { Sleep(1); },
      [] {
        const bool refreshed = NativeReplacement::RestoreForStreaming(nullptr);
        // Loading can make the actor/path diagnostic unavailable after the
        // graph was already restored. Retirement is the publication state,
        // rather than the success of that optional diagnostic.
        return refreshed || !NativeReplacement::Installed();
      });
  if (restored) {
    Probe().Cancel();
    preparationHandoff.Cancel();
    NativeFormPreparation::Clear();
    NativeReplacement::FinishPending();
    Publish();
    spdlog::info("Retired runtime navigation before native cell teardown; "
                 "committed geometry retained for cache restoration.");
  }
  return restored;
}
namespace {
void Tick() {
  if (!available.load() || !sceneLoaded)
    return;
  const auto ui = RE::UI::GetSingleton();
  if (GenerationGameplayPause::Owned() &&
      !GenerationGameplayPause::OnlyGameplayPause())
    return; // Another menu may pause too; preserve our in-flight transaction.
  if (ui && ((!GenerationGameplayPause::Owned() && ui->GameIsPaused()) ||
             ui->IsMenuOpen("Loading Menu"))) {
    Probe().Cancel();
    preparationHandoff.Cancel();
    NativeFormPreparation::Clear();
    Publish();
    return;
  }
  auto action = requested.exchange(Action::kNone);
  if (action != Action::kNone && NativeFormPreparation::Pending()) {
    NativeFormPreparation::Clear();
    preparationHandoff.Cancel();
  }
  if (action == Action::kNone)
    if (const auto ready =
            preparationHandoff.TakeReady(NativeFormPreparation::Pending(),
                                         NativeFormPreparation::PendingReady()))
      action = *ready;
  if (action == Action::kNone && deferredStreamingRefresh.load() &&
      Probe().State() != PauseProbeState::kWaiting &&
      !NativeFormPreparation::Pending()) {
    const auto player = RE::PlayerCharacter::GetSingleton();
    const auto cell = player ? player->GetParentCell() : nullptr;
    if (cell && cell->IsAttached()) {
      deferredStreamingRefresh.store(false);
      action = Action::kRefreshLoadedPaths;
      spdlog::info("Running deferred path refresh for attached actors after "
                   "streaming retirement.");
    }
  }
  if (action == Action::kNone && Probe().State() != PauseProbeState::kWaiting &&
      Now() >= nextMembershipCheck && NativeReplacement::Installed() &&
      !NativeFormPreparation::Pending()) {
    nextMembershipCheck = Now() + 100;
    if (NativeReplacement::NeedsMembershipReconciliation()) {
      action = Action::kReconcileMembership;
      nextMembershipCheck = Now() + 2000;
    }
  }
  if (action != Action::kNone && Probe().State() != PauseProbeState::kWaiting) {
    api.instance = coordinator();
    preparationHandoff.Begin(action);
    Probe().Start(
        api, Now(),
        action == Action::kInstallReplacement ? NativeReplacement::Install
        : action == Action::kRestoreCommitted ? RestoreCommittedNavigation
        : action == Action::kRestoreOriginal  ? NativeReplacement::Restore
        : action == Action::kReconcileMembership
            ? NativeReplacement::ReconcileMembership
        : action == Action::kRefreshLoadedPaths
            ? NativePathing::RefreshLoadedPaths
        : action == Action::kCaptureBaseline ? CapturePendingBaseline
                                             : nullptr,
        nullptr, NativeDerivedWork::QueuesIdle);
    Publish();
  }
  Probe().Step(Now());
  preparationHandoff.CompletePause(Probe().State() == PauseProbeState::kWaiting,
                                   NativeFormPreparation::Pending());
  if (Probe().State() != PauseProbeState::kWaiting)
    NativeFormPreparation::Finish();
  if (Probe().State() != PauseProbeState::kWaiting)
    NativeReplacement::FinishPending();
  Publish();
}
} // namespace
void Update() {
  if (!available.load() || !sceneLoaded.load())
    return;
  const bool pending = NativeFormPreparation::Pending() ||
                       requested.load() != Action::kNone ||
                       deferredStreamingRefresh.load() ||
                       reported.load() == PauseProbeState::kWaiting;
  // Commands and in-flight work run promptly. An idle installed graph needs
  // a task only when its existing membership-check deadline is due.
  if (!pending &&
      (!NativeReplacement::Installed() || Now() < nextMembershipCheck.load()))
    return;
  if (taskQueued.exchange(true))
    return;
  if (const auto tasks = SKSE::GetTaskInterface()) {
    // Player updates are only a scheduler. Execute native coordination and
    // movement operations through SKSE's game-thread task queue.
    tasks->AddTask([] {
      taskQueued.store(false);
      try {
        Tick();
      } catch (const std::exception &error) {
        if (GenerationGameplayPause::Owned())
          AbortGeneration(error.what());
        else {
          CancelPending();
          spdlog::error("Navigation coordination failed: {}", error.what());
        }
      }
    });
  } else {
    taskQueued.store(false);
    requested.store(Action::kNone);
    reported.store(PauseProbeState::kUnavailable);
  }
}
void RequestReplacement() {
  if (available.load())
    requested.store(Action::kInstallReplacement);
}
void RequestOriginalRestore() {
  SuppressCommittedRestoration();
  if (available.load())
    requested.store(Action::kRestoreOriginal);
}
void RequestCommittedRestore() {
  if (available.load())
    requested.store(Action::kRestoreCommitted);
}
void DeferStreamingPathRefresh() {
  deferredStreamingRefresh.store(true);
  spdlog::info("Deferred path refresh until an attached player cell is "
               "available; graph restoration already completed.");
}
bool RequestBaselineCapture() {
  if (!available.load() || reported.load() == PauseProbeState::kWaiting)
    return false;
  auto none = Action::kNone;
  return requested.compare_exchange_strong(none, Action::kCaptureBaseline);
}
bool Available() { return available.load(); }
PauseProbeState ProbeState() {
  return requested.load() != Action::kNone ? PauseProbeState::kWaiting
                                           : reported.load();
}
const char *StatusText() {
  if (requested.load() != Action::kNone)
    return "Queued: close this menu to update navigation.";
  switch (reported.load()) {
  case PauseProbeState::kIdle:
    return "Navigation is ready.";
  case PauseProbeState::kWaiting:
    return "Waiting for native navigation work to finish.";
  case PauseProbeState::kPassed:
    return "Navigation update complete.";
  case PauseProbeState::kPassedSharedPause:
    return "Navigation update complete; existing pause preserved.";
  case PauseProbeState::kTimedOut:
    return "Timed out; navigation update pause was released.";
  case PauseProbeState::kCancelled:
    return "Cancelled; navigation update pause was released.";
  case PauseProbeState::kActionFailed:
    return "The navigation update did not complete; navigation update pause "
           "was "
           "released.";
  case PauseProbeState::kFailed:
    return "Failed: native pause behavior did not match the trace.";
  case PauseProbeState::kUnavailable:
    return "No verified native synchronization binding for this runtime.";
  }
  return "Unknown synchronization state.";
}
} // namespace SkyBuild::Navmesh::NativeSynchronization
