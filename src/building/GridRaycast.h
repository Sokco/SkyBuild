#pragma once
#include "building/Snapping.h"
namespace SkyBuild::GridRaycast {
using Result = Snapping::SnapResult;
[[nodiscard]] float GetRayDistanceTo(const RE::NiPoint3 &point);
[[nodiscard]] Result
CastGridRay(float maximum, Grid::SnapMode mode, Grid::Settings settings,
            std::optional<BuildingManager::BuildingID> building = std::nullopt);
} // namespace SkyBuild::GridRaycast
