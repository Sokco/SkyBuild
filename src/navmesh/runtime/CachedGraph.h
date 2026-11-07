#pragma once
#include "navmesh/generation/SurfaceOwnership.h"
#include <map>
#include <set>
namespace SkyBuild::Navmesh {
struct CachedGraph {
  Mesh mesh;
  SurfaceOwnership ownership;
  std::vector<std::uint32_t> islandCells;
  std::set<std::uint32_t> cells;
  [[nodiscard]] bool Valid() const { return ownership.Valid(); }
};
// Preserve per-cell ownership. Matching sampled edges on shared exterior cell
// planes become reciprocal links; authored borders use fresh native metadata.
[[nodiscard]] CachedGraph AssembleCachedGraph(
    const std::map<std::uint32_t, Mesh> &cells,
    const std::map<std::uint32_t, std::size_t> &sourceCounts,
    const std::map<std::uint32_t, BuildInput::Bounds> &domains = {});
} // namespace SkyBuild::Navmesh
