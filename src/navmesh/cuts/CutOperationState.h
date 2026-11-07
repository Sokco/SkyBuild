#pragma once
#include "navmesh/generation/AuthoredTopology.h"
namespace SkyBuild::Navmesh {
// Native93318 clears 0x100 on affected faces and sets it on their neighbors
// while collecting edge state (1170: 122F6A9/122F7C2). Even a failed 93310
// operation can leave those markers changed. They are not surface geometry.
// Only this operation-level comparison excludes them; raw captures and native
// storage retain every bit. Cut ownership is checked independently and exactly.
inline bool
SameCutOperationGeometry(const std::map<std::uint32_t, AuthoredMesh> &before,
                         const std::map<std::uint32_t, AuthoredMesh> &after) {
  if (before.size() != after.size())
    return false;
  for (const auto &[id, a] : before) {
    const auto found = after.find(id);
    if (found == after.end())
      return false;
    const auto &b = found->second;
    if (a.formID != b.formID || a.nativeDependencies != b.nativeDependencies ||
        a.vertices != b.vertices || a.portals != b.portals ||
        a.coverTriangles != b.coverTriangles ||
        a.doorTriangles != b.doorTriangles ||
        a.closedDoorTriangles != b.closedDoorTriangles ||
        a.triangles.size() != b.triangles.size())
      return false;
    for (std::size_t i = 0; i < a.triangles.size(); ++i) {
      const auto &x = a.triangles[i], &y = b.triangles[i];
      if (x.vertices != y.vertices || x.edges != y.edges ||
          x.traversal != y.traversal || ((x.flags ^ y.flags) & ~0x100U))
        return false;
    }
  }
  return true;
}
} // namespace SkyBuild::Navmesh
