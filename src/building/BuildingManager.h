#pragma once

#include "building/PackIdentity.h"
#include "building/SnappingProfile.h"
namespace SkyBuild::PlaceableRegistry {
struct Definition;
}
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace SkyBuild::BuildingManager {
inline constexpr float GRID_CELL_SIZE = 256.0F;

using BuildingID = std::uint32_t;
inline constexpr BuildingID INVALID_BUILDING_ID = 0;

using GridObjectType = Grid::ObjectType;

struct GridPosition {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t z{0};

  bool operator==(const GridPosition &) const = default;
};

struct Building {
  BuildingID id{INVALID_BUILDING_ID};
  RE::NiPoint3 origin{};
  float yaw{0.0F};
};

struct PlacedObject {
  // Runtime-only cache. Not serialized.
  RE::ObjectRefHandle handle{};

  BuildingID buildingID{INVALID_BUILDING_ID};
  GridPosition gridPosition{};
  GridObjectType type{GridObjectType::kBasic};
  Grid::Settings
      grid{}; // Runtime cache of current pack settings, never an override.
  Packs::Identity identity;
  bool sideY{}, legacyIdentityPending{};
};

using BuildingMap = std::unordered_map<BuildingID, Building>;
using PlacedObjectMap = std::unordered_map<RE::FormID, PlacedObject>;

[[nodiscard]] bool InitializeSerialization();

[[nodiscard]] BuildingID CreateBuilding(const RE::NiPoint3 &a_origin,
                                        float a_yaw);
[[nodiscard]] BuildingID
CreateBuildingFromObject(RE::TESObjectREFR *a_object, GridObjectType a_type,
                         std::optional<float> a_baseYaw = std::nullopt,
                         Grid::Settings settings = {});
[[nodiscard]] std::optional<Building> GetBuilding(BuildingID a_buildingID);
[[nodiscard]] std::optional<BuildingID> GetBuildingID(RE::FormID a_formID);

void SetPlacementIgnored(RE::FormID id, bool ignored);
void AddPlacedObject(RE::TESObjectREFR *a_object, BuildingID a_buildingID,
                     GridPosition a_gridPosition = {},
                     GridObjectType a_type = GridObjectType::kBasic,
                     Grid::Settings settings = {},
                     Packs::Identity identity = {});
void RefreshPackSettings();
const PlaceableRegistry::Definition *GetPlaceableDefinition(RE::FormID id);
void RemovePlacedObject(RE::TESObjectREFR *a_object);
void RemovePlacedObject(RE::FormID a_formID);

[[nodiscard]] bool IsPlacedObject(RE::FormID a_formID);
[[nodiscard]] bool IsSlotOccupied(BuildingID id, const Grid::Slot &slot);
[[nodiscard]] Grid::Settings GetGridSettings(RE::FormID id);
[[nodiscard]] bool CanPlaceGridObject(BuildingID id,
                                      const GridPosition &position,
                                      GridObjectType type,
                                      Grid::Settings settings = {});
void SetLastSeenBuilding(BuildingID a_buildingID);
[[nodiscard]] std::optional<BuildingID> GetLastSeenBuilding();

[[nodiscard]] GridPosition GetGridPosition(RE::FormID a_formID);
[[nodiscard]] std::optional<GridObjectType>
GetGridObjectType(RE::FormID a_formID);
void SetGridPosition(RE::FormID a_formID, const GridPosition &a_gridPosition);

[[nodiscard]] GridPosition GetGridPosition(BuildingID a_buildingID,
                                           const RE::NiPoint3 &a_worldPosition);
[[nodiscard]] RE::NiPoint3 GetWorldPosition(BuildingID a_buildingID,
                                            const GridPosition &a_gridPosition);

[[nodiscard]] RE::NiPointer<RE::TESObjectREFR>
ResolvePlacedObject(RE::FormID a_formID);
[[nodiscard]] RE::NiPointer<RE::TESObjectREFR>
GetAnyResolvedPlacedObject(BuildingID a_buildingID);
[[nodiscard]] std::vector<RE::NiPointer<RE::TESObjectREFR>>
GetResolvedPlacedObjects();
[[nodiscard]] const BuildingMap &GetBuildings();
[[nodiscard]] const PlacedObjectMap &GetPlacedObjects();
[[nodiscard]] std::size_t GetPlacedObjectCount();

/** Clears tracking data without deleting placed references from Skyrim. */
void RestoreBuilding(const Building &building);
void Clear();
} // namespace SkyBuild::BuildingManager
