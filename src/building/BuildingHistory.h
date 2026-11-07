#pragma once
#include "building/BuildingManager.h"
#include "building/BuildCostData.h"
namespace SkyBuild::BuildingHistory {
struct Snapshot {
  RE::NiPointer<RE::TESObjectREFR> object;
  RE::NiPoint3 position{}, rotation{};
  float scale{1};
  RE::FormID cell{};
  BuildingManager::PlacedObject placement{};
  BuildingManager::Building building{};
  std::optional<std::vector<BuildCosts::Material>> materials;
};
Snapshot Capture(RE::TESObjectREFR *object);
void Record(Snapshot before, Snapshot after);
void Undo();
void Redo();
bool CanUndo();
bool CanRedo();
void Clear();
} // namespace SkyBuild::BuildingHistory
