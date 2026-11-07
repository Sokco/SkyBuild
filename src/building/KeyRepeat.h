#pragma once
#include <cmath>
namespace SkyBuild {
// Driven by Skyrim's per-frame held-button events; never catches up in bursts.
class KeyRepeat {
  static constexpr float initialDelay = .5F;
  float next = initialDelay;

public:
  bool Pulse(bool down, bool pressed, float held) {
    if (!pressed) {
      next = initialDelay;
      return false;
    }
    if (down) {
      next = initialDelay;
      return true;
    }
    if (!std::isfinite(held) || held < next)
      return false;
    next = held + .065F;
    return true;
  }
};
} // namespace SkyBuild
