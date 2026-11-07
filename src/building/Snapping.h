#pragma once
#include "building/BuildingManager.h"
namespace SkyBuild::Snapping {
struct SnapResult {
  RE::NiPoint3 position{}, rotation{};
  BuildingManager::BuildingID buildingID{BuildingManager::INVALID_BUILDING_ID};
  BuildingManager::GridPosition gridPosition{};
  BuildingManager::GridObjectType type{BuildingManager::GridObjectType::kBasic};
  bool didSnap{false};
};
// Near a voxel boundary, prefer the neighbor across its closest face. Interior
// hits stay in their voxel. A known source cell resolves noisy volume hits.
[[nodiscard]] RE::NiPoint3 OffsetGridHit(
    BuildingManager::BuildingID id, const RE::NiPoint3 &point,
    const RE::NiPoint3 &rayDirection,
    std::optional<BuildingManager::GridPosition> sourceCell = std::nullopt);
[[nodiscard]] SnapResult MakeCandidate(
    BuildingManager::BuildingID id, const BuildingManager::GridPosition &cell,
    BuildingManager::GridObjectType type, Grid::Settings settings = {},
    bool allowAdjacentVolumeFaces = true);
// sampleDirection resolves exact cell boundaries in the forward ray direction.
[[nodiscard]] SnapResult SnapBasicAtPoint(BuildingManager::BuildingID id,
                                          const RE::NiPoint3 &point,
                                          const RE::NiPoint3 &sampleDirection,
                                          Grid::Settings settings);
[[nodiscard]] SnapResult SnapCellAtPoint(BuildingManager::BuildingID id,
                                         const RE::NiPoint3 &point,
                                         const RE::NiPoint3 &sampleDirection,
                                         Grid::SnapMode mode,
                                         Grid::Settings settings);
[[nodiscard]] SnapResult
SnapToObject(RE::TESObjectREFR *reference, const RE::NiPoint3 &point,
             const RE::NiPoint3 &normal, Grid::SnapMode mode,
             Grid::Settings settings, const RE::NiPoint3 &camera);
} // namespace SkyBuild::Snapping
