#pragma once
#include "navmesh/generation/AuthoredTopology.h"
namespace SkyBuild::Navmesh {
struct IndexRemap {
  // Complete old-to-new permutations. Empty means preserve existing indices.
  std::vector<std::uint16_t> vertices, triangles;
};
struct RemappedTopology {
  std::map<std::uint32_t, AuthoredMesh> meshes;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
// Detached, all-or-nothing graph remapping. Retains directed portals and opaque
// traversal words. Does not register forms, publish buffers or modify
// originals. Index-changing edits with native overlap/special-link dependencies
// are rejected until their auxiliary native records have a verified remapping
// implementation.
[[nodiscard]] RemappedTopology
RemapAuthoredIndices(const std::map<std::uint32_t, AuthoredMesh> &originals,
                     const std::map<std::uint32_t, IndexRemap> &changes);
} // namespace SkyBuild::Navmesh
