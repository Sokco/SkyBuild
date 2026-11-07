#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace SkyBuild::PlacementRotation {
// Inverse of NiMatrix3::SetEulerAnglesXYZ. Use the standard atan2 rather than
// CommonLib's NiFastATan2, whose approximation reverses angles in some octants.
template <class Matrix, class Vector>
void ToEulerXYZ(const Matrix &matrix, Vector &angles) {
  const auto x = matrix.GetVectorX();
  const auto y = matrix.GetVectorY();
  const auto z = matrix.GetVectorZ();
  const float cy = std::hypot(x.x, y.x);
  angles.y = std::atan2(-z.x, cy);
  if (cy > .000001F) {
    angles.x = std::atan2(z.y, z.z);
    angles.z = std::atan2(y.x, x.x);
  } else {
    angles.x = std::atan2(std::copysign(1.F, -z.x) * x.y, y.y);
    angles.z = 0;
  }
}
inline float FacingYaw(float x, float y, float fallback) {
  return x * x + y * y > .0001F ? std::atan2(x, y) : fallback;
}
// Extract Y-X-Z factors: global roll(Y), then pitch(X) and yaw(Z) in
// the roll-defined frame. Components retain the usual x=pitch,y=roll,z=yaw.
template <class Matrix, class Vector>
void ToRollFrame(const Matrix &matrix, Vector &angles) {
  const auto x = matrix.GetVectorX(), y = matrix.GetVectorY(),
             z = matrix.GetVectorZ();
  const float cx = std::hypot(x.y, y.y);
  angles.x = std::atan2(z.y, cx);
  if (cx > .000001F) {
    angles.y = std::atan2(-z.x, z.z);
    angles.z = std::atan2(-x.y, y.y);
  } else {
    angles.y = std::atan2(x.z, x.x);
    angles.z = 0.F;
  }
}
inline float PropBaseYaw(float nx, float ny, float nz, float previous) {
  return std::abs(nz) < .5F ? FacingYaw(nx, ny, previous) : previous;
}
inline float Step(bool snapped, bool side, bool frozen) {
  return !snapped || frozen ? std::numbers::pi_v<float> / 12.F
         : side             ? std::numbers::pi_v<float>
                            : std::numbers::pi_v<float> * .5F;
}
inline float Advance(float offset, float step) {
  return std::remainder(offset + step, 2.F * std::numbers::pi_v<float>);
}
} // namespace SkyBuild::PlacementRotation
