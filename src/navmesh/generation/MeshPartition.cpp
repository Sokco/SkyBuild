#include "navmesh/generation/MeshPartition.h"
#include "navmesh/generation/MeshGeometry.h"
#include <cmath>
#include <limits>

namespace SkyBuild::Navmesh {
PartitionedMesh
PartitionGeneratedMesh(const Mesh &mesh,
                       const std::vector<std::uint32_t> &owners) {
  PartitionedMesh result;
  auto fail = [&](std::string reason) {
    result.meshes.clear();
    result.triangles.clear();
    result.error = std::move(reason);
    return result;
  };
  if (owners.size() != mesh.triangles.size())
    return fail("Every generated triangle requires explicit native ownership");
  for (const auto &vertex : mesh.vertices)
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
        !std::isfinite(vertex.z))
      return fail("Non-finite generated vertex");
  result.triangles.reserve(mesh.triangles.size());
  std::map<std::uint32_t, std::map<std::uint32_t, std::uint16_t>> vertexIndices;
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto owner = owners[i];
    if (!owner)
      return fail("Unassigned generated surface has no native owner");
    auto &output = result.meshes[owner];
    output.formID = owner;
    if (output.triangles.size() >= 65535)
      return fail("Native partition exceeds triangle index limits");
    const auto &source = mesh.triangles[i];
    if (source.flags & (7U | 8U | 32U))
      return fail(
          "Generated input contains external or inactive triangle flags");
    AuthoredTriangle triangle;
    triangle.flags = source.flags;
    for (unsigned edge = 0; edge < 3; ++edge) {
      const auto vertex = source.vertices[edge];
      if (vertex >= mesh.vertices.size())
        return fail("Invalid generated vertex index");
      auto &indices = vertexIndices[owner];
      auto found = indices.find(vertex);
      if (found == indices.end()) {
        if (output.vertices.size() >= 65535)
          return fail("Native partition exceeds vertex index limits");
        found = indices
                    .emplace(vertex,
                             static_cast<std::uint16_t>(output.vertices.size()))
                    .first;
        output.vertices.push_back(mesh.vertices[vertex]);
      }
      triangle.vertices[edge] = found->second;
    }
    result.triangles.emplace_back(
        owner, static_cast<std::uint16_t>(output.triangles.size()));
    output.triangles.push_back(triangle);
  }
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto [owner, local] = result.triangles[i];
    auto &output = result.meshes.at(owner);
    auto &triangle = output.triangles[local];
    const auto &source = mesh.triangles[i];
    for (unsigned edge = 0; edge < 3; ++edge) {
      const auto neighbor = source.neighbors[edge];
      if (neighbor == -1)
        continue;
      if (neighbor < 0 ||
          static_cast<std::size_t>(neighbor) >= mesh.triangles.size())
        return fail("Invalid generated neighbor index");
      const auto &other = mesh.triangles[neighbor];
      const auto portalEndpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto p = mesh.vertices[a], q = mesh.vertices[b];
        return std::abs(p.x - q.x) <= Geometry::WELD_TOLERANCE &&
               std::abs(p.y - q.y) <= Geometry::WELD_TOLERANCE &&
               std::abs(p.z - q.z) <= Geometry::MAX_STEP;
      };
      const bool crossOwner = owners[i] != owners[neighbor];
      unsigned reverseEdges{};
      for (unsigned reverse = 0; reverse < 3; ++reverse)
        reverseEdges +=
            other.neighbors[reverse] == static_cast<std::int32_t>(i) &&
            ((other.vertices[reverse] == source.vertices[(edge + 1) % 3] &&
              other.vertices[(reverse + 1) % 3] == source.vertices[edge]) ||
             (crossOwner &&
              portalEndpoint(other.vertices[reverse],
                             source.vertices[(edge + 1) % 3]) &&
              portalEndpoint(other.vertices[(reverse + 1) % 3],
                             source.vertices[edge])));
      if (reverseEdges != 1)
        return fail("Generated adjacency is not uniquely reciprocal");
      const auto [otherOwner, otherLocal] = result.triangles[neighbor];
      if (otherOwner == owner) {
        triangle.edges[edge] = otherLocal;
      } else {
        if (output.portals.size() >= 65535)
          return fail("Native partition exceeds portal index limits");
        triangle.flags |= static_cast<std::uint16_t>(1U << edge);
        triangle.edges[edge] =
            static_cast<std::uint16_t>(output.portals.size());
        output.portals.push_back({0, otherOwner, otherLocal});
      }
    }
  }
  const auto audit = AuditAuthoredTopology(result.meshes);
  if (!audit.OrdinaryReciprocalTopology())
    return fail("Partitioned native topology failed validation: " +
                audit.firstError);
  return result;
}
} // namespace SkyBuild::Navmesh
