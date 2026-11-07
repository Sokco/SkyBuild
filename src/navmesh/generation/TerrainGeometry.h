#pragma once
#include "navmesh/generation/MeshGeometry.h"
#include "terrain/TerrainNavigation.h"
namespace SkyBuild::Navmesh {
inline BuildInput::Bounds TerrainBounds(const Terrain::NavigationCell &cell) {
  const auto p = cell.volume.origin;
  return {p.x, p.y, p.x + Terrain::CellSize, p.y + Terrain::CellSize};
}
inline std::vector<BuildInput::Bounds>
TerrainFootprints(const std::vector<Terrain::NavigationCell> &cells,
                  float margin) {
  std::vector<BuildInput::Bounds> result;
  for (const auto &cell : cells)
    for (const auto &surface : cell.surfaces)
      if (surface) {
        const auto &mesh = surface->Geometry();
        for (const auto &triangle : mesh.triangles) {
          const auto a = mesh.vertices[triangle[0]].position;
          const auto b = mesh.vertices[triangle[1]].position;
          const auto c = mesh.vertices[triangle[2]].position;
          // Ignore converted-but-unchanged ground and repeated brush history.
          // Include full changed triangles, including walls and cave ceilings.
          if (std::abs(a.z - cell.volume.Height(a.x, a.y)) <= .01F &&
              std::abs(b.z - cell.volume.Height(b.x, b.y)) <= .01F &&
              std::abs(c.z - cell.volume.Height(c.x, c.y)) <= .01F)
            continue;
          result.push_back({std::min({a.x, b.x, c.x}) - margin,
                            std::min({a.y, b.y, c.y}) - margin,
                            std::max({a.x, b.x, c.x}) + margin,
                            std::max({a.y, b.y, c.y}) + margin});
        }
      }
  return result;
}
// Authored navigation approximates collision within the engine's step height.
// Remove that old LAND band, retaining elevated bridges and underground floors.
// Clipping preserves portions outside the converted cell and vertical band.
inline std::vector<Geometry::Polygon>
TerrainGroundRemainder(Geometry::Polygon polygon,
                       const Terrain::NavigationCell &cell) {
  const auto bounds = TerrainBounds(cell);
  auto result = Geometry::Subtract(polygon, bounds, {});
  for (const auto &[axis, edge, positive] :
       std::array<std::tuple<int, float, bool>, 4>{{{0, bounds.left, true},
                                                    {0, bounds.right, false},
                                                    {1, bounds.bottom, true},
                                                    {1, bounds.top, false}}})
    polygon = Geometry::Clip(polygon, axis, edge, positive);
  if (!Geometry::HasArea(polygon))
    return result;
  const auto clip = [&](const Geometry::Polygon &input, float edge,
                        bool above) {
    Geometry::Polygon out;
    if (input.empty())
      return out;
    const auto distance = [&](Point p) {
      return p.z - cell.volume.Height(p.x, p.y) - edge;
    };
    auto previous = input.back();
    auto d0 = distance(previous);
    for (const auto current : input) {
      const auto d1 = distance(current);
      const bool in0 = above ? d0 >= 0 : d0 <= 0;
      const bool in1 = above ? d1 >= 0 : d1 <= 0;
      if (in0 != in1)
        out.push_back(Geometry::Mix(previous, current, d0 / (d0 - d1)));
      if (in1)
        out.push_back(current);
      previous = current;
      d0 = d1;
    }
    return out;
  };
  for (const auto &[edge, above] :
       std::array<std::pair<float, bool>, 2>{{{32, true}, {-32, false}}}) {
    auto retained = clip(polygon, edge, above);
    if (Geometry::HasArea(retained))
      result.push_back(std::move(retained));
  }
  return result;
}
} // namespace SkyBuild::Navmesh
