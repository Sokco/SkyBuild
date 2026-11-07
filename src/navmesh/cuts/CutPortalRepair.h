#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
namespace SkyBuild::Navmesh {
struct CutPortalRepair {
  std::map<std::uint32_t, AuthoredMesh> meshes;
  std::size_t redirected{}, closed{};
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
// Repairs only existing authored connections, using explicit native face/edge
// replacement records. One authored edge can have several surviving segments;
// match them within that provenance by boundary intervals. No proximity search
// or new authored connections. Ambiguous many-to-one segments remain rejected.
[[nodiscard]] CutPortalRepair RepairCutPortals(
    const std::map<std::uint32_t, AuthoredMesh> &before,
    const std::map<std::uint32_t, AuthoredMesh> &cut,
    const std::map<std::uint32_t, std::vector<CutUndoRecord>> &records);
} // namespace SkyBuild::Navmesh
