#pragma once
#include "building/Snapping.h"
namespace SkyBuild::GridRaycast {
using Result = Snapping::SnapResult;
[[nodiscard]] Result
CastGridRay(const RE::NiPoint3 &origin, const RE::NiPoint3 &direction,
            float maximum, Grid::SnapMode mode, Grid::Settings settings,
            std::optional<BuildingManager::BuildingID> building = std::nullopt);
} // namespace SkyBuild::GridRaycast
