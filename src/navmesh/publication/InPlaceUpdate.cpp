#include "navmesh/publication/InPlaceUpdate.h"

#include <utility>

namespace SkyBuild::Navmesh {
InPlaceUpdate::InPlaceUpdate(std::unique_ptr<PreparedUpdate> update, Tick limit)
    : prepared(std::move(update)), timeout(limit) {}

void InPlaceUpdate::Finish(UpdateState result) noexcept {
  state = result;
  lease.reset();
}

UpdateState InPlaceUpdate::Step(UpdateBackend &backend, Tick now) {
  if (state == UpdateState::kPrepared) {
    if (!prepared) {
      Finish(UpdateState::kFailed);
      return state;
    }
    try {
      lease = backend.RequestPause(*prepared);
    } catch (...) {
      Finish(UpdateState::kFailed);
      return state;
    }
    if (!lease) {
      Finish(UpdateState::kUnavailable);
      return state;
    }
    started = now;
    state = UpdateState::kWaiting;
  }
  if (state != UpdateState::kWaiting)
    return state;
  if (now < started || now - started >= timeout) {
    Finish(UpdateState::kTimedOut);
    return state;
  }
  const auto pause = lease->Poll();
  if (pause == PauseState::kWaiting)
    return state;
  if (pause == PauseState::kFailed) {
    Finish(UpdateState::kFailed);
    return state;
  }
  if (!lease->StillCurrent(*prepared)) {
    Finish(UpdateState::kStale);
    return state;
  }
  if (!lease->Commit(*prepared)) {
    Finish(UpdateState::kFailed);
    return state;
  }
  if (!lease->InvalidatePaths(*prepared)) {
    lease->Restore(*prepared);
    Finish(UpdateState::kFailed);
    return state;
  }
  Finish(UpdateState::kApplied);
  return state;
}

void InPlaceUpdate::Cancel() noexcept {
  if (state == UpdateState::kPrepared || state == UpdateState::kWaiting)
    Finish(UpdateState::kCancelled);
}

std::unique_ptr<PreparedUpdate> InPlaceUpdate::TakeAppliedBackup() noexcept {
  return state == UpdateState::kApplied ? std::move(prepared) : nullptr;
}
} // namespace SkyBuild::Navmesh
