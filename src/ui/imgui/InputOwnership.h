#pragma once
#include <array>
#include <cstddef>

namespace SkyBuild::UI {
struct InputOwnership {
  std::array<bool, 512> held{};
  bool Consume(std::size_t slot, bool down, bool up, bool capture) {
    if (slot >= held.size())
      return capture;
    // A fresh press supersedes stale ownership if its release was lost.
    if (down)
      held[slot] = capture;
    const bool consume = capture || held[slot];
    if (up)
      held[slot] = false;
    return consume;
  }
  void Reset() { held.fill(false); }
};
// Always deliver the release of a press already seen by gameplay, even if a
// SkyBuild window/tool acquired input in the meantime.
struct InputDelivery {
  std::array<bool, 512> gameplayHeld{};
  bool Consume(std::size_t slot, bool down, bool up, bool capture) {
    if (slot >= gameplayHeld.size())
      return capture;
    if (up) {
      const bool delivered = gameplayHeld[slot];
      gameplayHeld[slot] = false;
      return capture && !delivered;
    }
    if (down)
      gameplayHeld[slot] = !capture;
    return capture;
  }
};
} // namespace SkyBuild::UI
