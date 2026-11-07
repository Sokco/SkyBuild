#pragma once
#include "../../../extern/ImAnim/Easing.h"
#include <algorithm>
#include <cmath>
namespace SkyBuild::UI {
// One bounded track per strip. Retargeting starts from the current position.
class StripAnimation {
  float current{}, start{}, target{}, elapsed{};
  bool initialized{};

public:
  float Update(float destination, float dt, bool reset = false) {
    if (!initialized || reset) {
      initialized = true;
      current = start = target = destination;
      elapsed = duration;
    }
    if (destination != target) {
      start = current;
      target = destination;
      elapsed = 0;
    }
    elapsed = std::min(duration,
                       elapsed + (std::isfinite(dt) ? std::max(0.F, dt) : 0.F));
    current = start + (target - start) * ImAnim::OutCubic(elapsed / duration);
    return current;
  }
  static constexpr float duration = .22F;
};
} // namespace SkyBuild::UI
