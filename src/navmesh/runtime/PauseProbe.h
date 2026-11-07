#pragma once

#include <cstdint>

namespace SkyBuild::Navmesh {
// Bound only after the runtime's entry points have been verified. These calls
// manage search admission, not geometry publication or actor-path invalidation.
struct SearchPauseAPI {
  void *instance{};
  void (*request)(void *){};
  bool (*ready)(void *){};
  bool (*release)(void *){}; // true when this releases the final owner
  bool (*idle)(void *){};
  [[nodiscard]] bool Valid() const noexcept {
    return instance && request && ready && release && idle;
  }
};

enum class PauseProbeState {
  kIdle,
  kWaiting,
  kPassed,
  kPassedSharedPause,
  kTimedOut,
  kCancelled,
  kActionFailed,
  kFailed,
  kUnavailable
};

// Game-thread only. Exercise the actual owned-pause protocol without touching
// any meshes. Never keep a navcut/cell/registry lock across native release:
// release may pump movement callbacks before returning.
class PauseProbe {
public:
  using Tick = std::uint64_t;
  using ReadyAction = bool (*)(void *) noexcept;
  using Admission = bool (*)() noexcept;
  PauseProbe() = default;
  PauseProbe(const PauseProbe &) = delete;
  PauseProbe &operator=(const PauseProbe &) = delete;
  ~PauseProbe() { ReleaseOwned(); }

  void Start(SearchPauseAPI api, Tick now, ReadyAction action = nullptr,
             void *context = nullptr, Admission admission = nullptr) noexcept {
    if (state == PauseProbeState::kWaiting)
      return;
    calls = api;
    if (!calls.Valid()) {
      state = PauseProbeState::kUnavailable;
      return;
    }
    started = now;
    whenReady = action;
    actionContext = context;
    canAdmit = admission;
    if (!canAdmit || canAdmit()) {
      calls.request(calls.instance);
      owned = 1;
    }
    state = PauseProbeState::kWaiting;
  }

  void Step(Tick now) noexcept {
    if (state != PauseProbeState::kWaiting)
      return;
    if (now < started || now - started >= (owned ? 2000U : 10000U)) {
      Finish(PauseProbeState::kTimedOut);
      return;
    }
    if (!owned) {
      if (canAdmit && !canAdmit())
        return;
      calls.request(calls.instance);
      owned = 1;
      started = now; // Native search-pause timeout starts after admission.
    }
    if (!calls.ready(calls.instance))
      return;

    // Two owned requests let us prove releasing one cannot resume the other.
    calls.request(calls.instance);
    ++owned;
    --owned;
    const bool releasedFinal = calls.release(calls.instance);
    if (releasedFinal || !calls.ready(calls.instance)) {
      Finish(PauseProbeState::kFailed);
      return;
    }
    if (whenReady && !whenReady(actionContext)) {
      Finish(PauseProbeState::kActionFailed);
      return;
    }
    --owned;
    const bool lastOwner = calls.release(calls.instance);
    state = lastOwner ? (calls.idle(calls.instance) ? PauseProbeState::kPassed
                                                    : PauseProbeState::kFailed)
                      : (calls.ready(calls.instance)
                             ? PauseProbeState::kPassedSharedPause
                             : PauseProbeState::kFailed);
  }

  void Cancel() noexcept {
    if (state == PauseProbeState::kWaiting)
      Finish(PauseProbeState::kCancelled);
  }
  [[nodiscard]] PauseProbeState State() const noexcept { return state; }

private:
  void ReleaseOwned() noexcept {
    while (owned) {
      --owned;
      calls.release(calls.instance);
    }
  }
  void Finish(PauseProbeState result) noexcept {
    ReleaseOwned();
    state = result;
  }
  SearchPauseAPI calls{};
  Admission canAdmit{};
  ReadyAction whenReady{};
  void *actionContext{};
  Tick started{};
  unsigned owned{};
  PauseProbeState state{PauseProbeState::kIdle};
};
} // namespace SkyBuild::Navmesh
