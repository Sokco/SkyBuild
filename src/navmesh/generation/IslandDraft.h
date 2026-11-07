#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <map>
#include <stdexcept>
namespace SkyBuild::Navmesh {
inline Mesh ExtractIslandDraft(const Mesh &mesh,
                               const std::vector<std::uint32_t> &component) {
  Mesh output;
  std::map<std::uint32_t, std::uint32_t> triangles, vertices;
  for (std::uint32_t i = 0; i < component.size(); ++i)
    triangles.emplace(component[i], i);
  for (const auto index : component) {
    auto triangle = mesh.triangles.at(index);
    if (triangle.source.Authored())
      throw std::runtime_error("Island contains authored geometry");
    for (auto &vertex : triangle.vertices) {
      auto found = vertices.find(vertex);
      if (found == vertices.end()) {
        found = vertices
                    .emplace(vertex,
                             static_cast<std::uint32_t>(output.vertices.size()))
                    .first;
        output.vertices.push_back(mesh.vertices.at(vertex));
      }
      vertex = found->second;
    }
    for (auto &neighbor : triangle.neighbors)
      if (neighbor >= 0) {
        // Cross-cell neighbors remain in the complete graph. An isolated
        // staging form contains only local indices; partitioning creates
        // portals.
        const auto local = triangles.find(static_cast<std::uint32_t>(neighbor));
        neighbor = local == triangles.end()
                       ? -1
                       : static_cast<std::int32_t>(local->second);
      }
    output.triangles.push_back(triangle);
  }
  return output;
}
} // namespace SkyBuild::Navmesh
