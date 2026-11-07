#include "building/ModifyMode.h"
#include "activators/Connections.h"
#include "building/BuildingHistory.h"
#include "building/BuildingMode.h"
#include "building/PlaceableRegistry.h"
#include "containers/Containers.h"

#include "building/BuildingManager.h"
#include "building/PlacementPreview.h"
#include "building/PreviewMesh.h"
#include "building/BuildCosts.h"
#include "physics/Raycast.h"
#include "ui/imgui/UI.h"
#include "util.h"

namespace SkyBuild::ModifyMode {
namespace {
constexpr float RAYCAST_MAX_DISTANCE = 1024.0F;
PreviewMesh highlightMesh;
float highlightOpacity{.65F};
RE::NiColor highlightTint{.1F, .45F, 1.F};

RE::NiPointer<RE::TESObjectREFR> targetedObject{};
bool active = false;

RE::NiPointer<RE::TESObjectREFR> FindTargetedObject() {
  const auto hits = Physics::CastSelectionRay(
      RAYCAST_MAX_DISTANCE, [](RE::TESObjectREFR *ref) {
        return BuildingManager::IsPlacedObject(ref->GetFormID());
      });
  if (hits.empty() || !hits.front().hitReference ||
      !BuildingManager::IsPlacedObject(hits.front().hitReference->GetFormID()))
    return nullptr;
  return hits.front().hitReference;
}

void ClearTarget() {
  targetedObject.reset();
  highlightMesh.Reset();
}

} // namespace

void SetOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.F, 1.F);
  if (highlightOpacity != opacity) {
    highlightOpacity = opacity;
    PlacementPreview::SetMoveOpacity(opacity);
    highlightMesh.Reset();
  }
}
void SetTint(const RE::NiColor &tint) {
  if (highlightTint != tint) {
    highlightTint = tint;
    PlacementPreview::SetMoveTint(tint);
    highlightMesh.Reset();
  }
}

void Start() {
  active = true;
  ClearTarget();
}

void Stop() {
  active = false;
  PlacementPreview::CancelMove();
  ClearTarget();
}

void Update() {
  if (!active || UI::IsToolWheelOpen() || UI::IsGameMenuBlocking() ||
      PlacementPreview::IsMovingObject()) {
    if (UI::IsGameMenuBlocking(true))
      PlacementPreview::CancelMove();
    ClearTarget();
    return;
  }

  auto target = FindTargetedObject();
  if (!target) {
    ClearTarget();
    return;
  }

  auto baseObject = target->GetBaseObject();
  if (!baseObject ||
      !highlightMesh.EnsureCreated(
          baseObject, {highlightOpacity, highlightTint, 1.5F, true})) {
    ClearTarget();
    return;
  }

  targetedObject = std::move(target);
  highlightMesh.SetTransform(targetedObject->GetPosition(),
                             targetedObject->GetAngle(),
                             targetedObject->GetScale());
  highlightMesh.SetVisible(true);
}

void Use() {
  if (!active)
    return;
  if (PlacementPreview::IsMovingObject()) {
    PlacementPreview::PlacePreview();
    return;
  }
  Update();
  if (targetedObject && PlacementPreview::BeginMove(targetedObject.get()))
    ClearTarget();
}
void Break() {
  if (!active || PlacementPreview::IsMovingObject())
    return;
  Update();
  if (!HasTarget())
    return;
  const auto object = targetedObject;
  const auto handle = object->GetHandle();
  Containers::RequestRemoval(object.get(), [handle] {
    const auto target = handle.get();
    if (!target || !BuildingManager::IsPlacedObject(target->GetFormID()) ||
        !Containers::CanRemove(target.get()))
      return;
    auto before = BuildingHistory::Capture(target.get());
    if (!before.object || !before.materials)
      return;
    BuildCosts::Refund refund;
    if (!refund.Prepare(*before.materials, true))
      return;
    ClearTarget();
    BuildingHistory::Record(std::move(before), {});
    BuildingManager::RemovePlacedObject(target.get());
    target->Disable();
    refund.Grant();
    (void)Util::PlaySoundAt(RE::PlayerCharacter::GetSingleton(), 0x0003C82A,
                            1.F);
  });
}
bool CanCopy() {
  return HasTarget() &&
         BuildingManager::GetPlaceableDefinition(targetedObject->GetFormID());
}
bool CanEditProperties() {
  return HasTarget() && Activators::SupportsProperties(targetedObject.get());
}
void Copy() {
  if (!active || PlacementPreview::IsMovingObject())
    return;
  Update();
  if (!CanCopy())
    return;
  const auto definition =
      *BuildingManager::GetPlaceableDefinition(targetedObject->GetFormID());
  const auto id = targetedObject->GetBaseObject()->GetFormID();
  BuildingMode::SelectTool(BuildingMode::InteractionMode::kBuild);
  if (!PlacementPreview::SelectInstalledPlaceable(
          id, definition.packName, definition.category, definition.identity))
    RE::SendHUDMessage::ShowHUDMessage(
        "SkyBuild: this piece is no longer available in its pack.");
}
void EditProperties() {
  if (!active || PlacementPreview::IsMovingObject())
    return;
  Update();
  if (CanEditProperties())
    Activators::OpenProperties(targetedObject.get());
}
bool HasTarget() {
  return active && targetedObject &&
         BuildingManager::IsPlacedObject(targetedObject->GetFormID());
}
} // namespace SkyBuild::ModifyMode
