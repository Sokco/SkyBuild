#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <map>

namespace SkyBuild::Navmesh {
struct AuthoredTriangle {
  std::array<std::uint16_t, 3> vertices{};
  // Link flag set: extra-edge record index. Otherwise: local triangle index.
  std::array<std::uint16_t, 3> edges{0xFFFF, 0xFFFF, 0xFFFF};
  std::uint16_t flags{}, traversal{};
  bool operator==(const AuthoredTriangle &) const = default;
};
struct AuthoredPortal {
  std::uint32_t type{}, targetMesh{};
  std::uint16_t targetTriangle{};
  // Reserved slot in historical diagnostic files. Native93306/91274 load only
  // type, target form and target face;30266 copies those same ten bytes. The
  // final two native bytes are not portable topology, including special links.
  std::uint16_t auxiliary{};
  bool operator==(const AuthoredPortal &other) const {
    return type == other.type && targetMesh == other.targetMesh &&
           targetTriangle == other.targetTriangle;
  }
};
// Owned, per-form metadata. No engine pointers enter worker code.
struct AuthoredMesh {
  std::uint32_t formID{};
  bool nativeDependencies{};
  std::vector<Point> vertices;
  std::vector<AuthoredTriangle> triangles;
  std::vector<AuthoredPortal> portals;
  std::vector<std::uint16_t> coverTriangles;
  std::vector<std::uint16_t> doorTriangles, closedDoorTriangles;
  bool operator==(const AuthoredMesh &) const = default;
};
struct TopologyAudit {
  std::size_t localEdges{}, externalEdges{}, reciprocalPortals{};
  std::size_t unloadedTargets{}, specialLinks{}, coverRecords{}, errors{};
  std::size_t overlappingTriangles{}, asymmetricPortals{};
  std::string firstError;
  std::map<std::string, std::size_t> errorCounts;
  [[nodiscard]] bool Valid() const { return errors == 0; }
  [[nodiscard]] bool OrdinaryReciprocalTopology() const {
    return Valid() && unloadedTargets == 0 && specialLinks == 0 &&
           asymmetricPortals == 0 && overlappingTriangles == 0;
  }
};
// Missing target meshes are reported separately, never treated as verified.
[[nodiscard]] TopologyAudit
AuditAuthoredTopology(const std::map<std::uint32_t, AuthoredMesh> &meshes);
// Mirrors native builder 91071: cover candidates are selected by traversal
// bits in the combined flag word, excluding overlapping triangles.
[[nodiscard]] std::vector<std::uint16_t>
BuildAuthoredCover(const std::vector<AuthoredTriangle> &triangles);
// Native cutting can compact extra-edge slots without changing surface links.
// Compare referenced portal values rather than their transient array indices.
[[nodiscard]] bool SameAuthoredSurface(const AuthoredMesh &a,
                                       const AuthoredMesh &b);
} // namespace SkyBuild::Navmesh
