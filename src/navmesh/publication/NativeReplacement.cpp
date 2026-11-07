#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/diagnostics/DebugVisualization.h"
#include "navmesh/runtime/GenerationGameplayPause.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/cuts/NativeCutAudit.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/native/NativeObstacleSafety.h"
#include "navmesh/native/NativePathing.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/runtime/NativeSynchronization.h"
#include "navmesh/publication/RetirementEligibility.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "navmesh/diagnostics/TimingTrace.h"
#include <mutex>
namespace SkyBuild::Navmesh::NativeReplacement {
namespace {
auto &candidate = *new std::unique_ptr<NativePublication>;
auto &active = *new std::unique_ptr<NativePublication>;
// A retired replacement can still be referenced by an engine location or
// solution. Pin the retained identity cohort until restart; cache guards
// invalidate those references lazily. Never run native destruction at DLL
// teardown.
auto &retired = *new std::vector<std::unique_ptr<NativePublication>>;
std::mutex mutex;
std::atomic_bool live{};
std::string status{"Original navigation is active."};
std::string retirementStatus{"No retired navigation graphs."};
void ReportRetirement() {
  std::size_t bytes{}, sampled{}, released{}, releasedCore{};
  for (const auto &graph : retired) {
    const auto &memory = graph->RetirementMemory();
    sampled += memory.sampled;
    bytes += memory.knownBytes;
    released += memory.comparisonBytesReleased;
    releasedCore += graph->ReleasedCoreBytes();
  }
  const auto detailed = std::format(
      "Retired graphs: {} ({} measured).\nKnown payload at retirement: {:.2f} "
      "MiB; core buffers freed: {:.2f} KiB; comparison storage freed: {:.2f} "
      "KiB.\nForms and navigation identities remain retained until Skyrim "
      "exits. Measurements exclude opaque allocations and may count shared "
      "storage more than once.",
      retired.size(), sampled, double(bytes) / (1024 * 1024),
      double(releasedCore) / 1024, double(released) / 1024);
  {
    const std::scoped_lock lock(mutex);
    retirementStatus = detailed;
  }
  spdlog::debug("{}", detailed);
}
std::set<RE::TESObjectCELL *> affected;
using Dependencies = std::set<RE::FormID>;
std::atomic<std::shared_ptr<const Dependencies>> dependencies, deferredTargets;
void SetLive(bool value) {
  if (value) {
    auto ids = std::make_shared<Dependencies>();
    for (const auto cell : affected)
      if (cell)
        ids->insert(cell->GetFormID());
    dependencies.store(std::move(ids), std::memory_order_release);
    deferredTargets.store(
        std::make_shared<Dependencies>(active->DeferredTargets()),
        std::memory_order_release);
  } else {
    dependencies.store(nullptr, std::memory_order_release);
    deferredTargets.store(nullptr, std::memory_order_release);
  }
  live.store(value);
}
void RetireRestored(std::unique_ptr<NativePublication> graph) {
  std::string error;
  if (!graph->SealRetired(error))
    spdlog::warn("Retired graph recovery journal retained: {}", error);
  retired.push_back(std::move(graph));
  ReportRetirement();
}

void Report(std::string value) {
  const std::scoped_lock lock(mutex);
  status = std::move(value);
  spdlog::info("Native navmesh replacement: {}", status);
}
void RecoverSurvivingPublication() {
  if (active)
    return;
  for (std::size_t i = retired.size(); i > 0; --i) {
    if (!retired[i - 1]->StillPublished())
      continue;
    active = std::move(retired[i - 1]);
    retired.erase(retired.begin() + static_cast<std::ptrdiff_t>(i - 1));
    ReportRetirement();
    affected = active->AffectedCells();
    SetLive(true);
    Report("Recovered surviving runtime navigation after save load; "
           "original restoration still requires full journal validation.");
    return;
  }
}
} // namespace
bool Prepare() {
  TimingTrace timing{"NativeReplacement::Prepare"};
  if (active || candidate) {
    Report("Finish the current navigation operation first.");
    return false;
  }
  spdlog::info("Runtime replacement checkpoint: detached preparation begins "
               "(location hooks deferred).");
  if (!NativeFormPreparation::Prepare()) {
    if (NativeFormPreparation::Pending())
      return false;
    Report(NativeFormPreparation::Status());
    NativeFormPreparation::Finish();
    return false;
  }
  auto graph = NativeFormPreparation::TakeGraphForReplacement();
  timing.Mark("native form preparation and graph transfer");
  spdlog::info("Runtime replacement checkpoint: detached graph transferred; "
               "publication preparation begins.");
  std::string error;
  candidate = NativePublication::Prepare(std::move(graph), error);
  timing.Mark("native publication preparation");
  if (!candidate) {
    Report("Replacement preparation failed: " + error);
    spdlog::info("Runtime replacement checkpoint: rejected preparation cleaned "
                 "up; no location hooks enabled.");
    return false;
  }
  affected = candidate->AffectedCells();
  Report("Replacement staged; waiting for navigation pause.");
  return true;
}
bool CopyOriginalView(NativeOriginalView &out, std::string &error) {
  if (!active || !active->Installed()) {
    error = "No active original navigation publication";
    return false;
  }
  return active->CopyOriginalView(out, error);
}
bool Install(void *) noexcept {
  TimingTrace timing{"NativeReplacement::Install"};
  std::unique_ptr<NativePublication> previous;
  std::set<RE::TESObjectCELL *> previousAffected;
  auto fail = [&](std::string failure) {
    // Keep every exposed cohort pinned, including failed candidates. Restore
    // the previous live view before releasing this same navigation pause.
    if (candidate && candidate->Installed()) {
      active = std::move(candidate);
      SetLive(true);
      if (previous) {
        retired.push_back(std::move(previous));
        ReportRetirement();
      }
      DebugVisualization::RefreshInstalledGeometry();
      Report("Replacement remains installed after failure: " + failure);
      return false;
    }
    candidate.reset();
    if (previous) {
      std::string error;
      if (previous->Commit(error)) {
        active = std::move(previous);
        affected = previousAffected;
        SetLive(true);
        const auto validation = active->ValidateInstalled();
        const bool refreshed = NativePathing::RefreshPaths(&affected);
        DebugVisualization::RefreshInstalledGeometry();
        Report("New replacement rejected; previous replacement reinstated: " +
               failure +
               (validation.empty() && refreshed
                    ? ""
                    : "; rollback verification: " + validation + "; " +
                          NativePathing::Status()));
      } else {
        retired.push_back(std::move(previous));
        ReportRetirement();
        SetLive(false);
        affected = previousAffected;
        NativePathing::RefreshPaths(&affected);
        DebugVisualization::RefreshInstalledGeometry();
        Report("New replacement rejected; originals retained because previous "
               "publication could not be reinstated: " +
               failure + "; " + error);
      }
    } else
      Report(std::move(failure));
    return false;
  };
  try {
    RecoverSurvivingPublication();
    previousAffected = affected;
    timing.Mark("recover surviving publication");
    if (active) {
      std::string error;
      if (!active->Restore(error)) {
        Report("Replacement handoff rejected; previous navigation unchanged: " +
               error);
        return false;
      }
      previous = std::move(active);
      SetLive(false);
      spdlog::info("Replacement handoff: original native view opened inside "
                   "ready pause; gameplay has not resumed.");
    }
    // Geometry generation finished before this callback. Native preparation
    // requires original registry/cut identities, so stage and publish within
    // this single pause, without refreshing paths or drawing the interim view.
    timing.Mark("restore previous graph for handoff");
    if (!candidate && !Prepare()) {
      if (NativeFormPreparation::Pending()) {
        if (previous) {
          std::string error;
          if (!previous->Commit(error)) {
            NativeFormPreparation::Clear();
            return fail(
                "Could not retain previous navigation during preparation: " +
                error);
          }
          active = std::move(previous);
          affected = previousAffected;
          SetLive(true);
        }
        Report("Preparing navigation topology; current "
               "navigation remains active.");
        return true;
      }
      return fail(Status());
    }
    timing.Mark("prepare replacement");
    if (!NativeLocationCache::Initialize() ||
        !NativeObstacleSafety::Initialize())
      return fail("Native location/obstacle guards could not be installed.");
    std::string error;
    if (!candidate->Commit(error))
      return fail("Replacement rejected: " + error);
    timing.Mark("initialize guards and commit");
    const auto reason = candidate->ValidateInstalled();
    timing.Mark("validate installed graph");
    if (!reason.empty() || !NativePathing::RefreshPaths(&affected)) {
      const auto failure = reason.empty() ? NativePathing::Status() : reason;
      if (!candidate->Restore(error))
        return fail(failure + "; " + error);
      RetireRestored(std::move(candidate));
      return fail(failure);
    }
    active = std::move(candidate);
    timing.Mark("refresh NPC paths and inspect meshes");
    SetLive(true);
    if (previous)
      RetireRestored(std::move(previous));
    ConfirmInstalledCache();
    // TODO: reclaim native core buffers only after raw worker/cache borrowers
    // have a verified retirement boundary. A reference census is insufficient;
    // retain geometry until process exit, as we already do for mesh identities.
    DebugVisualization::RefreshInstalledGeometry();
    timing.Mark("commit caches and request gizmo refresh");
    Report(std::format("Installed runtime replacement: {} meshes. Native "
                       "registry, cell lists, bounds, cuts and paths verified. "
                       "Use Revert Navmesh to restore the originals.",
                       active->MeshCount()));
    return true;
  } catch (const std::exception &exception) {
    return fail(std::string("Runtime replacement failed: ") + exception.what());
  }
}
bool NeedsMembershipReconciliation() {
  return active && active->NeedsMembershipReconciliation();
}
bool ReconcileMembership(void *) noexcept {
  try {
    if (!active)
      return false;
    std::string error;
    if (!active->ReconcileMembership(error)) {
      Report("Live membership reconciliation rejected: " + error);
      return false;
    }
    if (!NativePathing::RefreshPaths(&affected))
      NativeSynchronization::DeferStreamingPathRefresh();
    DebugVisualization::RefreshInstalledGeometry();
    Report("Runtime navigation reconciled; verified dormant meshes removed "
           "from live cell lists.");
    return true;
  } catch (const std::exception &error) {
    Report(std::string("Live membership reconciliation failed: ") +
           error.what());
    return false;
  }
}
static bool RestoreImpl(bool streaming) noexcept {
  try {
    RecoverSurvivingPublication();
    if (!active)
      return false;
    std::string error;
    if (!active->Restore(error)) {
      Report("Original restoration rejected: " + error);
      return false;
    }
    const bool refreshed = NativePathing::RefreshPaths(&affected);
    if (!refreshed && streaming)
      NativeSynchronization::DeferStreamingPathRefresh();
    const auto count = active->MeshCount();
    RetireRestored(std::move(active));
    SetLive(false);
    DebugVisualization::RefreshInstalledGeometry();
    Report(std::format(
        "{}: installed and restored {} runtime meshes; original "
        "buffers retained intact. Rollback buffers released when retirement "
        "verified; native reference owners retained.",
        refreshed ? "Passed" : "Restored; path refresh needs inspection",
        count));
    if (!refreshed)
      spdlog::info("Restoration path refresh detail: {}",
                   NativePathing::Status());
    return refreshed;
  } catch (const std::exception &exception) {
    Report(std::string("Runtime restoration failed: ") + exception.what());
    return false;
  }
}
bool Restore(void *) noexcept { return RestoreImpl(false); }
bool RestoreForStreaming(void *) noexcept { return RestoreImpl(true); }
bool Installed() { return live.load(); }
bool DependsOnCell(RE::FormID cell) {
  const auto cohort = dependencies.load(std::memory_order_acquire);
  // An absent snapshot during activation must fail conservatively.
  return live.load() && (!cohort || cohort->contains(cell));
}
std::set<RE::FormID>
ReadyDeferredConnections(const std::set<RE::FormID> &cells) {
  std::set<RE::FormID> ready;
  const auto pending = deferredTargets.load(std::memory_order_acquire);
  if (!live.load() || !pending || pending->empty())
    return ready;
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
  const RE::BSReadLockGuard publicationGuard(*publication.get());
  const RE::BSReadLockGuard listingGuard(*listing.get());
  for (const auto id : *pending) {
    const auto mesh = RE::TESForm::LookupByID<RE::NavMesh>(id);
    const auto cell = mesh ? mesh->GetSaveParentCell() : nullptr;
    if (cell && cells.contains(cell->GetFormID()) && cell->IsAttached() &&
        !mesh->triangles.empty())
      ready.insert(id);
  }
  return ready;
}
bool TracksMesh(const RE::NavMesh *mesh) {
  if ((active && active->TracksMesh(mesh)) ||
      (candidate && candidate->TracksMesh(mesh)))
    return true;
  return std::ranges::any_of(retired, [&](const auto &publication) {
    return publication->TracksMesh(mesh);
  });
}
bool TracksRetiredMesh(const RE::NavMesh *mesh) {
  return mesh && std::ranges::any_of(retired, [&](const auto &publication) {
           return !publication->Installed() && publication->TracksMesh(mesh);
         });
}
void FinishPending() { candidate.reset(); }
void SceneLoaded() {
  // Loading reconstructs native locations and may reuse freed info addresses.
  // An obsolete pointer set from the preceding world must not affect them.
  NativeLocationCache::Obsolete empty;
  for (const auto &publication : retired) {
    const auto infos = publication->EmptyRetiredInfos();
    empty.insert(infos.begin(), infos.end());
  }
  NativeLocationCache::Publish(
      std::make_shared<const NativeLocationCache::Obsolete>(std::move(empty)));
}
namespace {
bool ReclaimRetiredBuffers() noexcept {
  try {
    using Coordinator = void *(*)();
    using Ready = bool (*)(void *);
    const REL::Relocation<Coordinator> coordinator{REL::ID(30584)};
    const REL::Relocation<Ready> ready{REL::ID(91403)};
    if (!ready(coordinator())) {
      Report("Retirement audit requires a ready native navigation pause.");
      return false;
    }
    const auto writers = NativeCutAudit::Snapshot();
    const auto loads = NativeAuthoredBasis::LoadObservation();
    if (!NativeLocationCache::Available() || !writers.available ||
        writers.observation.active || loads.active) {
      Report("Retirement audit deferred: guards unavailable or cut/load worker "
             "active.");
      return false;
    }
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)},
        listing{REL::ID(402042)};
    const RE::BSWriteLockGuard publicationGuard(*publication.get());
    const RE::BSReadLockGuard listingGuard(*listing.get());
    const REL::Relocation<RE::BSNavmeshInfoMap **> global{REL::ID(402040)};
    auto *registry = static_cast<RE::NavMeshInfoMap *>(*global.get());
    if (!registry)
      return false;
    const RE::BSReadLockGuard registryGuard(registry->mapLock);
    const auto inputs = CaptureRegisteredCutData();
    const auto dispatched = inputs.retainedCollections.find(0x1D8);
    const auto dispatchedTasks = dispatched == inputs.retainedCollections.end()
                                     ? 0U
                                     : dispatched->second;
    spdlog::debug(
        "Retirement cleanup obstacle census: {} prepared tasks, {} dispatched "
        "tasks. Observation only; no reclamation authorization.",
        inputs.preparedTasks, dispatchedTasks);
    for (const auto &[offset, count] : inputs.retainedCollections)
      spdlog::debug("Retirement cleanup obstacle collection +{:X}: {} entries",
                    offset, count);
    // Registration retries and retained edge effects need not drain for a
    // read-only census. Scheduled work still prevents following undo arrays;
    // the writer/load checks below remain required independently.
    if (!CanInspectRetirementWork(inputs.preparedTasks, dispatchedTasks)) {
      Report("Retirement audit deferred: native obstacle tasks are prepared or "
             "dispatched; collection counts recorded.");
      return false;
    }
    NativePublication::RetirementAuditContext context;
    NativeDerivedWork derivedWork;
    const auto borrowedWork = derivedWork.BorrowedInfos();
    context.derivedWorkInfos.insert(borrowedWork.begin(), borrowedWork.end());
    for (const auto &entry : registry->infoMap)
      context.registeredInfos.insert(entry.second);
    for (const auto &input : inputs.inputs)
      context.cutMemberships.insert(input.affectedInfos.begin(),
                                    input.affectedInfos.end());
    RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> loaded;
    if (!RE::Pathing::GetSingleton() ||
        !RE::Pathing::GetSingleton()->GetAllLoadedNavmeshes5(loaded))
      return false;
    std::vector<const RE::BSNavmesh *> sources;
    for (const auto &mesh : loaded)
      if (mesh) {
        sources.push_back(mesh.get());
        context.loadedForms.insert(mesh.get());
      }
    if (active)
      active->AppendCutSources(sources);
    for (const auto &graph : retired)
      graph->AppendCutSources(sources);
    for (const auto *mesh : sources)
      for (const auto &edge : mesh->extraEdgeInfo)
        context.portalTargets[reinterpret_cast<std::uintptr_t>(mesh)].insert(
            edge.portal.otherMeshID);
    context.undoTargets = CaptureCutBorrowedTargets(sources);
    const auto after = NativeCutAudit::Snapshot();
    if (WriterObserver::Changed(writers.observation, after.observation) ||
        WriterObserver::Changed(loads,
                                NativeAuthoredBasis::LoadObservation())) {
      Report("Retirement audit deferred: cut/load activity changed during "
             "census.");
      return false;
    }
    if (!derivedWork.Unchanged())
      return false;
    std::size_t candidates{};
    for (const auto &graph : retired)
      candidates += graph->ReclaimRetiredCore(context);
    if (!derivedWork.Unchanged()) {
      Report("Retirement inspection observed a derived-work change; inspect "
             "logs before further cleanup.");
      return false;
    }
    ReportRetirement();
    spdlog::debug("Automatic retired geometry cleanup: {} forms reclaimed; "
                  "identities and auxiliary data retained",
                  candidates);
    return true;
  } catch (const std::exception &error) {
    const auto message =
        std::string("Retirement operation stopped: ") + error.what();
    Report(message);
    return false;
  } catch (...) {
    Report("Retirement operation stopped: unknown native failure.");
    return false;
  }
}
} // namespace
bool ReclaimRetired(void *) noexcept { return ReclaimRetiredBuffers(); }
void SceneReset() {
  // The loader owns the scene transition. Keep exposed buffers/infos pinned;
  // never restore an old registry/cell journal into the newly loaded world.
  if (active) {
    retired.push_back(std::move(active));
    ReportRetirement();
    Report("Scene transition retained the exposed navigation graph "
           "until restart.");
  }
  candidate.reset();
  SetLive(false);
}
std::string Status() {
  const std::scoped_lock lock(mutex);
  return status;
}
std::string RetirementStatus() {
  const std::scoped_lock lock(mutex);
  return retirementStatus;
}
} // namespace SkyBuild::Navmesh::NativeReplacement
