#include "activators/Connections.h"
#include "naming/ReferenceNames.h"
#include "map-markers/MapMarkers.h"
#include "building/BuildingManager.h"
#include "building/BuildingMode.h"
#include "building/BuildingHistory.h"
#include "building/BuildingSaveRecords.h"
#include "building/PlaceableRegistry.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "terrain/TerrainGrass.h"
#include "terrain/TerrainTool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <numbers>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace SkyBuild::BuildingManager {
namespace {
constexpr std::uint32_t MakeRecordType(char a_first, char a_second,
                                       char a_third, char a_fourth) {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_first)) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_second))
          << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_third))
          << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_fourth))
          << 24);
}

constexpr std::uint32_t SERIALIZATION_ID = MakeRecordType('S', 'K', 'B', 'D');
constexpr std::uint32_t BUILDINGS_RECORD = MakeRecordType('B', 'L', 'D', 'G');
constexpr std::uint32_t SERIALIZATION_VERSION = 7;
constexpr std::uint32_t MAX_SERIALIZED_BUILDINGS = 100'000;
constexpr std::uint32_t MAX_SERIALIZED_OBJECTS = 1'000'000;

using SaveRecords::LegacySerializedPlacedObject;
using SaveRecords::SerializedBuilding;
using SaveRecords::SerializedPlacedObject;

struct SlotKey {
  BuildingID buildingID{INVALID_BUILDING_ID};
  GridPosition gridPosition{};
  Grid::SlotKind kind{Grid::SlotKind::kCenter};

  bool operator==(const SlotKey &) const = default;
};

struct SlotKeyHash {
  std::size_t operator()(const SlotKey &a_key) const noexcept {
    std::size_t hash = std::hash<BuildingID>{}(a_key.buildingID);
    const auto combine = [&hash](std::int32_t a_value) {
      const auto valueHash = std::hash<std::int32_t>{}(a_value);
      hash ^= valueHash + 0x9E3779B9 + (hash << 6) + (hash >> 2);
    };
    combine(a_key.gridPosition.x);
    combine(a_key.gridPosition.y);
    combine(a_key.gridPosition.z);
    combine(static_cast<std::int32_t>(a_key.kind));
    return hash;
  }
};

BuildingMap buildings{};
PlacedObjectMap placedObjects{};
// Papyrus rack registration can query membership from outside the game thread.
// Guard structural writes and membership reads; release before engine
// callbacks.
std::shared_mutex membershipMutex;
std::unordered_set<RE::FormID> placementIgnored;
std::unordered_map<SlotKey, RE::FormID, SlotKeyHash> occupiedSlots{};
std::optional<BuildingID> lastSeenBuilding{};
BuildingID nextBuildingID = 1;

void ResetState() {
  BuildingHistory::Clear();
  buildings.clear();
  {
    std::unique_lock lock(membershipMutex);
    placedObjects.clear();
  }
  occupiedSlots.clear();
  placementIgnored.clear();
  lastSeenBuilding.reset();
  nextBuildingID = 1;
}

SlotKey Key(BuildingID id, const Grid::Slot &slot) {
  return {id, {slot.x, slot.y, slot.z}, slot.kind};
}
void IndexObjectSlots(RE::FormID id, const PlacedObject &entry) {
  if (placementIgnored.contains(id))
    return;
  for (const auto &slot :
       Grid::OccupiedSlots(entry.gridPosition.x, entry.gridPosition.y,
                           entry.gridPosition.z, entry.type, entry.grid))
    occupiedSlots.insert_or_assign(Key(entry.buildingID, slot), id);
}
void UnindexObjectSlots(RE::FormID id, const PlacedObject &entry) {
  for (const auto &slot :
       Grid::OccupiedSlots(entry.gridPosition.x, entry.gridPosition.y,
                           entry.gridPosition.z, entry.type, entry.grid)) {
    const auto found = occupiedSlots.find(Key(entry.buildingID, slot));
    if (found != occupiedSlots.end() && found->second == id)
      occupiedSlots.erase(found);
  }
}

void ApplyPackSettings(RE::FormID id, PlacedObject &entry) {
  if (entry.legacyIdentityPending && !entry.identity) {
    const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
    if (const auto definition = PlaceableRegistry::FindUnique(
            ref ? ref->GetBaseObject() : nullptr)) {
      entry.identity = definition->identity;
      entry.legacyIdentityPending = false;
    }
    if (ref)
      entry.legacyIdentityPending = false;
  }
  const auto definition = PlaceableRegistry::Find(entry.identity);
  entry.grid = definition ? definition->grid : Grid::Settings{0};
  using T = GridObjectType;
  entry.type = !definition || definition->type == Grid::SnapMode::kNone
                   ? T::kNone
               : definition->type == Grid::SnapMode::kCorner ? T::kCorner
               : definition->type == Grid::SnapMode::kSide
                   ? (entry.sideY ? T::kSideY : T::kSideX)
                   : T::kBasic;
}

template <class T>
bool WriteValue(SKSE::SerializationInterface *a_interface, const T &a_value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return a_interface &&
         a_interface->WriteRecordData(std::addressof(a_value),
                                      static_cast<std::uint32_t>(sizeof(T)));
}

template <class T>
bool ReadValue(SKSE::SerializationInterface *a_interface, T &a_value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return a_interface && a_interface->ReadRecordData(
                            std::addressof(a_value),
                            static_cast<std::uint32_t>(sizeof(T))) == sizeof(T);
}

RE::NiPointer<RE::TESObjectREFR>
MakeReferencePointer(RE::TESObjectREFR *a_reference) {
  RE::NiPointer<RE::TESObjectREFR> result{};
  if (a_reference) {
    result.reset(a_reference);
  }
  return result;
}

RE::NiPointer<RE::TESObjectREFR> ResolveEntry(RE::FormID a_formID,
                                              PlacedObject &a_entry) {
  if (!a_formID) {
    return nullptr;
  }
  if (a_entry.legacyIdentityPending) {
    UnindexObjectSlots(a_formID, a_entry);
    ApplyPackSettings(a_formID, a_entry);
    IndexObjectSlots(a_formID, a_entry);
  }
  if (a_entry.handle) {
    if (auto reference = a_entry.handle.get()) {
      return reference;
    }
    a_entry.handle.reset();
  }
  auto *reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_formID);
  if (!reference) {
    return nullptr;
  }
  a_entry.handle = reference->GetHandle();
  return MakeReferencePointer(reference);
}

void SaveCallback(SKSE::SerializationInterface *a_interface) {
  if (!a_interface ||
      !a_interface->OpenRecord(BUILDINGS_RECORD, SERIALIZATION_VERSION)) {
    spdlog::error("BuildingManager failed to open its serialization record.");
    return;
  }
  if (buildings.size() > std::numeric_limits<std::uint32_t>::max() ||
      placedObjects.size() > std::numeric_limits<std::uint32_t>::max()) {
    spdlog::error("BuildingManager has too much data to serialize.");
    return;
  }

  const auto buildingCount = static_cast<std::uint32_t>(buildings.size());
  if (!WriteValue(a_interface, buildingCount)) {
    return;
  }
  for (const auto &[buildingID, building] : buildings) {
    const SerializedBuilding serialized{buildingID, building.origin.x,
                                        building.origin.y, building.origin.z,
                                        building.yaw};
    if (!WriteValue(a_interface, serialized)) {
      spdlog::error("Failed to serialize building {}.", buildingID);
      return;
    }
  }

  RefreshPackSettings();
  const auto objectCount = static_cast<std::uint32_t>(placedObjects.size());
  if (!WriteValue(a_interface, objectCount)) {
    return;
  }
  for (const auto &[formID, entry] : placedObjects) {
    const SerializedPlacedObject serialized{
        formID,
        entry.buildingID,
        entry.gridPosition.x,
        entry.gridPosition.y,
        entry.gridPosition.z,
        entry.identity,
        static_cast<std::uint8_t>(entry.sideY)};
    if (!WriteValue(a_interface, serialized)) {
      spdlog::error("Failed to serialize placed reference {:08X}.", formID);
      return;
    }
  }

  spdlog::info("Serialized {} building(s) containing {} placed object(s).",
               buildingCount, objectCount);
  BuildingMode::SaveHammerGrant(a_interface);
  Navmesh::SaveCache(a_interface);
  Terrain::Save(a_interface);
  Terrain::Grass::Save(a_interface);
  MapMarkers::Save(a_interface);
  Activators::Save(a_interface);
  Naming::Save(a_interface);
}

void LoadBuildingsRecord(SKSE::SerializationInterface *a_interface,
                         std::uint32_t a_version) {
  if (!a_interface || (a_version != SERIALIZATION_VERSION && a_version != 6)) {
    spdlog::warn("Unsupported BuildingManager serialization version {}.",
                 a_version);
    return;
  }

  std::uint32_t buildingCount{0};
  if (!ReadValue(a_interface, buildingCount) ||
      buildingCount > MAX_SERIALIZED_BUILDINGS) {
    spdlog::error("BuildingManager rejected its serialized building count.");
    return;
  }
  buildings.reserve(buildingCount);
  for (std::uint32_t index = 0; index < buildingCount; ++index) {
    SerializedBuilding serialized{};
    if (!ReadValue(a_interface, serialized)) {
      spdlog::error("Stopped reading buildings at entry {} of {}.", index,
                    buildingCount);
      return;
    }
    if (serialized.id == INVALID_BUILDING_ID) {
      continue;
    }
    buildings.insert_or_assign(
        serialized.id,
        Building{serialized.id,
                 {serialized.originX, serialized.originY, serialized.originZ},
                 serialized.yaw});
    nextBuildingID = std::max(nextBuildingID, serialized.id + 1);
  }

  std::uint32_t objectCount{0};
  if (!ReadValue(a_interface, objectCount) ||
      objectCount > MAX_SERIALIZED_OBJECTS) {
    spdlog::error("BuildingManager rejected its serialized object count.");
    return;
  }
  {
    std::unique_lock lock(membershipMutex);
    placedObjects.reserve(objectCount);
  }
  occupiedSlots.reserve(objectCount);
  std::uint32_t loadedObjects{0};
  std::uint32_t unresolvedObjects{0};
  for (std::uint32_t index = 0; index < objectCount; ++index) {
    SerializedPlacedObject serialized{};
    if (a_version == 6) {
      LegacySerializedPlacedObject legacy{};
      if (!ReadValue(a_interface, legacy))
        return;
      if (legacy.type > GridObjectType::kNone ||
          (legacy.grid >> 5) > static_cast<unsigned>(Grid::SideRole::kDoor))
        continue;
      serialized = SaveRecords::Upgrade(legacy);
    } else if (!ReadValue(a_interface, serialized))
      return;
    if (!buildings.contains(serialized.buildingID) || serialized.sideY > 1)
      continue;
    RE::FormID resolvedFormID{0};
    if (!a_interface->ResolveFormID(serialized.formID, resolvedFormID)) {
      ++unresolvedObjects;
      continue;
    }
    std::unique_lock lock(membershipMutex);
    auto [iterator, inserted] = placedObjects.try_emplace(
        resolvedFormID,
        PlacedObject{{},
                     serialized.buildingID,
                     {serialized.gridX, serialized.gridY, serialized.gridZ},
                     GridObjectType::kNone,
                     Grid::Settings{0},
                     serialized.identity,
                     serialized.sideY != 0,
                     !serialized.identity});
    lock.unlock();
    if (!inserted) {
      continue;
    }
    ApplyPackSettings(resolvedFormID, iterator->second);
    IndexObjectSlots(resolvedFormID, iterator->second);
    if (auto *reference =
            RE::TESForm::LookupByID<RE::TESObjectREFR>(resolvedFormID)) {
      iterator->second.handle = reference->GetHandle();
    }
    ++loadedObjects;
  }
  spdlog::info(
      "Loaded {} building(s) and {} placed object(s); {} objects unresolved.",
      buildings.size(), loadedObjects, unresolvedObjects);
}

void LoadCallback(SKSE::SerializationInterface *a_interface) {
  BuildingMode::ResetHammerGrant();
  Activators::Reset();
  Naming::Reset();
  MapMarkers::Reset();
  Terrain::Reset();
  ResetState();
  if (!a_interface) {
    return;
  }
  std::uint32_t type{0};
  std::uint32_t version{0};
  std::uint32_t length{0};
  while (a_interface->GetNextRecordInfo(type, version, length)) {
    if (type == BUILDINGS_RECORD) {
      LoadBuildingsRecord(a_interface, version);
    } else if (type == Naming::NamesSaveRecord) {
      Naming::Load(a_interface, version, length);
    } else if (type == Activators::SaveRecord) {
      Activators::Load(a_interface, version, length);
    } else if (type == BuildingMode::HammerSaveRecord) {
      BuildingMode::LoadHammerGrant(a_interface, version, length);
    } else if (type == Navmesh::CELL_CACHE_RECORD) {
      Navmesh::LoadCache(a_interface, version, length);
    } else if (type == Terrain::Grass::ClearSaveRecord) {
      Terrain::Grass::Load(a_interface, version, length);
    } else if (type == MapMarkers::SaveRecord) {
      MapMarkers::Load(a_interface, version, length);
    } else if (type == Terrain::SaveRecord) {
      Terrain::Load(a_interface, version, length);
    } else {
      spdlog::warn("BuildingManager encountered unknown record {:08X}, version "
                   "{}, length {}.",
                   type, version, length);
    }
  }
}

void RevertCallback(SKSE::SerializationInterface *) {
  BuildingMode::ResetHammerGrant();
  Activators::Reset();
  Naming::Reset();
  MapMarkers::Reset();
  Terrain::Reset();
  Navmesh::Reset();
  ResetState();
  spdlog::info("Cleared BuildingManager state during revert.");
}
} // namespace

bool InitializeSerialization() {
  auto *serialization = SKSE::GetSerializationInterface();
  if (!serialization) {
    spdlog::error("SKSE serialization interface was unavailable.");
    return false;
  }
  serialization->SetUniqueID(SERIALIZATION_ID);
  serialization->SetSaveCallback(SaveCallback);
  serialization->SetLoadCallback(LoadCallback);
  serialization->SetRevertCallback(RevertCallback);
  spdlog::info("Initialized BuildingManager serialization.");
  return true;
}

BuildingID CreateBuilding(const RE::NiPoint3 &a_origin, float a_yaw) {
  while (nextBuildingID == INVALID_BUILDING_ID ||
         buildings.contains(nextBuildingID)) {
    ++nextBuildingID;
  }
  const BuildingID buildingID = nextBuildingID++;
  buildings.emplace(buildingID, Building{buildingID, a_origin, a_yaw});
  spdlog::info("Created building {} at ({}, {}, {}) with yaw {}.", buildingID,
               a_origin.x, a_origin.y, a_origin.z, a_yaw);
  return buildingID;
}

BuildingID CreateBuildingFromObject(RE::TESObjectREFR *a_object,
                                    GridObjectType a_type,
                                    std::optional<float> a_baseYaw,
                                    Grid::Settings settings) {
  if (!a_object) {
    return INVALID_BUILDING_ID;
  }
  auto origin = RE::NiPoint3{a_object->GetPositionX(), a_object->GetPositionY(),
                             a_object->GetPositionZ()};
  float yaw = a_baseYaw.value_or(a_object->GetAngleZ());
  if (a_type == GridObjectType::kSideX) {
    yaw -= std::numbers::pi_v<float> * 0.5F;
  }
  const RE::NiPoint3 forward{std::sin(yaw), std::cos(yaw), 0.0F};
  const RE::NiPoint3 right{std::cos(yaw), -std::sin(yaw), 0.0F};
  if (a_type == GridObjectType::kSideX) {
    origin = origin + right * (GRID_CELL_SIZE * 0.5F);
  } else if (a_type == GridObjectType::kSideY) {
    origin = origin + forward * (GRID_CELL_SIZE * 0.5F);
  } else if (a_type == GridObjectType::kCorner) {
    origin = origin + (right + forward) * (GRID_CELL_SIZE * 0.5F);
  }
  return CreateBuilding(origin, yaw);
}

std::optional<Building> GetBuilding(BuildingID a_buildingID) {
  const auto iterator = buildings.find(a_buildingID);
  return iterator == buildings.end() ? std::nullopt
                                     : std::optional{iterator->second};
}

std::optional<BuildingID> GetBuildingID(RE::FormID a_formID) {
  const auto iterator = placedObjects.find(a_formID);
  return iterator == placedObjects.end()
             ? std::nullopt
             : std::optional{iterator->second.buildingID};
}

void SetPlacementIgnored(RE::FormID id, bool ignored) {
  const auto found = placedObjects.find(id);
  if (found == placedObjects.end())
    return;
  if (ignored) {
    placementIgnored.insert(id);
    UnindexObjectSlots(id, found->second);
  } else {
    placementIgnored.erase(id);
    IndexObjectSlots(id, found->second);
  }
}
void AddPlacedObject(RE::TESObjectREFR *a_object, BuildingID a_buildingID,
                     GridPosition a_gridPosition, GridObjectType a_type,
                     Grid::Settings settings, Packs::Identity identity) {
  if (!a_object || !buildings.contains(a_buildingID)) {
    return;
  }
  const auto formID = a_object->GetFormID();
  if (!formID) {
    return;
  }
  BuildingID previousBuilding = INVALID_BUILDING_ID;
  bool sideY = a_type == GridObjectType::kSideY;
  if (const auto existing = placedObjects.find(formID);
      existing != placedObjects.end()) {
    previousBuilding = existing->second.buildingID;
    if (!identity)
      identity = existing->second.identity;
    if (a_type != GridObjectType::kSideX && a_type != GridObjectType::kSideY)
      sideY = existing->second.sideY;
    UnindexObjectSlots(existing->first, existing->second);
  }
  std::unique_lock lock(membershipMutex);
  const auto iterator =
      placedObjects
          .insert_or_assign(formID,
                            PlacedObject{a_object->GetHandle(), a_buildingID,
                                         a_gridPosition, a_type, settings,
                                         identity, sideY, !identity})
          .first;
  lock.unlock();
  ApplyPackSettings(iterator->first, iterator->second);
  IndexObjectSlots(iterator->first, iterator->second);
  Navmesh::ReferenceChanged(a_object);
  if (previousBuilding != INVALID_BUILDING_ID &&
      previousBuilding != a_buildingID &&
      !std::ranges::any_of(placedObjects,
                           [previousBuilding](const auto &entry) {
                             return entry.second.buildingID == previousBuilding;
                           })) {
    buildings.erase(previousBuilding);
    if (lastSeenBuilding == previousBuilding)
      lastSeenBuilding = a_buildingID;
  }

  spdlog::info(
      "Tracking object {:08X} in building {} at ({}, {}, {}). Total: {}.",
      formID, a_buildingID, a_gridPosition.x, a_gridPosition.y,
      a_gridPosition.z, placedObjects.size());
}

void RemovePlacedObject(RE::TESObjectREFR *a_object) {
  if (a_object) {
    RemovePlacedObject(a_object->GetFormID());
  }
}

void RemovePlacedObject(RE::FormID a_formID) {
  placementIgnored.erase(a_formID);
  const auto iterator = placedObjects.find(a_formID);
  if (iterator == placedObjects.end()) {
    return;
  }
  const auto buildingID = iterator->second.buildingID;
  Navmesh::ReferenceRemoved(a_formID);
  UnindexObjectSlots(iterator->first, iterator->second);
  {
    std::unique_lock lock(membershipMutex);
    placedObjects.erase(iterator);
  }
  const bool buildingHasObjects =
      std::ranges::any_of(placedObjects, [buildingID](const auto &a_entry) {
        return a_entry.second.buildingID == buildingID;
      });
  if (!buildingHasObjects) {
    buildings.erase(buildingID);
    if (lastSeenBuilding == buildingID) {
      lastSeenBuilding.reset();
    }
    spdlog::info("Removed empty building {}.", buildingID);
  }
  spdlog::info("Stopped tracking SkyBuild object {:08X}. Total: {}.", a_formID,
               placedObjects.size());
}

bool IsPlacedObject(RE::FormID a_formID) {
  std::shared_lock lock(membershipMutex);
  return a_formID != 0 && placedObjects.contains(a_formID);
}

void RefreshPackSettings() {
  occupiedSlots.clear();
  for (auto &[id, entry] : placedObjects) {
    if (!entry.identity)
      entry.legacyIdentityPending = true;
    ApplyPackSettings(id, entry);
    IndexObjectSlots(id, entry);
  }
}
const PlaceableRegistry::Definition *GetPlaceableDefinition(RE::FormID id) {
  const auto it = placedObjects.find(id);
  return it == placedObjects.end()
             ? nullptr
             : PlaceableRegistry::Find(it->second.identity);
}
Grid::Settings GetGridSettings(RE::FormID id) {
  const auto found = placedObjects.find(id);
  return found == placedObjects.end() ? Grid::Settings{} : found->second.grid;
}
bool IsSlotOccupied(BuildingID id, const Grid::Slot &slot) {
  return occupiedSlots.contains(Key(id, slot));
}
bool CanPlaceGridObject(BuildingID id, const GridPosition &position,
                        GridObjectType type, Grid::Settings settings) {
  if (!buildings.contains(id))
    return false;
  const bool side =
      type == GridObjectType::kSideX || type == GridObjectType::kSideY;
  if (side && settings.sideRole != Grid::SideRole::kDoor &&
      settings.sideRole != Grid::SideRole::kDoorway &&
      IsSlotOccupied(id,
                     {position.x, position.y, position.z,
                      type == GridObjectType::kSideX ? Grid::SlotKind::kDoorX
                                                     : Grid::SlotKind::kDoorY}))
    return false;
  for (const auto &slot :
       Grid::OccupiedSlots(position.x, position.y, position.z, type, settings))
    if (IsSlotOccupied(id, slot))
      return false;
  return true;
}

void SetLastSeenBuilding(BuildingID a_buildingID) {
  lastSeenBuilding = buildings.contains(a_buildingID)
                         ? std::optional{a_buildingID}
                         : std::nullopt;
}

std::optional<BuildingID> GetLastSeenBuilding() { return lastSeenBuilding; }

GridPosition GetGridPosition(RE::FormID a_formID) {
  const auto iterator = placedObjects.find(a_formID);
  return iterator == placedObjects.end() ? GridPosition{}
                                         : iterator->second.gridPosition;
}

std::optional<GridObjectType> GetGridObjectType(RE::FormID a_formID) {
  const auto iterator = placedObjects.find(a_formID);
  return iterator == placedObjects.end() ? std::nullopt
                                         : std::optional{iterator->second.type};
}

void SetGridPosition(RE::FormID a_formID, const GridPosition &a_gridPosition) {
  const auto iterator = placedObjects.find(a_formID);
  if (iterator != placedObjects.end()) {
    UnindexObjectSlots(iterator->first, iterator->second);
    iterator->second.gridPosition = a_gridPosition;
    IndexObjectSlots(iterator->first, iterator->second);
  }
}

GridPosition GetGridPosition(BuildingID a_buildingID,
                             const RE::NiPoint3 &a_worldPosition) {
  const auto building = GetBuilding(a_buildingID);
  if (!building) {
    return {};
  }
  const float deltaX = a_worldPosition.x - building->origin.x;
  const float deltaY = a_worldPosition.y - building->origin.y;
  return {
      static_cast<std::int32_t>(std::lround((deltaX * std::cos(building->yaw) -
                                             deltaY * std::sin(building->yaw)) /
                                            GRID_CELL_SIZE)),
      static_cast<std::int32_t>(std::lround((deltaX * std::sin(building->yaw) +
                                             deltaY * std::cos(building->yaw)) /
                                            GRID_CELL_SIZE)),
      static_cast<std::int32_t>(std::lround(
          (a_worldPosition.z - building->origin.z) / GRID_CELL_SIZE))};
}

RE::NiPoint3 GetWorldPosition(BuildingID a_buildingID,
                              const GridPosition &a_gridPosition) {
  const auto building = GetBuilding(a_buildingID);
  if (!building) {
    return {};
  }
  const float localX = static_cast<float>(a_gridPosition.x) * GRID_CELL_SIZE;
  const float localY = static_cast<float>(a_gridPosition.y) * GRID_CELL_SIZE;
  return {building->origin.x + localX * std::cos(building->yaw) +
              localY * std::sin(building->yaw),
          building->origin.y - localX * std::sin(building->yaw) +
              localY * std::cos(building->yaw),
          building->origin.z +
              static_cast<float>(a_gridPosition.z) * GRID_CELL_SIZE};
}

RE::NiPointer<RE::TESObjectREFR> ResolvePlacedObject(RE::FormID a_formID) {
  const auto iterator = placedObjects.find(a_formID);
  return iterator == placedObjects.end()
             ? nullptr
             : ResolveEntry(iterator->first, iterator->second);
}

RE::NiPointer<RE::TESObjectREFR>
GetAnyResolvedPlacedObject(BuildingID a_buildingID) {
  for (auto &[formID, entry] : placedObjects) {
    if (entry.buildingID == a_buildingID) {
      if (auto reference = ResolveEntry(formID, entry)) {
        return reference;
      }
    }
  }
  return nullptr;
}

std::vector<RE::NiPointer<RE::TESObjectREFR>> GetResolvedPlacedObjects() {
  std::vector<RE::NiPointer<RE::TESObjectREFR>> result{};
  result.reserve(placedObjects.size());
  for (auto &[formID, entry] : placedObjects) {
    if (auto reference = ResolveEntry(formID, entry)) {
      result.push_back(std::move(reference));
    }
  }
  return result;
}

const BuildingMap &GetBuildings() { return buildings; }

const PlacedObjectMap &GetPlacedObjects() { return placedObjects; }

std::size_t GetPlacedObjectCount() { return placedObjects.size(); }

void RestoreBuilding(const Building &building) {
  if (building.id != INVALID_BUILDING_ID) {
    buildings.try_emplace(building.id, building);
    nextBuildingID = std::max(nextBuildingID, building.id + 1);
  }
}

void Clear() {
  ResetState();
  spdlog::info("Cleared BuildingManager tracking data.");
}
} // namespace SkyBuild::BuildingManager
