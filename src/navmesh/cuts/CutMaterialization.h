#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace SkyBuild::Navmesh {
// A record is shared across native geometry copies. Its status is not the
// materialization of its effects in a particular mesh. This observation model
// deliberately has no publication/capture-coherence authority.
enum class CutMaterialization { Applied, Removed, Unresolved };
struct CutOwnershipWitness {
  std::uint32_t triangle{};
  std::uint64_t
      dataIdentity{}; // Opaque capture-local token, never dereferenced.
  bool operator==(const CutOwnershipWitness &) const = default;
};
struct CutMeshObservation {
  std::uint32_t rawStatus{};
  std::uint32_t originalVertices{}, addedVertices{};
  std::uint32_t originalTriangles{}, addedTriangles{};
  std::uint32_t vertexCount{};
  std::uint64_t dataIdentity{};
  std::span<const std::uint16_t> triangleFlags;
  std::span<const CutOwnershipWitness> ownership;
};
struct CutMaterializationResult {
  CutMaterialization state{CutMaterialization::Unresolved};
  std::string reason;
};
// Requires complete per-mesh ownership evidence. Does not reinterpret a raw
// status to recover missing payload. Ambiguous/nested ownership remains
// unresolved until the complete stack can be normalized as one observation.
[[nodiscard]] inline CutMaterializationResult
ObserveCutMaterialization(const CutMeshObservation &observation) {
  auto unknown = [](const char *reason) {
    return CutMaterializationResult{CutMaterialization::Unresolved, reason};
  };
  if (observation.rawStatus != 1 && observation.rawStatus != 3)
    return unknown("Unsupported shared record status");
  if (!observation.dataIdentity)
    return unknown("Cut data identity missing");
  const auto vertexEnd =
      std::uint64_t{observation.originalVertices} + observation.addedVertices;
  const auto triangleEnd =
      std::uint64_t{observation.originalTriangles} + observation.addedTriangles;
  if (!observation.addedTriangles)
    return unknown("Zero-face record needs complete native stack semantics");
  std::size_t owned{};
  std::map<std::uint32_t, std::uint64_t> owners;
  for (const auto &entry : observation.ownership) {
    if (!entry.dataIdentity ||
        entry.triangle >= observation.triangleFlags.size())
      return unknown("Invalid mesh-local ownership witness");
    if (!owners.emplace(entry.triangle, entry.dataIdentity).second)
      return unknown("Repeated mesh-local ownership key");
    if (entry.dataIdentity == observation.dataIdentity) {
      if (entry.triangle < observation.originalTriangles ||
          entry.triangle >= triangleEnd)
        return unknown("Cut owns a face outside its recorded append range");
      ++owned;
    }
  }
  const bool compacted =
      observation.vertexCount == observation.originalVertices &&
      observation.triangleFlags.size() == observation.originalTriangles;
  if (compacted && !owned)
    return {CutMaterialization::Removed,
            "Exact compacted dimensions and no materialized ownership"};
  if (vertexEnd > observation.vertexCount ||
      triangleEnd > observation.triangleFlags.size())
    return unknown("Append range absent or partially compacted");
  std::size_t active{};
  for (std::size_t face = observation.originalTriangles; face < triangleEnd;
       ++face) {
    // Native undo writes overlap32. Deleted8 does not establish cut removal.
    if (observation.triangleFlags[face] & 32U)
      continue;
    ++active;
    const auto entry = owners.find(static_cast<std::uint32_t>(face));
    if (entry == owners.end())
      return unknown("Active append face has no cut ownership witness");
    if (entry->second != observation.dataIdentity)
      return unknown("Active append face belongs to a different cut");
  }
  if (active && owned)
    return {CutMaterialization::Applied,
            "Active append geometry and matching mesh-local cut ownership"};
  if (!active && !owned)
    return {CutMaterialization::Removed,
            "Inactive append geometry and no materialized ownership"};
  return unknown(
      "Inactive owned geometry needs complete nested-stack evidence");
}
} // namespace SkyBuild::Navmesh
