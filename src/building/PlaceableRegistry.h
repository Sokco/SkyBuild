#pragma once
#include "building/BuildCostData.h"
#include "models/ModelData.h"
#include "building/PackIdentity.h"

#include "building/PlacementPreview.h"

#include <string>
#include <vector>

namespace SkyBuild::PlaceableRegistry {
struct Definition {
  Packs::Identity identity;
  std::optional<Models::Bounds> selectionBounds;
  std::vector<BuildCosts::Material> requiredMaterials;
  std::string packName{};
  std::string name{};
  std::string category{"Other"};
  std::string pluginName{};
  RE::FormID localFormID{0};
  PlacementPreview::PlaceableType type{PlacementPreview::PlaceableType::kBasic};
  Grid::Settings grid{};
  RE::NiPoint3 freePlaceOffset{};
  RE::NiPoint3 rotationOffset{}; // Local model rotation, in radians.
  RE::FormID resolvedFormID{0};
  bool extendBasePack{false};
  bool alignToWall{false};
  float iconYaw{0}, iconLight{1};
  std::string_view PickerPack() const {
    return extendBasePack ? std::string_view{"SkyBuild"}
                          : std::string_view{packName};
  }
};

[[nodiscard]] bool Load();
const Definition *Find(Packs::Identity identity);
const Definition *FindUnique(RE::TESBoundObject *object);
[[nodiscard]] std::vector<Definition> &GetDefinitions();
[[nodiscard]] RE::TESBoundObject *Resolve(Definition &a_definition);
} // namespace SkyBuild::PlaceableRegistry
