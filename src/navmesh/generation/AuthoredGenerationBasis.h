#pragma once
#include "navmesh/generation/AuthoredBasisStore.h"
#include "navmesh/generation/AuthoredSourceCompatibility.h"
#include <set>
namespace SkyBuild::Navmesh {
// Geometry source only. Native cut inputs, bindings and publication authority
// belong to a different transaction; they cannot be inferred from this result.
struct AuthoredGenerationBasis {
  std::map<std::uint32_t, AuthoredMesh> topology;
  std::map<std::uint32_t, std::shared_ptr<const Baseline>> baselines;
  std::map<std::uint32_t, std::pair<std::string, std::uint64_t>> provenance;
  // Runtime annotation witnesses; never serialized as geometry or authority
  // to ignore arbitrary triangle flags.
  std::map<std::uint32_t, std::set<std::uint32_t>> doorFaces;
  std::string error;
  bool Valid() const { return error.empty(); }
};
inline AuthoredGenerationBasis PrepareAuthoredGenerationBasis(
    const std::map<std::uint32_t, std::shared_ptr<const AuthoredBasisRecord>>
        &records,
    const std::set<std::uint32_t> &selected) {
  AuthoredGenerationBasis result;
  auto fail = [](std::string error) {
    AuthoredGenerationBasis failed;
    failed.error = std::move(error);
    return failed;
  };
  for (const auto &[id, record] : records) {
    if (!record || !id || id != record->mesh.formID || id >= 0xFF000000 ||
        record->mesh.nativeDependencies || record->plugin.empty() ||
        !record->loadSequence)
      return fail("Missing or invalid authored record provenance");
    result.topology.emplace(id, record->mesh);
    for (const auto *faces :
         {&record->mesh.doorTriangles, &record->mesh.closedDoorTriangles})
      for (const auto face : *faces)
        result.doorFaces[id].insert(face);
    result.provenance.emplace(id,
                              std::pair{record->plugin, record->loadSequence});
  }
  const auto audit = AuditAuthoredTopology(result.topology);
  if (!audit.Valid())
    return fail("Authored record topology: " + audit.firstError);
  if (audit.overlappingTriangles)
    return fail("Authored overlap requires explicit source semantics");
  for (auto id : selected) {
    const auto found = result.topology.find(id);
    if (found == result.topology.end())
      return fail("Selected authored record is unavailable");
    auto baseline = std::make_shared<Baseline>();
    baseline->formID = id;
    baseline->mesh.vertices = found->second.vertices;
    for (std::size_t index = 0; index < found->second.triangles.size();
         ++index) {
      const auto &face = found->second.triangles[index];
      Triangle triangle;
      triangle.flags = face.flags;
      triangle.source = {id, static_cast<std::uint32_t>(index)};
      for (unsigned side = 0; side < 3; ++side) {
        triangle.vertices[side] = face.vertices[side];
        triangle.neighbors[side] =
            (face.flags & (1U << side)) || face.edges[side] == 0xFFFF
                ? -1
                : face.edges[side];
      }
      baseline->mesh.triangles.push_back(triangle);
    }
    result.baselines.emplace(id, std::move(baseline));
  }
  return result;
}
// Runtime doors are bindings over authored faces, not generation geometry.
// Keep baselines immutable; only the topology passed to native reconstruction
// receives this explicitly witnessed overlay. Appended cut faces need an
// independently resolved door anchor and are rejected rather than guessed.
inline std::string ApplyAuthoredDoorBindings(
    AuthoredGenerationBasis &basis,
    const std::map<std::uint32_t, AuthoredMesh> &observed) {
  auto topology = basis.topology;
  auto doorFaces = basis.doorFaces;
  for (auto &[id, source] : topology) {
    const auto live = observed.find(id);
    if (live == observed.end())
      return "Missing live binding source";
    const auto prefix = CompareAuthoredGeometryPrefix(source, live->second);
    if (!prefix.Matches())
      return "Authored source geometry changed: " + prefix.error;
    for (const auto *bindings :
         {&live->second.doorTriangles, &live->second.closedDoorTriangles})
      for (const auto face : *bindings) {
        if (face >= source.triangles.size())
          return "Runtime door binding requires an authored-face anchor";
        doorFaces[id].insert(face);
      }
    source.doorTriangles = live->second.doorTriangles;
    source.closedDoorTriangles = live->second.closedDoorTriangles;
    for (const auto face : source.closedDoorTriangles)
      source.triangles[face].flags |= 0x1000U;
  }
  basis.topology = std::move(topology);
  basis.doorFaces = std::move(doorFaces);
  return {};
}
} // namespace SkyBuild::Navmesh
