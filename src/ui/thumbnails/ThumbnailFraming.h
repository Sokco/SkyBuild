#pragma once
#include <algorithm>
#include <limits>
namespace SkyBuild::UI::ThumbnailFraming {
struct Fit {
  float distance, horizontalOffset, verticalOffset;
};
class Constraints {
  float horizontalSlope, verticalSlope;
  float left = std::numeric_limits<float>::lowest(),
        right = std::numeric_limits<float>::max();
  float bottom = std::numeric_limits<float>::lowest(),
        top = std::numeric_limits<float>::max();
  float minimumDepth = std::numeric_limits<float>::max();

public:
  Constraints(float horizontal, float vertical)
      : horizontalSlope(horizontal), verticalSlope(vertical) {}
  void Add(float x, float y, float depth) {
    left = std::max(left, x - horizontalSlope * depth);
    right = std::min(right, x + horizontalSlope * depth);
    bottom = std::max(bottom, y - verticalSlope * depth);
    top = std::min(top, y + verticalSlope * depth);
    minimumDepth = std::min(minimumDepth, depth);
  }
  Fit Solve(float nearDepth) const {
    return {std::max({(left - right) / (2 * horizontalSlope),
                      (bottom - top) / (2 * verticalSlope),
                      nearDepth - minimumDepth}),
            (left + right) * 0.5F, (bottom + top) * 0.5F};
  }
};
} // namespace SkyBuild::UI::ThumbnailFraming
