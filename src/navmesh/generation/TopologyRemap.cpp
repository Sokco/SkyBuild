#include "navmesh/generation/TopologyRemap.h"
#include <algorithm>
namespace SkyBuild::Navmesh {
namespace {
bool Permutation(const std::vector<std::uint16_t> &mapping, std::size_t count) {
  if (mapping.empty())
    return true;
  if (mapping.size() != count)
    return false;
  std::vector<bool> seen(count);
  for (auto index : mapping) {
    if (index >= count || seen[index])
      return false;
    seen[index] = true;
  }
  return true;
}
std::uint16_t Index(const std::vector<std::uint16_t> &mapping,
                    std::uint16_t index) {
  return mapping.empty() ? index : mapping[index];
}
bool Changed(const std::vector<std::uint16_t> &mapping) {
  for (std::size_t i = 0; i < mapping.size(); ++i)
    if (mapping[i] != i)
      return true;
  return false;
}
} // namespace
RemappedTopology
RemapAuthoredIndices(const std::map<std::uint32_t, AuthoredMesh> &originals,
                     const std::map<std::uint32_t, IndexRemap> &changes) {
  RemappedTopology result;
  const auto audit = AuditAuthoredTopology(originals);
  if (!audit.Valid()) {
    result.error = "Invalid original topology: " + audit.firstError;
    return result;
  }
  bool changed = false;
  for (const auto &[id, mapping] : changes) {
    const auto source = originals.find(id);
    if (source == originals.end() ||
        !Permutation(mapping.vertices, source->second.vertices.size()) ||
        !Permutation(mapping.triangles, source->second.triangles.size())) {
      result.error = "Unknown mesh or incomplete/non-bijective index mapping";
      return result;
    }
    changed |= Changed(mapping.vertices) || Changed(mapping.triangles);
  }
  // Overlap and special-link data can carry indices outside this owned graph.
  // A graph-level gate also prevents changing the destination of an unchanged
  // mesh's native undo/special link records.
  const bool nativeDependencies =
      std::ranges::any_of(originals, [](const auto &entry) {
        return entry.second.nativeDependencies;
      });
  if (changed && (audit.overlappingTriangles || audit.specialLinks ||
                  nativeDependencies)) {
    result.error = "Native navcut or special-link auxiliary remapping required";
    return result;
  }
  const IndexRemap identity;
  auto mappingFor = [&](std::uint32_t id) -> const IndexRemap & {
    const auto found = changes.find(id);
    return found == changes.end() ? identity : found->second;
  };
  auto staged = originals;
  for (const auto &[id, original] : originals) {
    auto &output = staged.at(id);
    const auto &mapping = mappingFor(id);
    for (std::size_t i = 0; i < original.vertices.size(); ++i)
      output.vertices[Index(mapping.vertices, static_cast<std::uint16_t>(i))] =
          original.vertices[i];
    for (std::size_t i = 0; i < original.triangles.size(); ++i) {
      auto triangle = original.triangles[i];
      for (unsigned edge = 0; edge < 3; ++edge) {
        triangle.vertices[edge] =
            Index(mapping.vertices, triangle.vertices[edge]);
        if (!(triangle.flags & (1U << edge)) &&
            triangle.edges[edge] != 0xFFFF) {
          // Retained inactive triangles can have obsolete local references.
          // Preserve their raw data on identity copies; never index unchecked.
          if (triangle.edges[edge] >= original.triangles.size()) {
            if (changed) {
              result.error = "Inactive triangle has unmappable local reference";
              return result;
            }
          } else {
            triangle.edges[edge] =
                Index(mapping.triangles, triangle.edges[edge]);
          }
        }
      }
      output
          .triangles[Index(mapping.triangles, static_cast<std::uint16_t>(i))] =
          triangle;
    }
    for (auto &portal : output.portals) {
      // Special payloads are opaque. Never interpret them as normal
      // destinations.
      if (portal.type != 0)
        continue;
      const auto destination = originals.find(portal.targetMesh);
      if (destination == originals.end()) {
        if (changed) {
          result.error = "Unloaded portal destination prevents graph remapping";
          return result;
        }
        continue;
      }
      if (portal.targetTriangle >= destination->second.triangles.size()) {
        if (changed) {
          result.error = "Unused portal record has unmappable destination";
          return result;
        }
        continue;
      }
      portal.targetTriangle =
          Index(mappingFor(portal.targetMesh).triangles, portal.targetTriangle);
    }
    for (auto &index : output.coverTriangles)
      index = Index(mapping.triangles, index);
    for (auto &index : output.doorTriangles)
      index = Index(mapping.triangles, index);
    for (auto &index : output.closedDoorTriangles)
      index = Index(mapping.triangles, index);
  }
  const auto remappedAudit = AuditAuthoredTopology(staged);
  if (!remappedAudit.Valid() ||
      remappedAudit.reciprocalPortals != audit.reciprocalPortals ||
      remappedAudit.asymmetricPortals != audit.asymmetricPortals) {
    result.error = "Remapped graph failed connectivity validation";
    return result;
  }
  result.meshes = std::move(staged);
  return result;
}
} // namespace SkyBuild::Navmesh
