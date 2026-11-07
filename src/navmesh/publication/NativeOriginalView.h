#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
#include "navmesh/cuts/NativeCutData.h"
namespace SkyBuild::Navmesh {
// Game-thread snapshot of the active publication's retained original epoch.
// Never serialize its retained native inputs or send them to workers.
struct NativeOriginalView {
  std::map<std::uint32_t, AuthoredMesh> topology;
  NativeCutSnapshot cuts;
  std::vector<NativeCutDataSource> cutData;
  std::map<std::uint32_t, std::vector<std::uint32_t>> cellMeshes;
};
} // namespace SkyBuild::Navmesh
