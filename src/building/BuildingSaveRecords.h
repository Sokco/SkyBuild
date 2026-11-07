#pragma once
#include "building/PackIdentity.h"
#include "building/SnappingProfile.h"
#include <type_traits>
namespace SkyBuild::BuildingManager::SaveRecords {
using BuildingID = std::uint32_t;
constexpr BuildingID INVALID_BUILDING_ID = 0;
using GridObjectType = Grid::ObjectType;
#pragma pack(push, 1)
struct SerializedBuilding {
  BuildingID id{INVALID_BUILDING_ID};
  float originX{0.0F};
  float originY{0.0F};
  float originZ{0.0F};
  float yaw{0.0F};
};

struct LegacySerializedPlacedObject {
  std::uint32_t formID{0};
  BuildingID buildingID{INVALID_BUILDING_ID};
  std::int32_t gridX{0};
  std::int32_t gridY{0};
  std::int32_t gridZ{0};
  GridObjectType type{GridObjectType::kBasic};
  std::uint8_t grid{}; // Five occupancy bits and two side-role bits.
};
struct SerializedPlacedObject {
  std::uint32_t formID{};
  BuildingID buildingID{};
  std::int32_t gridX{}, gridY{}, gridZ{};
  Packs::Identity identity;
  std::uint8_t
      sideY{}; // Placement orientation within the building, not a setting.
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<SerializedBuilding>);
static_assert(std::is_trivially_copyable_v<SerializedPlacedObject>);
static_assert(sizeof(SerializedBuilding) == 20);
static_assert(sizeof(LegacySerializedPlacedObject) == 22);
static_assert(sizeof(SerializedPlacedObject) == 53);

inline SerializedPlacedObject
Upgrade(const LegacySerializedPlacedObject &legacy) {
  return {legacy.formID,
          legacy.buildingID,
          legacy.gridX,
          legacy.gridY,
          legacy.gridZ,
          {},
          static_cast<std::uint8_t>(legacy.type == GridObjectType::kSideY)};
}
} // namespace SkyBuild::BuildingManager::SaveRecords
