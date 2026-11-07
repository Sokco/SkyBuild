#include "navmesh/cuts/CutGenerationBasis.h"
#include <algorithm>
namespace SkyBuild::Navmesh {
CutGenerationBasis PrepareCutGenerationBasis(
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const NativeCutSnapshot &cuts, const std::set<std::uint32_t> &cellMeshes,
    const std::map<std::uint32_t, AuthoredMesh> *validatedSessionBasis) {
  CutGenerationBasis result;
  auto fail = [&](std::string error) {
    result = {};
    result.error = std::move(error);
    return result;
  };
  auto restored =
      ReplayCapturedCutUndo(originals, cuts.records, validatedSessionBasis);
  if (!restored.Valid())
    return fail("Uncut generation basis: " + restored.error);
  result.portableTopology = std::move(restored.meshes);
  for (auto &[id, mesh] : result.portableTopology) {
    const auto records = cuts.records.find(id);
    const auto count =
        records == cuts.records.end() ? 0 : records->second.size();
    const bool applied =
        records != cuts.records.end() &&
        std::any_of(records->second.begin(), records->second.end(),
                    [](const auto &record) { return record.Applied(); });
    const auto obstacleEntries = cuts.obstacleEntries.find(id);
    if (!applied && obstacleEntries != cuts.obstacleEntries.end() &&
        obstacleEntries->second != 0)
      return fail("Native obstacle ownership without an applied cut");
    if (mesh.nativeDependencies && !count)
      return fail("Native dependencies without a captured cut stack");
    if (count) {
      const auto auxiliary = cuts.auxiliaryIndices.find(id);
      if (auxiliary != cuts.auxiliaryIndices.end()) {
        std::vector<std::uint16_t> accounted;
        for (const auto &record : records->second)
          if (record.Applied())
            accounted.insert(accounted.end(), record.auxiliaryIndices.begin(),
                             record.auxiliaryIndices.end());
        if (accounted != auxiliary->second)
          return fail("Unaccounted native cut auxiliary entries");
      }
      for (const auto &record : records->second)
        if (record.Applied()) {
          result.pendingCutMeshes.insert(id);
          ++result.pendingCuts;
        }
      // This marker describes portable metadata, never native readiness.
      mesh.nativeDependencies = false;
    }
    for (const auto &face : mesh.triangles)
      if (face.flags & 32U)
        return fail("Unrestored overlap in generation basis");
  }
  for (const auto id : cellMeshes) {
    const auto found = result.portableTopology.find(id);
    if (found == result.portableTopology.end())
      return fail("Cell mesh missing from restored graph");
    const auto &source = found->second;
    auto baseline = std::make_shared<Baseline>();
    baseline->formID = id;
    baseline->mesh.vertices = source.vertices;
    for (std::size_t index = 0; index < source.triangles.size(); ++index) {
      const auto &face = source.triangles[index];
      Triangle triangle;
      triangle.source = {id, static_cast<std::uint32_t>(index)};
      triangle.flags = face.flags;
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
} // namespace SkyBuild::Navmesh
