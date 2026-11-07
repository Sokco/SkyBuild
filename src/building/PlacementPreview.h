#pragma once

#include <string_view>
#include <memory>
#include "building/BuildCostData.h"
#include <vector>
#include "building/SnappingProfile.h"
#include "building/PackIdentity.h"

namespace SkyBuild::PlacementPreview {
using PlaceableType = Grid::SnapMode;

struct PlaceableInfo {
  RE::FormID formID{0};
  RE::FormID localFormID{0};
  std::string_view packName{};
  std::string_view pluginName{};
  std::string_view name{};
  PlaceableType type{PlaceableType::kBasic};
  Grid::Settings grid{};
  std::string_view category{};
  bool extendBasePack{false};
  float iconYaw{0}, iconLight{1};
};

struct State {
  RE::TESBoundObject *baseObject{nullptr};

  RE::NiPoint3 position{};
  RE::NiPoint3 rotation{};

  float opacity{0.5F};
  float scale{1.0F};

  bool snappingEnabled{true};
  bool active{false};
  bool visible{false};
  bool validPlacement{false};
  bool enoughMaterials{true};
  bool selectionPending{false};
};

[[nodiscard]]
const State &GetState();
std::shared_ptr<const std::vector<BuildCosts::Requirement>> GetRequirements();
[[nodiscard]]
std::optional<PlaceableType> GetPlaceableType(RE::FormID a_formID);
[[nodiscard]] std::vector<PlaceableInfo> GetPlaceables();

void ReloadPlaceables();
void SetBaseObject(RE::TESBoundObject *a_baseObject);
bool SelectInstalledPlaceable(RE::FormID id, std::string_view pack,
                              std::string_view category,
                              Packs::Identity identity = {});
void SetOpacity(float a_opacity);
void SetTint(const RE::NiColor &tint);
void SetRotation(const RE::NiPoint3 &a_rotation);

[[nodiscard]] bool Start();
void Stop();
bool BeginMove(RE::TESObjectREFR *object);
void CancelMove();
bool IsMovingObject();
void SetMoveOpacity(float opacity);
void SetMoveTint(const RE::NiColor &tint);
void Update();

void ToggleSnapping();
void RotateSelectedPlaceable(bool clockwise = true);
void PlacePreview();
bool BeginFineAdjustments();
void SetFineTransform(const RE::NiPoint3 &position,
                      const RE::NiPoint3 &rotation, float scale);
void FinishFineAdjustments(bool confirm);
[[nodiscard]] std::string_view GetActivePack();
void SelectPack(std::int32_t a_direction);
void SelectCategory(std::int32_t a_direction);
void SelectPreviousPlaceableObject();
void SelectNextPlaceableObject();
} // namespace SkyBuild::PlacementPreview
