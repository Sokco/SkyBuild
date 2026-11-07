#pragma once
#include "terrain/TerrainVolume.h"
#include <array>
#include <optional>
namespace SkyBuild::Terrain::BrushOverlay {
// BSGraphics projection matrices use row-vector DirectX convention. Compare
// two view-space distances, accounting for both left- and right-handed views.
// Reject UI/orthographic/invalid matrices instead of guessing a convention.
inline std::optional<bool> ReversedDepth(const float projection[4][4],
                                         float nearPlane, float farPlane) {
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      if (!std::isfinite(projection[row][column]))
        return {};
  const double wScale = projection[2][3];
  if (std::abs(wScale) < 1e-6 || std::abs(projection[3][3]) > 1e-5 ||
      std::abs(projection[0][0]) < 1e-6 || std::abs(projection[1][1]) < 1e-6 ||
      !std::isfinite(nearPlane) || nearPlane <= 0 || !std::isfinite(farPlane) ||
      farPlane <= 4 * nearPlane)
    return {};
  const double first = double(nearPlane) * 2;
  const double second = std::min(double(farPlane) * .5, first * 16);
  const auto depth = [&](double distance) {
    const double z = std::copysign(distance, wScale);
    return (z * projection[2][2] + projection[3][2]) /
           (z * wScale + projection[3][3]);
  };
  const double closeDepth = depth(first), distantDepth = depth(second);
  if (!std::isfinite(closeDepth) || !std::isfinite(distantDepth) ||
      closeDepth < -.01 || closeDepth > 1.01 || distantDepth < -.01 ||
      distantDepth > 1.01 || std::abs(closeDepth - distantDepth) < 1e-7)
    return {};
  return distantDepth < closeDepth;
}
// NiCamera's CPU projection is forward-Z. Adapt only its Z row when the
// active rendering projection reports reversed depth; XY/W remain unchanged.
inline void SceneDepthProjection(const float camera[4][4], float scene[4][4],
                                 bool reversed) {
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      scene[row][column] = reversed && row == 2
                               ? camera[3][column] - camera[2][column]
                               : camera[row][column];
}
inline bool Intersects(const std::array<Vec, 3> &triangle, const Brush &brush) {
  // Conservative sphere/AABB rejection; the pixel shader does the exact mask.
  auto lo = triangle[0], hi = lo;
  for (auto p : triangle) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const Vec nearest{std::clamp(brush.center.x, lo.x, hi.x),
                    std::clamp(brush.center.y, lo.y, hi.y),
                    std::clamp(brush.center.z, lo.z, hi.z)};
  return Length(nearest - brush.center) <= brush.radius;
}
} // namespace SkyBuild::Terrain::BrushOverlay
