#include "navmesh/native/NativePathing.h"
#include "navmesh/runtime/NativeSynchronization.h"
#include <cstring>
#include <mutex>

namespace SkyBuild::Navmesh::NativePathing {
namespace {
struct Result {
  std::size_t followers{}, cleared{}, local{}, inactive{}, unsupported{};
  bool ran{}, completed{};
  std::string failure;
};
Result result;
std::mutex resultMutex;
void Publish(Result value) {
  const std::scoped_lock lock(resultMutex);
  result = value;
}
struct Follower {
  RE::NiPointer<RE::Actor> actor;
  RE::MovementControllerNPC *controller{};
  RE::IMovementInterface *pathFollower{};
};
bool Refresh(void *context, bool allAttached) noexcept {
  Result report;
  report.ran = true;
  const auto fail = [&](std::string reason) {
    report.failure = std::move(reason);
    Publish(report);
    spdlog::info("Native path refresh incomplete: {}; {} routes cleared, {} "
                 "local, {} inactive, {} unsupported; all-attached scope {}.",
                 report.failure, report.cleared, report.local, report.inactive,
                 report.unsupported, allAttached);
    return false;
  };
  try {
    if (!NativeSynchronization::Available()) {
      return fail("navigation synchronization unavailable");
    }
    const auto player = RE::PlayerCharacter::GetSingleton();
    const auto cell = player ? player->GetParentCell() : nullptr;
    const auto processes = RE::ProcessLists::GetSingleton();
    const auto pathing = RE::Pathing::GetSingleton();
    if (!cell)
      return fail("player has no parent cell during transition");
    if (!cell->IsAttached())
      return fail("player cell is not attached during transition");
    if (!processes || !pathing)
      return fail("native process/pathing singleton unavailable");
    const REL::Relocation<RE::BSFixedString *> name{REL::RelocationID(525747, 412228)};
    const REL::Relocation<std::uintptr_t> expected{
        RE::VTABLE_MovementAgentPathFollowerStandard[4]};
    const REL::Relocation<std::uintptr_t> refresh{REL::RelocationID(88326, 90737)};
    if (!name.get()->data() ||
        reinterpret_cast<const std::uintptr_t *>(expected.address())[1] !=
            refresh.address()) {
      return fail(
          "standard path-follower binding differs from the verified interface");
    }

    // Pin every actor and preflight all followers before requesting any
    // path refresh. Unsupported agents prevent this diagnostic from claiming
    // that the cell's cached paths have been accounted for.
    std::vector<Follower> followers;
    processes->ForEachHighActor([&](RE::Actor *actor) {
      const auto affected =
          static_cast<const std::set<RE::TESObjectCELL *> *>(context);
      if (!actor || actor == player ||
          (affected      ? !affected->contains(actor->GetParentCell())
           : allAttached ? (!actor->GetParentCell() ||
                            !actor->GetParentCell()->IsAttached())
                         : actor->GetParentCell() != cell) ||
          actor->IsDead() || actor->IsDisabled())
        return RE::BSContainer::ForEachResult::kContinue;
      const auto controller =
          actor->GetActorRuntimeData().movementController.get();
      if (controller) {
        if (const auto pathFollower = controller->GetInterface1(*name.get())) {
          ++report.followers;
          if (*reinterpret_cast<const std::uintptr_t *>(pathFollower) !=
              expected.address())
            ++report.unsupported;
          else
            followers.push_back(
                {actor->GetHandle().get(), controller, pathFollower});
        }
      }
      return RE::BSContainer::ForEachResult::kContinue;
    });
    if (report.unsupported) {
      spdlog::warn("Navigation path refresh rejected {} unsupported followers; "
                   "no paths were changed.",
                   report.unsupported);
      return fail("unsupported path-follower interfaces; no routes changed");
    }

    // Verified standard interface slot/signature: native routine 90737 takes
    // this interface and a 32-bit mode, returning 0 (no active route), 1
    // (local movement branch) or 2 (released request/solution). Mode 0 chooses
    // the refresh branch when not handling local movement near the goal.
    // No fields are written directly and no NPC AI package is reset.
    using Refresh = std::uint32_t (*)(RE::IMovementInterface *, std::uint32_t);
    const auto call = reinterpret_cast<Refresh>(refresh.address());
    for (const auto &follower : followers) {
      if (!follower.actor ||
          follower.actor->GetActorRuntimeData().movementController.get() !=
              follower.controller ||
          follower.controller->GetInterface1(*name.get()) !=
              follower.pathFollower) {
        ++report.unsupported;
        return fail(
            "actor movement controller or follower changed after preflight");
      }
      switch (call(follower.pathFollower, 0)) {
      case 0:
        ++report.inactive;
        break;
      case 1:
        ++report.local;
        break;
      case 2:
        ++report.cleared;
        break;
      default:
        spdlog::error("Native path refresh returned an unrecognized result.");
        return fail("native path refresh returned an unrecognized result");
      }
    }
    report.completed = true;
    Publish(report);
    spdlog::debug(
        "Navigation paths refreshed: {} cleared, {} local, {} inactive",
        report.cleared, report.local, report.inactive);
    return true;
  } catch (const std::exception &error) {
    return fail(std::string("native path refresh exception: ") + error.what());
  } catch (...) {
    return fail("unknown native path refresh exception");
  }
}
} // namespace
bool RefreshPaths(void *context) noexcept { return Refresh(context, false); }
bool RefreshLoadedPaths(void *) noexcept { return Refresh(nullptr, true); }

std::string Status() {
  Result view;
  {
    const std::scoped_lock lock(resultMutex);
    view = result;
  }
  if (!view.ran)
    return "No path refresh requested.";
  if (!view.completed)
    return "Path refresh incomplete: " + view.failure;
  return std::format(
      "{} paths refreshed; {} local movement branches; {} inactive followers.",
      view.cleared, view.local, view.inactive);
}
} // namespace SkyBuild::Navmesh::NativePathing
