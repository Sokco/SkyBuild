#pragma once
#include <array>
#include <cmath>
#include <algorithm>
namespace SkyBuild::FineAdjustmentMath {
using Vector = std::array<float, 3>;
inline Vector CardinalForward(float x, float y, Vector previous = {0, 1, 0}) {
  if (x * x + y * y < 0.000001F)
    return previous;
  return std::abs(x) > std::abs(y) ? Vector{x >= 0 ? 1.0F : -1.0F, 0, 0}
                                   : Vector{0, y >= 0 ? 1.0F : -1.0F, 0};
}
inline Vector Right(const Vector &forward) {
  return {forward[1], -forward[0], 0};
}
inline Vector PlacementForward(float x, float y, float yaw,
                               Vector previous = {0, 1, 0}) {
  if (x * x + y * y < 0.000001F)
    return previous;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const auto local = CardinalForward(x * c - y * s, x * s + y * c);
  return {local[0] * c + local[1] * s, -local[0] * s + local[1] * c, 0};
}
// Post-multiply so each increment follows the object's current local axes.
template <class Matrix>
void RotateLocal(Matrix &rotation, int axis, float angle) {
  Matrix increment;
  increment.MakeRotation(angle, axis == 0 ? 1.F : 0.F,
                         axis == 1 ? 1.F : 0.F, axis == 2 ? 1.F : 0.F);
  rotation = rotation * increment;
}
inline float StepScale(float scale, int direction) {
  return std::clamp(std::round((scale + 0.05F * direction) * 20.0F) / 20.0F,
                    0.05F, 10.0F);
}
} // namespace SkyBuild::FineAdjustmentMath
