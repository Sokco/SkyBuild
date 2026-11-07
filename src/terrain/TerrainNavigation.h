#pragma once
#include "terrain/TerrainSurface.h"
#include <memory>
namespace SkyBuild::Terrain {
// Game-thread capture; workers retain immutable published geometry only.
struct NavigationCell {
  std::uint32_t cellID{};
  Volume volume;
  std::array<std::shared_ptr<const TerrainSurface>, 16> surfaces;
};
std::vector<NavigationCell> CaptureNavigation(std::uint32_t worldspaceID);
bool NavigationBusy();
} // namespace SkyBuild::Terrain
