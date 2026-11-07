#include "building/BuildingHistory.h"
#include "building/BuildCosts.h"
#include "building/PlaceableRegistry.h"
#include "building/PlacementPreview.h"
#include "containers/Containers.h"
#include "util.h"
namespace SkyBuild::BuildingHistory {
namespace {
struct Change {
  Snapshot before, after;
};
std::vector<Change> undo, redo;
constexpr std::size_t Limit = 256;
std::uint64_t revision{};
bool Available(const Change &change, bool reverse) {
  const auto &target = reverse ? change.before : change.after;
  const auto &existing = reverse ? change.after : change.before;
  const auto object = target.object ? target.object : existing.object;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(
      target.object ? target.cell : existing.cell);
  const auto player = RE::PlayerCharacter::GetSingleton();
  return object && cell && cell->IsAttached() && player &&
         cell->GetRuntimeData().worldSpace == player->GetWorldspace() &&
         (player->GetWorldspace() || cell == player->GetParentCell());
}
bool Apply(const Change &change, bool reverse) {
  const auto &target = reverse ? change.before : change.after;
  const auto &existing = reverse ? change.after : change.before;
  const auto object = target.object ? target.object : existing.object;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(
      target.object ? target.cell : existing.cell);
  const auto player = RE::PlayerCharacter::GetSingleton();
  if (!Available(change, reverse)) {
    RE::SendHUDMessage::ShowHUDMessage(
        "Return to the edited area to undo or redo building.");
    return false;
  }
  PlacementPreview::CancelMove();
  if (BuildingManager::IsPlacedObject(object->GetFormID()) !=
      bool(existing.object)) {
    RE::SendHUDMessage::ShowHUDMessage(
        "SkyBuild: this object's state has changed; undo or redo cancelled.");
    return false;
  }
  const auto cost =
      BuildCosts::CostForTransition(bool(existing.object), bool(target.object));
  const auto &materials = target.object ? target.materials : existing.materials;
  if (cost != BuildCosts::TransitionCost::None && !materials) {
    RE::SendHUDMessage::ShowHUDMessage(
        "SkyBuild: the item's pack settings are unavailable.");
    return false;
  }
  BuildCosts::Payment payment;
  BuildCosts::Refund refund;
  if (cost == BuildCosts::TransitionCost::Pay && !payment.Pay(*materials, true))
    return false;
  if (cost == BuildCosts::TransitionCost::Refund &&
      !refund.Prepare(*materials, true))
    return false;
  if (!target.object) {
    if (!Containers::CanRemove(object.get()))
      return false;
    BuildingManager::RemovePlacedObject(object.get());
    object->Disable();
    refund.Grant();
    return true;
  }
  using MoveReference = void (*)(
      RE::TESObjectREFR *, const RE::ObjectRefHandle &, RE::TESObjectCELL *,
      RE::TESWorldSpace *, const RE::NiPoint3 &, const RE::NiPoint3 &);
  static REL::Relocation<MoveReference> move{RELOCATION_ID(56227, 56626)};
  object->Disable();
  move(object.get(), player->GetHandle(), cell,
       cell->GetRuntimeData().worldSpace, target.position, target.rotation);
  object->SetScale(target.scale);
  object->Update3DPosition(true);
  BuildingManager::RestoreBuilding(target.building);
  const auto &p = target.placement;
  BuildingManager::AddPlacedObject(
      object.get(), p.buildingID, p.gridPosition,
      p.sideY ? BuildingManager::GridObjectType::kSideY : p.type, p.grid,
      p.identity);
  Util::EnableWithFade(object.get(), false);
  BuildingManager::SetLastSeenBuilding(p.buildingID);
  payment.Commit();
  return true;
}
void Replay(std::vector<Change> &from, std::vector<Change> &to, bool reverse) {
  if (from.empty())
    return;
  const auto &change = from.back();
  const auto &target = reverse ? change.before : change.after;
  const auto &existing = reverse ? change.after : change.before;
  if (!target.object && Available(change, reverse) &&
      !Containers::CanRemove(existing.object.get())) {
    const auto expected = revision;
    Containers::RequestRemoval(existing.object.get(),
                               [&from, &to, reverse, expected] {
                                 if (revision == expected)
                                   Replay(from, to, reverse);
                               });
    return;
  }
  if (!Apply(from.back(), reverse))
    return;
  to.push_back(std::move(from.back()));
  from.pop_back();
  ++revision;
}
} // namespace
Snapshot Capture(RE::TESObjectREFR *object) {
  Snapshot result;
  if (!object)
    return result;
  const auto &placed = BuildingManager::GetPlacedObjects();
  const auto found = placed.find(object->GetFormID());
  if (found == placed.end())
    return result;
  const auto building = BuildingManager::GetBuilding(found->second.buildingID);
  if (!building || !object->GetParentCell())
    return result;
  result.object.reset(object);
  result.position = object->GetPosition();
  result.rotation = object->GetAngle();
  result.scale = object->GetScale();
  result.cell = object->GetParentCell()->GetFormID();
  result.placement = found->second;
  result.building = *building;
  if (const auto *definition =
          BuildingManager::GetPlaceableDefinition(object->GetFormID()))
    result.materials =
        BuildCosts::EffectiveMaterials(definition->requiredMaterials);
  else
    // Missing pack entries remain removable. Record an empty transaction so
    // undo restores them for free and redo cannot grant an unearned refund.
    result.materials.emplace();
  return result;
}
void Record(Snapshot before, Snapshot after) {
  if (!before.object && !after.object)
    return;
  ++revision;
  redo.clear();
  if (undo.size() == Limit)
    undo.erase(undo.begin());
  undo.push_back({std::move(before), std::move(after)});
}
void Undo() { Replay(undo, redo, true); }
void Redo() { Replay(redo, undo, false); }
bool CanUndo() { return !undo.empty() && Available(undo.back(), true); }
bool CanRedo() { return !redo.empty() && Available(redo.back(), false); }
void Clear() {
  ++revision;
  undo.clear();
  redo.clear();
}
} // namespace SkyBuild::BuildingHistory
