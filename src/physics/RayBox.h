#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
namespace SkyBuild::Physics {
// Segment parameter remains in world-distance units after transforming both
// origin and direction into a box's local space (do not normalize again).
inline std::optional<float> IntersectBox(const std::array<float, 3> &origin,
                                         const std::array<float, 3> &direction,
                                         const std::array<float, 3> &minimum,
                                         const std::array<float, 3> &maximum,
                                         float distance) {
  if (!std::isfinite(distance) || distance <= 0)
    return std::nullopt;
  float entryDistance = 0, exitDistance = distance;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(origin[axis]) || !std::isfinite(direction[axis]) ||
        !std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
        minimum[axis] > maximum[axis])
      return std::nullopt;
    if (std::abs(direction[axis]) < 1e-8F) {
      if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
        return std::nullopt;
      continue;
    }
    auto a = (minimum[axis] - origin[axis]) / direction[axis];
    auto b = (maximum[axis] - origin[axis]) / direction[axis];
    if (a > b)
      std::swap(a, b);
    entryDistance = std::max(entryDistance, a);
    exitDistance = std::min(exitDistance, b);
    if (entryDistance > exitDistance)
      return std::nullopt;
  }
  return entryDistance;
}
} // namespace SkyBuild::Physics
