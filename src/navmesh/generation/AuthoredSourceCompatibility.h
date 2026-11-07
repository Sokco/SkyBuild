#pragma once
#include "navmesh/generation/AuthoredTopology.h"
#include <set>
namespace SkyBuild::Navmesh {
// Geometry-only observation for native append-based cuts. This deliberately
// does not interpret cut state or authorize source metadata/publication. A
// matching prefix proves only that recorded vertices and face surfaces remain
// present in this owned snapshot; flags, portals and bindings are separate.
struct AuthoredGeometryPrefix {
  std::string error;
  std::size_t changedFlags{}, changedEdges{}, appendedVertices{},
      appendedFaces{};
  [[nodiscard]] bool Matches() const { return error.empty(); }
};
inline AuthoredGeometryPrefix
CompareAuthoredGeometryPrefix(const AuthoredMesh &record,
                              const AuthoredMesh &live) {
  AuthoredGeometryPrefix result;
  const auto fail = [&](const char *reason) {
    result.error = reason;
    return result;
  };
  if (record.formID != live.formID ||
      live.vertices.size() < record.vertices.size() ||
      live.triangles.size() < record.triangles.size())
    return fail("Missing recorded source geometry");
  for (std::size_t i = 0; i < record.vertices.size(); ++i)
    if (record.vertices[i] != live.vertices[i])
      return fail("Recorded source vertex changed");
  for (std::size_t i = 0; i < record.triangles.size(); ++i) {
    const auto &a = record.triangles[i], &b = live.triangles[i];
    if (a.vertices != b.vertices || a.traversal != b.traversal)
      return fail("Recorded source face surface changed");
    result.changedFlags += a.flags != b.flags;
    result.changedEdges += a.edges != b.edges;
  }
  result.appendedVertices = live.vertices.size() - record.vertices.size();
  result.appendedFaces = live.triangles.size() - record.triangles.size();
  return result;
}
struct AuthoredSourceCompatibility {
  std::string error;
  bool doorBindingsChanged{};
  std::set<std::uint16_t> closedDoorFaces;
  bool GeometryCompatible() const { return error.empty(); }
};
// Compare authored geometry with an independently normalized live source.
// 91167 sets triangle flag1000 for faces it records in closedDoors;91168 clears
// it while removing that overlay. These are runtime annotations, not authored
// geometry. Every allowed difference needs that explicit ownership witness.
// Door bindings remain a separate native obligation, never silently copied from
// plugin geometry or considered validated by this geometry comparison.
inline AuthoredSourceCompatibility
CompareAuthoredSource(const AuthoredMesh &record, const AuthoredMesh &live) {
  AuthoredSourceCompatibility result;
  auto fail = [&](const char *reason) {
    result.error = reason;
    return result;
  };
  if (record.formID != live.formID || record.vertices != live.vertices ||
      record.triangles.size() != live.triangles.size())
    return fail("Authored/live geometry differs");
  if (record.coverTriangles != live.coverTriangles)
    return fail("Authored/live cover metadata differs");
  result.doorBindingsChanged =
      record.doorTriangles != live.doorTriangles ||
      record.closedDoorTriangles != live.closedDoorTriangles;
  for (auto face : live.closedDoorTriangles) {
    if (face >= live.triangles.size())
      return fail("Runtime closed-door face is out of range");
    result.closedDoorFaces.insert(face);
  }
  for (std::size_t index = 0; index < record.triangles.size(); ++index) {
    const auto &a = record.triangles[index], &b = live.triangles[index];
    if (a.vertices != b.vertices || a.traversal != b.traversal)
      return fail("Authored/live triangle surface differs");
    const auto expectedFlags = static_cast<std::uint16_t>(
        a.flags |
        (result.closedDoorFaces.contains(static_cast<std::uint16_t>(index))
             ? 0x1000U
             : 0U));
    if (b.flags != expectedFlags)
      return fail("Unaccounted runtime triangle flags");
    for (unsigned side = 0; side < 3; ++side) {
      if (a.flags & (1U << side)) {
        if (a.edges[side] >= record.portals.size() ||
            b.edges[side] >= live.portals.size() ||
            record.portals[a.edges[side]] != live.portals[b.edges[side]])
          return fail("Authored/live portal connectivity differs");
      } else if (a.edges[side] != b.edges[side])
        return fail("Authored/live local connectivity differs");
    }
  }
  return result;
}
} // namespace SkyBuild::Navmesh
