#pragma once
#include "navmesh/generation/AuthoredTopology.h"
namespace SkyBuild::Navmesh {
// Full graph materialization is stricter than the engine's worker copy30245.
// Keep every owned topology field visible rather than accepting count equality.
inline std::vector<std::string> WorkCopyDifferences(const AuthoredMesh &source,
                                                    const AuthoredMesh &work) {
  std::vector<std::string> differences;
  if (source.formID != work.formID)
    differences.emplace_back("identity");
  if (source.nativeDependencies != work.nativeDependencies)
    differences.emplace_back("cut-dependencies");
  if (source.vertices != work.vertices)
    differences.emplace_back("vertices");
  if (source.triangles != work.triangles)
    differences.emplace_back("triangles");
  if (source.portals != work.portals)
    differences.emplace_back("portals");
  if (source.coverTriangles != work.coverTriangles)
    differences.emplace_back("cover");
  if (source.doorTriangles != work.doorTriangles)
    differences.emplace_back("door-portals");
  if (source.closedDoorTriangles != work.closedDoorTriangles)
    differences.emplace_back("closed-doors");
  return differences;
}
} // namespace SkyBuild::Navmesh
