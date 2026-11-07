#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
#include <set>
namespace SkyBuild::Navmesh {
// Owned preparation data only. Portable metadata has no native cut bookkeeping;
// pendingCutMeshes MUST be reapplied before any native publication is
// permitted.
struct CutGenerationBasis {
  std::map<std::uint32_t, AuthoredMesh> portableTopology;
  std::map<std::uint32_t, std::shared_ptr<const Baseline>> baselines;
  std::set<std::uint32_t> pendingCutMeshes;
  std::size_t pendingCuts{};
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
[[nodiscard]] CutGenerationBasis PrepareCutGenerationBasis(
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const NativeCutSnapshot &cuts, const std::set<std::uint32_t> &cellMeshes,
    const std::map<std::uint32_t, AuthoredMesh> *validatedSessionBasis =
        nullptr);
} // namespace SkyBuild::Navmesh
