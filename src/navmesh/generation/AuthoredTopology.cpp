#include "navmesh/generation/AuthoredTopology.h"
#include <cmath>
#include <sstream>

namespace SkyBuild::Navmesh {
bool SameAuthoredSurface(const AuthoredMesh &a, const AuthoredMesh &b) {
  if (a.formID != b.formID || a.vertices != b.vertices ||
      a.triangles.size() != b.triangles.size() ||
      a.coverTriangles != b.coverTriangles ||
      a.doorTriangles != b.doorTriangles ||
      a.closedDoorTriangles != b.closedDoorTriangles)
    return false;
  for (std::size_t i = 0; i < a.triangles.size(); ++i) {
    const auto &x = a.triangles[i], &y = b.triangles[i];
    if (x.vertices != y.vertices || x.flags != y.flags ||
        x.traversal != y.traversal)
      return false;
    for (unsigned edge = 0; edge < 3; ++edge) {
      if (x.flags & (1U << edge)) {
        if (x.edges[edge] >= a.portals.size() ||
            y.edges[edge] >= b.portals.size() ||
            a.portals[x.edges[edge]] != b.portals[y.edges[edge]])
          return false;
      } else if (x.edges[edge] != y.edges[edge])
        return false;
    }
  }
  return true;
}
std::vector<std::uint16_t>
BuildAuthoredCover(const std::vector<AuthoredTriangle> &triangles) {
  std::vector<std::uint16_t> cover;
  if (triangles.size() > 65535)
    return cover;
  for (std::size_t i = 0; i < triangles.size(); ++i) {
    const auto &face = triangles[i];
    const auto combined = static_cast<std::uint32_t>(face.flags) |
                          (static_cast<std::uint32_t>(face.traversal) << 16U);
    if (!(face.flags & 32U) && (combined & 0x0FBE0000U))
      cover.push_back(static_cast<std::uint16_t>(i));
  }
  return cover;
}
TopologyAudit
AuditAuthoredTopology(const std::map<std::uint32_t, AuthoredMesh> &meshes) {
  TopologyAudit result;
  auto reject = [&](std::uint32_t id, const char *reason) {
    ++result.errors;
    ++result.errorCounts[reason];
    if (result.firstError.empty()) {
      std::ostringstream message;
      message << std::hex << id << ": " << reason;
      result.firstError = message.str();
    }
  };
  for (const auto &[id, mesh] : meshes) {
    if (id != mesh.formID || mesh.vertices.size() > 65535 ||
        mesh.triangles.size() > 65535)
      reject(id, "invalid mesh identity or index range");
    for (const auto &p : mesh.vertices)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        reject(id, "non-finite vertex");
    for (const auto cover : mesh.coverTriangles) {
      ++result.coverRecords;
      if (cover >= mesh.triangles.size())
        reject(id, "cover triangle out of range");
    }
    for (auto index : mesh.doorTriangles)
      if (index >= mesh.triangles.size())
        reject(id, "door triangle out of range");
    for (auto index : mesh.closedDoorTriangles)
      if (index >= mesh.triangles.size())
        reject(id, "closed-door triangle out of range");
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
      const auto &triangle = mesh.triangles[index];
      for (auto vertex : triangle.vertices)
        if (vertex >= mesh.vertices.size())
          reject(id, "triangle vertex out of range");
      // Native deletion can leave obsolete edge records in an unused triangle.
      if (triangle.flags & 8U)
        continue;
      // Native location lookup excludes overlapping triangles. Navcuts can
      // retain their old adjacency while redirecting active replacement faces.
      if (triangle.flags & 32U) {
        ++result.overlappingTriangles;
        continue;
      }
      for (unsigned edge = 0; edge < 3; ++edge) {
        const auto link = triangle.edges[edge];
        if (!(triangle.flags & (1U << edge))) {
          if (link == 0xFFFF)
            continue;
          ++result.localEdges;
          if (link >= mesh.triangles.size()) {
            reject(id, "local triangle out of range");
            continue;
          }
          const auto &other = mesh.triangles[link];
          bool reciprocal = false;
          for (unsigned e = 0; e < 3; ++e)
            reciprocal |=
                !(other.flags & (1U << e)) && other.edges[e] == index &&
                other.vertices[e] == triangle.vertices[(edge + 1) % 3] &&
                other.vertices[(e + 1) % 3] == triangle.vertices[edge];
          if (!reciprocal || (other.flags & 8U))
            reject(id, "local edge is not reciprocal");
          continue;
        }
        ++result.externalEdges;
        if (link >= mesh.portals.size()) {
          reject(id, "extra-edge record out of range");
          continue;
        }
        const auto &portal = mesh.portals[link];
        if (portal.type != 0) {
          ++result.specialLinks;
          continue; // Ledges and enable/disable links have different semantics.
        }
        const auto target = meshes.find(portal.targetMesh);
        if (target == meshes.end()) {
          ++result.unloadedTargets;
          continue;
        }
        if (portal.targetTriangle >= target->second.triangles.size()) {
          reject(id, "portal destination out of range");
          continue;
        }
        const auto &other = target->second.triangles[portal.targetTriangle];
        // Serialized portal destinations contain no edge index. The bytes
        // after targetTriangle are padding, despite the CommonLib declaration.
        // Find the return edge from the destination triangle's actual links.
        unsigned matching = 0;
        if (!(other.flags & 8U))
          for (unsigned e = 0; e < 3; ++e) {
            const auto backIndex = other.edges[e];
            if (!(other.flags & (1U << e)) ||
                backIndex >= target->second.portals.size())
              continue;
            const auto &back = target->second.portals[backIndex];
            matching += back.type == 0 && back.targetMesh == id &&
                        back.targetTriangle == index;
          }
        if (matching == 1)
          ++result.reciprocalPortals;
        else if (matching > 1)
          reject(id, "ambiguous portal return edges");
        else
          // Authored directed links and native navcuts can lack a return edge.
          // Preserve/report them separately; they must not be silently repaired
          // or treated as ordinary reciprocal topology by the future remapper.
          ++result.asymmetricPortals;
      }
    }
  }
  return result;
}
} // namespace SkyBuild::Navmesh
