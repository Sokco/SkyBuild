#pragma once
#include "navmesh/generation/AuthoredTopology.h"

namespace SkyBuild::Navmesh {
struct DeferredPortal {
  std::uint32_t sourceMesh{};
  std::uint16_t sourceTriangle{};
  unsigned sourceSide{};
  AuthoredPortal destination;
  Point a, b;
};
struct DoorRecordSources {
  std::vector<std::uint16_t> doors, closedDoors;
};
struct PartitionedMesh {
  // Each output door record explicitly names its original native record.
  // A subdivided unresolved placeholder can retain all authored descendants.
  std::map<std::uint32_t, DoorRecordSources> doorRecordSources;
  std::map<std::uint32_t, AuthoredMesh> meshes;
  // Generated triangle -> owning native form/local triangle. Explicit mapping
  // is required: sampled surfaces cannot silently inherit an arbitrary form.
  std::vector<std::pair<std::uint32_t, std::uint16_t>> triangles;
  // A boundary split can create multiple native faces for one generated face.
  // Empty means no splits; otherwise entries include the first face above.
  std::vector<std::vector<std::pair<std::uint32_t, std::uint16_t>>>
      triangleFragments;
  // Owned descriptors only. Native edges stay closed until a fresh loaded
  // neighbor permits reconstruction; these are never native pointer bindings.
  std::vector<DeferredPortal> deferredPortals;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
// Converts local generated adjacency into per-form local adjacency and ordinary
// reciprocal portals. Ownership is supplied by the caller. Does not restore
// authored doors/cover/undo records or publish anything into the engine.
[[nodiscard]] PartitionedMesh
PartitionGeneratedMesh(const Mesh &mesh,
                       const std::vector<std::uint32_t> &owners);
} // namespace SkyBuild::Navmesh
