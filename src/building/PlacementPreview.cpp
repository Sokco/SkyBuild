#include "building/PackCategories.h"
#include "building/BuildCosts.h"
#include "containers/Containers.h"
#include "activators/Connections.h"
#include "building/BuildingHistory.h"
#include "building/PlacementPreview.h"
#include "building/PlacementRotation.h"
#include "building/BuildingManager.h"
#include "building/GridRaycast.h"
#include "building/PlaceableRegistry.h"
#include "building/PreviewMesh.h"
#include "building/Snapping.h"
#include "physics/Raycast.h"
#include "ui/imgui/UI.h"
#include "util.h"
#include "ui/fine-adjustments/FineAdjustments.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include "ui/pack-editor/PackEditor.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <numbers>

namespace SkyBuild::PlacementPreview {
namespace {
// STAIRS: WHIntWoodStairs01 0x0007EE42;

constexpr RE::FormID PLACEMENT_SOUND_FORM_ID = 0x03017ECB;
constexpr std::uint32_t PLACEMENT_COOLDOWN_FRAMES = 3;
State state{};
float moveOpacity = .65F;
State fineOriginal{};
std::optional<Physics::RaycastResult> propAnchor;
bool fineEditing = false, fineFrozen = false;
float fineOriginalOffset = 0;
RE::NiPointer<RE::TESObjectREFR> movingObject;
std::optional<PlaceableRegistry::Definition> movingDefinition;
RE::TESBoundObject *savedBaseObject = nullptr;
std::size_t savedPlaceableIndex = 0;
bool savedCulled = false;
RE::NiColor moveTint{0.1F, 0.45F, 1.0F};
std::uint32_t placementCooldownFrames{0};
std::size_t activePlaceableIndex{0};
Packs::Identity activeIdentity;
float rotationOffset{0.0F};
float snappedRotationOffset{0.0F};
float propBaseYaw{0.F};
std::optional<BuildingManager::BuildingID> placementBuildingID{};
BuildingManager::GridPosition placementGridPosition{};
BuildingManager::GridObjectType placementType{
    BuildingManager::GridObjectType::kBasic};

PreviewMesh previewMesh{};
RE::NiColor previewTint{0.0F, 1.0F, 0.0F};

BuildingManager::GridObjectType GetGridObjectType(PlaceableType type) {
  using T = BuildingManager::GridObjectType;
  return type == PlaceableType::kNone     ? T::kNone
         : type == PlaceableType::kSide   ? T::kSideX
         : type == PlaceableType::kCorner ? T::kCorner
                                          : T::kBasic;
}
Grid::Settings PlacementSettings() {
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  return movingDefinition ? movingDefinition->grid
         : activePlaceableIndex < definitions.size()
             ? definitions[activePlaceableIndex].grid
             : Grid::Settings{};
}
RE::NiPoint3 ModelRotationOffset() {
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  return movingDefinition ? movingDefinition->rotationOffset
         : activePlaceableIndex < definitions.size()
             ? definitions[activePlaceableIndex].rotationOffset
             : RE::NiPoint3{};
}
RE::NiPoint3 ModelPlacementRotation() {
  const auto offset = ModelRotationOffset();
  if (offset == RE::NiPoint3{})
    return state.rotation;
  RE::NiMatrix3 placement, model;
  placement.SetEulerAnglesXYZ(state.rotation);
  model.SetEulerAnglesXYZ(offset);
  RE::NiPoint3 result;
  PlacementRotation::ToEulerXYZ(placement * model, result);
  return result;
}

[[nodiscard]]
RE::NiPointer<RE::TESObjectREFR> CreatePrefab(RE::TESBoundObject *a_prefab) {
  if (!a_prefab) {
    spdlog::error("Cannot create prefab without a base object.");

    return nullptr;
  }

  const auto player = RE::PlayerCharacter::GetSingleton();

  if (!player) {
    spdlog::error("PlayerCharacter singleton was unavailable.");

    return nullptr;
  }

  auto createdPrefab = player->PlaceObjectAtMe(a_prefab, false);

  if (!createdPrefab) {
    spdlog::error("Failed to create prefab from {:08X}.",
                  a_prefab->GetFormID());

    return nullptr;
  }

  return createdPrefab;
}

bool FullMoveTo(const RE::TESObjectREFRPtr &object,
                const RE::NiPoint3 &position, const RE::NiPoint3 &rotation) {
  const auto player = RE::PlayerCharacter::GetSingleton();
  if (!object || !player || !player->GetParentCell())
    return false;
  // Same engine routine used by TESObjectREFR::MoveTo, with an explicit
  // destination transform. SetPosition/Update3DPosition alone can leave
  // static collision and reference bookkeeping at the old location.
  using MoveReference = void (*)(
      RE::TESObjectREFR *, const RE::ObjectRefHandle &, RE::TESObjectCELL *,
      RE::TESWorldSpace *, const RE::NiPoint3 &, const RE::NiPoint3 &);
  static REL::Relocation<MoveReference> move{RELOCATION_ID(56227, 56626)};
  const auto before = object->GetPosition();
  const auto handle = player->GetHandle();
  move(object.get(), handle, player->GetParentCell(), player->GetWorldspace(),
       position, rotation);
  object->Update3DPosition(true);
  const auto after = object->GetPosition();
  return true;
}

[[nodiscard]] RE::NiPoint3 GetWorldPlacementRotation() {
  // A moved reference retains its authored world orientation.
  // New objects start at world zero; R supplies the user offset.
  if (!movingObject)
    return {};
  const auto offset = ModelRotationOffset();
  if (offset == RE::NiPoint3{})
    return movingObject->GetAngle();
  RE::NiMatrix3 world, model;
  world.SetEulerAnglesXYZ(movingObject->GetAngle());
  model.SetEulerAnglesXYZ(offset);
  RE::NiPoint3 result;
  PlacementRotation::ToEulerXYZ(world * model.Transpose(), result);
  return result;
}

struct BuildRay {
  Physics::RaycastResults hits{};
  float maxDistance{0.0F};
};

BuildRay CastBuildRay() {
  constexpr float RAYCAST_MAX_DISTANCE = 1024.0F;
  auto hits = Physics::CastCameraRay(RAYCAST_MAX_DISTANCE);
  if (movingObject)
    std::erase_if(
        hits, [](const auto &hit) { return hit.hitReference == movingObject; });
  return {std::move(hits), RAYCAST_MAX_DISTANCE};
}

void UpdateStateFromRaycast() {
  placementBuildingID.reset();
  propAnchor.reset();
  placementGridPosition = {};
  if (placementCooldownFrames > 0) {
    --placementCooldownFrames;
    state.visible = false;
    state.validPlacement = false;
    return;
  }

  if (!state.active || SkyBuild::UI::IsToolWheelOpen()) {
    state.visible = false;
    state.validPlacement = false;
    return;
  }

  state.visible = false;
  state.validPlacement = false;

  const auto &definitions = PlaceableRegistry::GetDefinitions();
  if (!movingDefinition && activePlaceableIndex >= definitions.size()) {
    return;
  }
  const auto &activePlaceable =
      movingDefinition ? *movingDefinition : definitions[activePlaceableIndex];
  placementType = GetGridObjectType(activePlaceable.type);
  const auto buildRay = CastBuildRay();

  const auto &physicsHits = buildRay.hits;
  if (!state.snappingEnabled || activePlaceable.type == PlaceableType::kNone) {
    if (physicsHits.empty()) {
      return;
    }
    state.visible = true;
    state.validPlacement = true;
    state.position =
        physicsHits.front().position + activePlaceable.freePlaceOffset;
    state.rotation = GetWorldPlacementRotation();
    if (activePlaceable.type == PlaceableType::kNone) {
      propAnchor = physicsHits.front();
      float normal[4];
      _mm_storeu_ps(normal, propAnchor->normal.quad);
      if (activePlaceable.alignToWall)
        propBaseYaw = PlacementRotation::PropBaseYaw(normal[0], normal[1],
                                                     normal[2], propBaseYaw);
      else if (!movingObject)
        propBaseYaw = 0.F;
      // GetWorldPlacementRotation already removed the pack's
      // model offset. Keep the moved item's pitch/roll and seed
      // its yaw basis from that same logical orientation.
      state.rotation.z = propBaseYaw;
    }
    return;
  }
  const bool placingDoor =
      activePlaceable.grid.sideRole == Grid::SideRole::kDoor;
  const auto placedHit =
      std::ranges::find_if(physicsHits, [placingDoor](const auto &a_hit) {
        return a_hit.hitReference &&
               BuildingManager::IsPlacedObject(
                   a_hit.hitReference->GetFormID()) &&
               (!placingDoor ||
                BuildingManager::GetGridSettings(
                    a_hit.hitReference->GetFormID())
                        .sideRole == Grid::SideRole::kDoorway) &&
               BuildingManager::GetGridSettings(a_hit.hitReference->GetFormID())
                       .sideRole != Grid::SideRole::kDoor &&
               BuildingManager::GetGridObjectType(
                   a_hit.hitReference->GetFormID()) !=
                   BuildingManager::GridObjectType::kNone;
      });

  if (placedHit != physicsHits.end()) {
    const auto hitReference = placedHit->hitReference.get();
    if (const auto buildingID =
            BuildingManager::GetBuildingID(hitReference->GetFormID())) {
      BuildingManager::SetLastSeenBuilding(*buildingID);
    }
    float hitNormal[4]{};
    _mm_storeu_ps(hitNormal, placedHit->normal.quad);
    const auto camera = RE::PlayerCamera::GetSingleton();
    if (!camera || !camera->cameraRoot)
      return;
    const auto snap = Snapping::SnapToObject(
        hitReference, placedHit->position,
        {hitNormal[0], hitNormal[1], hitNormal[2]}, activePlaceable.type,
        activePlaceable.grid, camera->cameraRoot->world.translate);

    if (snap.didSnap) {
      state.visible = true;
      state.validPlacement = true;
      state.position = snap.position;
      state.rotation = snap.rotation;
      placementBuildingID = snap.buildingID;
      placementGridPosition = snap.gridPosition;
      placementType = snap.type;
      return;
    }
    if (!placingDoor)
      return;
  }

  const auto closestHit =
      physicsHits.empty() ? nullptr : std::addressof(physicsHits.front());
  // A rejected doorway hit must not truncate the fallback at that
  // occupied opening; allow the ray to reach another doorway.
  const float gridRaycastMaxDistance =
      placingDoor && placedHit != physicsHits.end() ? buildRay.maxDistance
      : closestHit ? GridRaycast::GetRayDistanceTo(closestHit->position)
                   : buildRay.maxDistance;
  const auto gridHit = GridRaycast::CastGridRay(
      gridRaycastMaxDistance, activePlaceable.type, activePlaceable.grid);
  if (gridHit.didSnap) {
    state.visible = true;
    state.validPlacement = true;
    state.position = gridHit.position;
    state.rotation = gridHit.rotation;
    placementBuildingID = gridHit.buildingID;
    placementGridPosition = gridHit.gridPosition;
    placementType = gridHit.type;
    return;
  }
  if (activePlaceable.grid.sideRole == Grid::SideRole::kDoor)
    return;
  state.visible = closestHit != nullptr;
  state.validPlacement = closestHit != nullptr;
  if (!closestHit) {
    return;
  }

  state.position = closestHit->position + activePlaceable.freePlaceOffset;
  state.rotation = GetWorldPlacementRotation();
}

const auto emptyRequirements =
    std::make_shared<const std::vector<BuildCosts::Requirement>>();
std::shared_ptr<const std::vector<BuildCosts::Requirement>> requirements =
    emptyRequirements;
std::chrono::steady_clock::time_point nextCostCheck{};
void UpdateCosts(bool force = false) {
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  if (!state.active || !state.baseObject || movingObject ||
      activePlaceableIndex >= definitions.size() ||
      definitions[activePlaceableIndex].requiredMaterials.empty() ||
      Settings::SandboxMode()) {
    if (!state.enoughMaterials)
      previewMesh.Reset();
    state.enoughMaterials = true;
    requirements = emptyRequirements;
    nextCostCheck = {};
    return;
  }
  if (!force && UI::IsGameMenuBlocking())
    return;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextCostCheck)
    return;
  nextCostCheck = now + std::chrono::milliseconds(200);
  auto next = std::make_shared<std::vector<BuildCosts::Requirement>>();
  *next = BuildCosts::CheckAvailability(
      definitions[activePlaceableIndex].requiredMaterials);
  const bool affordable = std::ranges::all_of(
      *next, [](const auto &cost) { return cost.Affordable(); });
  if (state.enoughMaterials != affordable)
    previewMesh.Reset();
  state.enoughMaterials = affordable;
  requirements = std::move(next);
  for (const auto &cost : *requirements)
    UI::PackEditor::QueueResourceIcon(cost.material.plugin,
                                      cost.material.local);
}

void ApplyState() {
  if (!state.active || !state.visible || !state.validPlacement ||
      !state.baseObject) {
    previewMesh.SetVisible(false);
    return;
  }
  const auto tint = movingObject             ? moveTint
                    : !state.enoughMaterials ? RE::NiColor{1.F, 0.F, 0.F}
                                             : previewTint;
  if (!previewMesh.EnsureCreated(
          state.baseObject,
          {movingObject ? moveOpacity : state.opacity, tint,
           tint == RE::NiColor{1.0F, 1.0F, 1.0F} ? 0.0F : 1.0F})) {
    return;
  }
  previewMesh.SetVisible(true);
  previewMesh.SetTransform(state.position, ModelPlacementRotation(),
                           state.scale);
}

void LoadDefaultBaseObject() {
  if (state.baseObject) {
    return;
  }

  auto &definitions = PlaceableRegistry::GetDefinitions();
  if (definitions.empty()) {
    spdlog::error("No placeable definitions were loaded.");
    return;
  }
  activePlaceableIndex = std::min(activePlaceableIndex, definitions.size() - 1);
  state.baseObject =
      PlaceableRegistry::Resolve(definitions[activePlaceableIndex]);

  if (!state.baseObject) {
    spdlog::error("Could not find the default placeable object.");

    return;
  }
}

} // namespace

const State &GetState() { return state; }

std::optional<PlaceableType> GetPlaceableType(RE::FormID a_formID) {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  const auto placeable = std::find_if(
      definitions.begin(), definitions.end(), [a_formID](auto &a_definition) {
        const auto object = PlaceableRegistry::Resolve(a_definition);
        return object && object->GetFormID() == a_formID;
      });
  return placeable != definitions.end() ? std::optional{placeable->type}
                                        : std::nullopt;
}

std::vector<PlaceableInfo> GetPlaceables() {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  std::vector<PlaceableInfo> result{};
  result.reserve(definitions.size());
  for (auto &definition : definitions) {
    if (const auto object = PlaceableRegistry::Resolve(definition)) {
      result.push_back({object->GetFormID(), definition.localFormID,
                        definition.packName, definition.pluginName,
                        definition.name, definition.type, definition.grid,
                        definition.category, definition.extendBasePack,
                        definition.iconYaw, definition.iconLight});
    }
  }
  return result;
}

void ReloadPlaceables() {
  if (movingDefinition && movingDefinition->identity)
    if (const auto definition =
            PlaceableRegistry::Find(movingDefinition->identity))
      movingDefinition = *definition;
  auto &definitions = PlaceableRegistry::GetDefinitions();
  if (definitions.empty()) {
    Stop();
    state.baseObject = nullptr;
    return;
  }
  activePlaceableIndex = 0;
  for (std::size_t i = 0; i < definitions.size(); ++i)
    if (definitions[i].identity == activeIdentity) {
      activePlaceableIndex = i;
      break;
    }
  state.baseObject = nullptr;
  SetBaseObject(PlaceableRegistry::Resolve(definitions[activePlaceableIndex]));
  state.validPlacement = false;
  propBaseYaw = rotationOffset = snappedRotationOffset = 0.0F;
}

std::shared_ptr<const std::vector<BuildCosts::Requirement>> GetRequirements() {
  return requirements;
}
bool SelectInstalledPlaceable(RE::FormID id, std::string_view pack,
                              std::string_view category,
                              Packs::Identity identity) {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  for (std::size_t i = 0; i < definitions.size(); ++i) {
    auto &definition = definitions[i];
    if (definition.packName != pack || definition.category != category ||
        (identity && definition.identity != identity))
      continue;
    const auto object = PlaceableRegistry::Resolve(definition);
    if (!object || object->GetFormID() != id)
      continue;
    activePlaceableIndex = i;
    SetBaseObject(object);
    propBaseYaw = rotationOffset = snappedRotationOffset = 0.0F;
    SkyBuild::UI::SetActivePlaceable(id);
    return true;
  }
  return false;
}
void SetBaseObject(RE::TESBoundObject *a_baseObject) {
  const auto previousIdentity = activeIdentity;
  const auto &selectedDefinitions = PlaceableRegistry::GetDefinitions();
  if (activePlaceableIndex < selectedDefinitions.size() &&
      selectedDefinitions[activePlaceableIndex].resolvedFormID ==
          (a_baseObject ? a_baseObject->GetFormID() : 0))
    activeIdentity = selectedDefinitions[activePlaceableIndex].identity;
  if (state.baseObject == a_baseObject) {
    if (activeIdentity != previousIdentity) {
      propBaseYaw = rotationOffset = snappedRotationOffset = 0.F;
      UpdateCosts(true);
      SkyBuild::UI::SetActivePlaceable(a_baseObject ? a_baseObject->GetFormID()
                                                    : 0);
    }
    return;
  }

  UI::FineAdjustments::Close();
  fineEditing = fineFrozen = false;
  state.scale = 1.0F;
  state.baseObject = a_baseObject;
  state.selectionPending = a_baseObject != nullptr;
  propBaseYaw = rotationOffset = snappedRotationOffset = 0.0F;
  state.visible = false;
  state.validPlacement = false;
  placementBuildingID.reset();
  placementGridPosition = {};

  if (a_baseObject) {
    auto &definitions = PlaceableRegistry::GetDefinitions();
    const auto iterator = std::find_if(
        definitions.begin(), definitions.end(),
        [a_baseObject](auto &a_placeable) {
          const auto object = PlaceableRegistry::Resolve(a_placeable);
          return object && object->GetFormID() == a_baseObject->GetFormID();
        });
    const bool activeMatches =
        activePlaceableIndex < definitions.size() &&
        PlaceableRegistry::Resolve(definitions[activePlaceableIndex]) ==
            a_baseObject;
    if (!activeMatches && iterator != definitions.end()) {
      activePlaceableIndex =
          static_cast<std::size_t>(iterator - definitions.begin());
    }
  }

  UpdateCosts(true);
  SkyBuild::UI::SetActivePlaceable(a_baseObject ? a_baseObject->GetFormID()
                                                : 0);
  previewMesh.Reset();
}

void SetTint(const RE::NiColor &tint) {
  if (previewTint != tint) {
    previewTint = tint;
    previewMesh.Reset();
  }
}

void SetOpacity(float a_opacity) {
  const float opacity = std::clamp(a_opacity, 0.0F, 1.0F);
  if (state.opacity != opacity) {
    state.opacity = opacity;
    previewMesh.Reset();
  }
}

void SetRotation(const RE::NiPoint3 &a_rotation) {
  state.rotation = a_rotation;
}

bool IsMovingObject() { return movingObject != nullptr; }
void SetMoveOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.F, 1.F);
  if (moveOpacity != opacity) {
    moveOpacity = opacity;
    if (movingObject)
      previewMesh.Reset();
  }
}
void SetMoveTint(const RE::NiColor &tint) {
  moveTint = tint;
  if (movingObject)
    previewMesh.Reset();
}
bool BeginMove(RE::TESObjectREFR *object) {
  if (!object || movingObject ||
      !BuildingManager::IsPlacedObject(object->GetFormID()) ||
      !object->GetBaseObject())
    return false;
  const auto type = BuildingManager::GetGridObjectType(object->GetFormID());
  if (!type)
    return false;
  savedBaseObject = state.baseObject;
  savedPlaceableIndex = activePlaceableIndex;
  SetBaseObject(object->GetBaseObject());
  movingDefinition = PlaceableRegistry::Definition{};
  if (const auto definition =
          BuildingManager::GetPlaceableDefinition(object->GetFormID()))
    movingDefinition = *definition;
  using T = BuildingManager::GridObjectType;
  movingDefinition->type = *type == T::kNone     ? PlaceableType::kNone
                           : *type == T::kCorner ? PlaceableType::kCorner
                           : (*type == T::kSideX || *type == T::kSideY)
                               ? PlaceableType::kSide
                               : PlaceableType::kBasic;
  movingDefinition->grid =
      BuildingManager::GetGridSettings(object->GetFormID());
  movingObject.reset(object);
  state.scale = object->GetScale();
  previewMesh.Reset();
  if (const auto node = object->Get3D()) {
    savedCulled = node->GetAppCulled();
    node->SetAppCulled(true);
  }
  BuildingManager::SetPlacementIgnored(object->GetFormID(), true);
  if (*type != T::kNone) {
    if (const auto building =
            BuildingManager::GetBuildingID(object->GetFormID()))
      BuildingManager::SetLastSeenBuilding(*building);
  }
  state.active = true;
  placementCooldownFrames = 0;
  propBaseYaw = rotationOffset = snappedRotationOffset = 0;
  propBaseYaw = GetWorldPlacementRotation().z;
  if (const auto buildingID =
          BuildingManager::GetBuildingID(object->GetFormID()))
    if (const auto building = BuildingManager::GetBuilding(*buildingID)) {
      const float slotYaw =
          building->yaw +
          (*type == T::kSideX ? std::numbers::pi_v<float> * .5F : 0.F);
      snappedRotationOffset = std::remainder(propBaseYaw - slotYaw,
                                             2.F * std::numbers::pi_v<float>);
    }
  Update();
  return true;
}
void CancelMove() {
  if (!movingObject)
    return;
  UI::FineAdjustments::Close();
  fineFrozen = false;
  if (const auto node = movingObject->Get3D())
    node->SetAppCulled(savedCulled);
  BuildingManager::SetPlacementIgnored(movingObject->GetFormID(), false);
  movingObject.reset();
  movingDefinition.reset();
  SetBaseObject(savedBaseObject);
  activePlaceableIndex = savedPlaceableIndex;
  savedBaseObject = nullptr;
  propBaseYaw = rotationOffset = snappedRotationOffset = 0;
  state.active = state.visible = state.validPlacement = false;
  previewMesh.Reset();
  if (state.baseObject)
    SkyBuild::UI::SetActivePlaceable(state.baseObject->GetFormID());
}
bool Start() {
  if (state.active) {
    return true;
  }

  LoadDefaultBaseObject();

  if (!state.baseObject) {
    spdlog::error("Placement preview could not start because "
                  "no base object was available.");

    return false;
  }

  state.active = true;
  state.visible = false;
  state.validPlacement = false;
  SkyBuild::UI::SetActivePlaceable(state.baseObject->GetFormID());

  return true;
}

void Stop() {
  propAnchor.reset();
  UI::FineAdjustments::Close();
  fineEditing = fineFrozen = false;
  CancelMove();
  if (!state.active && !previewMesh.IsCreated()) {
    return;
  }

  state.active = false;
  state.visible = false;
  state.validPlacement = false;
  placementCooldownFrames = 0;

  previewMesh.Reset();
}

void Update() {
  UpdateCosts();
  if (Activators::IsConnecting()) {
    state.visible = false;
    state.validPlacement = false;
  } else if (!fineFrozen) {
    state.selectionPending = false;
    UpdateStateFromRaycast();
    if (state.visible && state.validPlacement) {
      if (movingObject && placementBuildingID &&
          PlacementSettings().sideRole != Grid::SideRole::kDoor) {
        const auto original = GetWorldPlacementRotation();
        state.rotation.x = original.x;
        state.rotation.y = original.y;
      }
      if (!placementBuildingID ||
          PlacementSettings().sideRole != Grid::SideRole::kDoor)
        state.rotation.z +=
            placementBuildingID ? snappedRotationOffset : rotationOffset;
    }
  }
  ApplyState();
}

void ToggleSnapping() {
  if (!state.active) {
    return;
  }
  fineFrozen = false;
  state.snappingEnabled = !state.snappingEnabled;
  SkyBuild::UI::SetSnappingEnabled(state.snappingEnabled);
  Update();
}

void RotateSelectedPlaceable(bool clockwise) {
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  if (!state.active || !state.visible || !state.validPlacement ||
      (!movingDefinition && activePlaceableIndex >= definitions.size())) {
    return;
  }
  // A fitted door inherits the doorway orientation exactly.
  if (!fineFrozen && placementBuildingID &&
      PlacementSettings().sideRole == Grid::SideRole::kDoor)
    return;
  const float step =
      (clockwise ? 1.F : -1.F) *
      PlacementRotation::Step(placementBuildingID.has_value(),
                              (movingDefinition
                                   ? movingDefinition->type
                                   : definitions[activePlaceableIndex].type) ==
                                  PlaceableType::kSide,
                              fineFrozen);
  if (fineFrozen) {
    state.rotation.z += step;
    ApplyState();
    return;
  }
  auto &offset = placementBuildingID ? snappedRotationOffset : rotationOffset;
  offset = PlacementRotation::Advance(offset, step);
  // Rotate the currently displayed placement. Target acquisition belongs
  // to the game update, not to each repeated keyboard event.
  state.rotation.z += step;
  if (propAnchor) {
    const auto &definition = movingDefinition
                                 ? *movingDefinition
                                 : definitions[activePlaceableIndex];
    state.position = propAnchor->position + definition.freePlaceOffset;
  }
  ApplyState();
}

bool BeginFineAdjustments() {
  if (!state.active || !state.visible || !state.validPlacement || fineEditing)
    return false;
  fineOriginal = state;
  fineOriginalOffset = rotationOffset;
  fineEditing = fineFrozen = true;
  return true;
}
void SetFineTransform(const RE::NiPoint3 &position,
                      const RE::NiPoint3 &rotation, float scale) {
  if (!fineEditing || !std::isfinite(scale) || scale < 0.05F || scale > 10.0F)
    return;
  for (float v :
       {position.x, position.y, position.z, rotation.x, rotation.y, rotation.z})
    if (!std::isfinite(v))
      return;
  state.position = position;
  state.rotation = rotation;
  state.scale = scale;
  ApplyState();
}
void FinishFineAdjustments(bool confirm) {
  if (!fineEditing)
    return;
  fineEditing = false;
  if (confirm) {
    fineFrozen = true;
    placementBuildingID.reset();
    placementGridPosition = {};
    const auto &definitions = PlaceableRegistry::GetDefinitions();
    const auto &definition = movingDefinition
                                 ? *movingDefinition
                                 : definitions[activePlaceableIndex];
    placementType = GetGridObjectType(definition.type);
    // Only this edited transform is free placement. Keep the player's
    // snapping preference for subsequent previews.
  } else {
    state = fineOriginal;
    rotationOffset = fineOriginalOffset;
    fineFrozen = false;
  }
  UI::SetSnappingEnabled(state.snappingEnabled);
  ApplyState();
}
void PlacePreview() {
  if (fineEditing)
    return;
  const auto player = RE::PlayerCharacter::GetSingleton();

  if (!player) {
    return;
  }

  if (!state.active || placementCooldownFrames > 0 || !state.validPlacement ||
      !state.baseObject) {
    return;
  }

  if (placementBuildingID && !BuildingManager::CanPlaceGridObject(
                                 *placementBuildingID, placementGridPosition,
                                 placementType, PlacementSettings())) {
    state.visible = false;
    state.validPlacement = false;
    return;
  }

  const bool moving = IsMovingObject();
  BuildCosts::Payment payment;
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  if (!moving) {
    if (activePlaceableIndex >= definitions.size())
      return;
    if (!payment.Pay(definitions[activePlaceableIndex].requiredMaterials)) {
      (void)Util::PlaySoundAt(RE::PlayerCharacter::GetSingleton(), 0x0006BE20,
                              1.F);
      UpdateCosts(true);
      ApplyState();
      return;
    }
  }
  auto placedObject = moving ? movingObject : CreatePrefab(state.baseObject);

  if (!placedObject) {
    return;
  }

  const auto before = moving ? BuildingHistory::Capture(placedObject.get())
                             : BuildingHistory::Snapshot{};
  if (!FullMoveTo(placedObject, state.position, ModelPlacementRotation())) {
    if (!moving)
      placedObject->Disable();
    return;
  }
  if (!moving && !Containers::PrepareNewPlacement(placedObject.get())) {
    placedObject->Disable();
    return;
  }
  placedObject->SetScale(state.scale);
  // Rebuild static/furniture 3D and Havok at the destination, retaining the
  // reference itself and its state.
  placedObject->Disable();

  auto buildingID =
      placementBuildingID.value_or(BuildingManager::INVALID_BUILDING_ID);
  if (buildingID == BuildingManager::INVALID_BUILDING_ID) {
    // A new grid follows the logical placed rotation, including free rotation.
    // ModelPlacementRotation adds the authored mesh offset separately.
    buildingID = BuildingManager::CreateBuildingFromObject(
        placedObject.get(), placementType, std::optional{state.rotation.z},
        PlacementSettings());
  }
  if (buildingID == BuildingManager::INVALID_BUILDING_ID) {
    spdlog::error("Could not create a building for placed object {:08X}.",
                  placedObject->GetFormID());
    if (!moving)
      placedObject->Disable();
    return;
  }
  BuildingManager::AddPlacedObject(
      placedObject.get(), buildingID, placementGridPosition, placementType,
      PlacementSettings(),
      movingDefinition
          ? movingDefinition->identity
          : PlaceableRegistry::GetDefinitions()[activePlaceableIndex].identity);
  SkyBuild::Util::EnableWithFade(placedObject.get(), false);
  payment.Commit();
  nextCostCheck = {};
  BuildingHistory::Record(before, BuildingHistory::Capture(placedObject.get()));
  if (placementType != BuildingManager::GridObjectType::kNone)
    BuildingManager::SetLastSeenBuilding(buildingID);
  fineFrozen = false;
  if (moving)
    CancelMove();
  placementCooldownFrames = PLACEMENT_COOLDOWN_FRAMES;
  state.visible = false;
  state.validPlacement = false;

  (void)SkyBuild::Util::PlaySoundAt(player, PLACEMENT_SOUND_FORM_ID, 1);
}

void SelectPlaceableObject(std::int32_t a_direction) {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  for (std::size_t offset = 1; offset <= definitions.size(); ++offset) {
    const auto index =
        a_direction > 0 ? (activePlaceableIndex + offset) % definitions.size()
                        : (activePlaceableIndex + definitions.size() -
                           offset % definitions.size()) %
                              definitions.size();
    if (activePlaceableIndex < definitions.size() &&
        (definitions[index].category !=
             definitions[activePlaceableIndex].category ||
         definitions[index].PickerPack() !=
             definitions[activePlaceableIndex].PickerPack())) {
      continue;
    }
    auto *baseObject = PlaceableRegistry::Resolve(definitions[index]);
    if (!baseObject) {
      spdlog::warn("Could not resolve placeable at index {}.", index);
      continue;
    }

    activePlaceableIndex = index;
    SetBaseObject(baseObject);
    return;
  }

  spdlog::error("No configured placeable objects could be resolved.");
}

std::string_view GetActivePack() {
  const auto &definitions = PlaceableRegistry::GetDefinitions();
  return activePlaceableIndex < definitions.size()
             ? definitions[activePlaceableIndex].packName
             : std::string_view{};
}

void SelectPack(std::int32_t a_direction) {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  if (activePlaceableIndex >= definitions.size()) {
    return;
  }
  std::vector<std::string_view> packs;
  for (const auto &definition : definitions) {
    if (std::find(packs.begin(), packs.end(), definition.PickerPack()) ==
        packs.end()) {
      packs.push_back(definition.PickerPack());
    }
  }
  const auto current =
      std::find(packs.begin(), packs.end(),
                definitions[activePlaceableIndex].PickerPack()) -
      packs.begin();
  const auto category = definitions[activePlaceableIndex].category;
  for (std::size_t offset = 1; offset < packs.size(); ++offset) {
    const auto next = a_direction > 0
                          ? (current + offset) % packs.size()
                          : (current + packs.size() - offset) % packs.size();
    // Preserve the category if the destination pack has a usable item in it.
    for (bool sameCategory : {true, false}) {
      for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (definitions[index].PickerPack() != packs[next] ||
            (sameCategory && definitions[index].category != category)) {
          continue;
        }
        if (auto *object = PlaceableRegistry::Resolve(definitions[index])) {
          activePlaceableIndex = index;
          SetBaseObject(object);
          propBaseYaw = rotationOffset = snappedRotationOffset = 0.0F;
          SkyBuild::UI::SetActivePlaceable(object->GetFormID());
          return;
        }
      }
    }
  }
}

void SelectCategory(std::int32_t a_direction) {
  auto &definitions = PlaceableRegistry::GetDefinitions();
  if (activePlaceableIndex >= definitions.size()) {
    return;
  }
  std::vector<std::string> categories;
  for (const auto &definition : definitions)
    if (definition.PickerPack() ==
            definitions[activePlaceableIndex].PickerPack() &&
        std::find(categories.begin(), categories.end(), definition.category) ==
            categories.end())
      categories.push_back(definition.category);
  std::stable_partition(
      categories.begin(), categories.end(), [](const auto &category) {
        return !category.empty() && Packs::CategoryKey(category) != "other";
      });
  if (categories.empty())
    return;
  const auto current = std::find(categories.begin(), categories.end(),
                                 definitions[activePlaceableIndex].category);
  const auto currentIndex = static_cast<int>(current - categories.begin());
  const auto count = static_cast<int>(categories.size());
  for (int offset = 1; offset < count; ++offset) {
    const auto category = categories
        [(currentIndex + (a_direction > 0 ? offset : -offset) + count) % count];
    for (std::size_t index = 0; index < definitions.size(); ++index) {
      if (definitions[index].category != category ||
          definitions[index].PickerPack() !=
              definitions[activePlaceableIndex].PickerPack()) {
        continue;
      }
      if (auto *object = PlaceableRegistry::Resolve(definitions[index])) {
        activePlaceableIndex = index;
        SetBaseObject(object);
        return;
      }
    }
  }
}

void SelectPreviousPlaceableObject() { SelectPlaceableObject(-1); }

void SelectNextPlaceableObject() { SelectPlaceableObject(1); }
} // namespace SkyBuild::PlacementPreview
