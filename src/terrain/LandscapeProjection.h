#pragma once
#include "terrain/TerrainVolume.h"

namespace SkyBuild::Terrain {
enum class LandscapeAxis : std::uint8_t { Top, X, Y };
struct LandscapeCharts {
  Mesh mesh;
  std::vector<LandscapeAxis> axes;
};
// Choose one projection per triangle. Shared vertices must split at chart
// boundaries; interpolating unrelated UV projections across a face collapses
// the mapping and gives the parallax shader an incorrect tangent frame.
inline LandscapeCharts ProjectLandscape(const Mesh &source) {
  LandscapeCharts result;
  result.mesh.triangles.reserve(source.triangles.size());
  result.mesh.vertices.reserve(source.vertices.size());
  std::vector<std::array<std::uint32_t, 3>> remap(
      source.vertices.size(), {UINT32_MAX, UINT32_MAX, UINT32_MAX});
  for (const auto &triangle : source.triangles) {
    for (auto index : triangle)
      if (index >= source.vertices.size())
        throw std::runtime_error("Invalid landscape projection index");
    const auto a = source.vertices[triangle[0]].position;
    const auto n = Unit(Cross(source.vertices[triangle[1]].position - a,
                              source.vertices[triangle[2]].position - a));
    // Leave slopes up to 60 degrees on the original landscape mapping.
    const auto axis = std::abs(n.z) >= .5F             ? LandscapeAxis::Top
                      : std::abs(n.x) >= std::abs(n.y) ? LandscapeAxis::X
                                                       : LandscapeAxis::Y;
    std::array<std::uint16_t, 3> mapped{};
    for (unsigned corner = 0; corner < 3; ++corner) {
      auto &index = remap[triangle[corner]][static_cast<unsigned>(axis)];
      if (index == UINT32_MAX) {
        if (result.mesh.vertices.size() >= 60000)
          throw std::runtime_error("Terrain projection exceeds vertex limit");
        index = static_cast<std::uint32_t>(result.mesh.vertices.size());
        result.mesh.vertices.push_back(source.vertices[triangle[corner]]);
        result.axes.push_back(axis);
      }
      mapped[corner] = static_cast<std::uint16_t>(index);
    }
    result.mesh.triangles.push_back(mapped);
  }
  return result;
}
} // namespace SkyBuild::Terrain
