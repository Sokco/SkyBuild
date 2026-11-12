#include "containers/Containers.h"
#include "building/BuildingManager.h"
#include "containers/RackOffsets.h"
#include "containers/RackRules.h"
#include "models/ModelAsset.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace SkyBuild::Containers {
namespace {
constexpr std::size_t MaxRackSlots = 3;
constexpr std::string_view DisplayName = "SkyBuildRackVisual";
struct Display {
  RE::NiPointer<RE::NiNode> pivot;
  RE::NiPointer<RE::NiNode> attachment;
  RE::NiPointer<RE::NiAVObject> model;
  RE::FormID base{};
  void Clear() {
    if (attachment && attachment->parent)
      attachment->parent->DetachChild(attachment.get());
    model.reset();
    attachment.reset();
    pivot.reset();
    base = 0;
  }
  void Sync(RE::NiNode *root) {
    if (!attachment || !pivot || !root)
      return;
    auto *parent = root->parent;
    if (!parent) {
      attachment->SetAppCulled(true);
      return;
    }
    bool changed = attachment->parent != parent;
    if (changed) {
      if (attachment->parent)
        attachment->parent->DetachChild(attachment.get());
      parent->AttachChild(attachment.get(), true);
    }
    const auto local = parent->world.Invert() * pivot->world;
    const bool hidden = root->GetAppCulled();
    changed |=
        attachment->local != local || attachment->GetAppCulled() != hidden;
    attachment->local = local;
    attachment->SetAppCulled(hidden);
    if (changed) {
      RE::NiUpdateData update{};
      update.flags.set(RE::NiUpdateData::Flag::kDirty);
      attachment->Update(update);
      // Rack displays are placed/teleported, not animated between these
      // transforms. The asset's old (often origin-space) transforms must not
      // become motion vectors for temporal reflections or upscaling. Seed
      // every child after Update has calculated its final world transform.
      RE::BSVisit::TraverseScenegraphObjects(
          attachment.get(), [](RE::NiAVObject *node) {
            node->previousWorld = node->world;
            return RE::BSVisit::BSVisitControl::kContinue;
          });
    }
  }
};
struct Rack {
  RE::ObjectRefHandle handle;
  RE::NiPointer<RE::NiNode> root;
  std::vector<Display> displays;
  bool dirty{true};
  void ClearDisplay() {
    for (auto &display : displays)
      display.Clear();
    displays.clear();
    root.reset();
  }
};
std::unordered_map<RE::FormID, Rack> racks;
using Index = std::unordered_set<RE::FormID>;
std::atomic<std::shared_ptr<const Index>> index{
    std::make_shared<const Index>()};
std::atomic_uint64_t epoch{};
std::atomic_bool refreshQueued{};
bool removalPending{};
std::chrono::steady_clock::time_point nextUpdate{};
void PublishIndex() {
  auto next = std::make_shared<Index>();
  for (const auto &[id, rack] : racks)
    next->emplace(id);
  index.store(std::move(next));
}
void Notify(const char *text) { RE::SendHUDMessage::ShowHUDMessage(text); }
bool SafeBase(RE::TESObjectCONT *base) {
  return base &&
         SafeContainer(base->data.flags.all(RE::CONT_DATA::Flag::kRespawn),
                       base->numContainerObjects);
}
bool ManagedContainer(RE::TESObjectREFR *ref) {
  return ref && ref->IsDynamicForm() && ref->GetBaseObject() &&
         ref->GetBaseObject()->Is(RE::FormType::Container) &&
         BuildingManager::IsPlacedObject(ref->GetFormID());
}
DisplayType TypeOf(RE::TESBoundObject *item) {
  if (const auto armor = item ? item->As<RE::TESObjectARMO>() : nullptr)
    return (armor->IsShield() ||
            (armor->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kShield))
               ? DisplayType::Shield
               : DisplayType::None;
  const auto weapon = item ? item->As<RE::TESObjectWEAP>() : nullptr;
  if (!weapon || weapon->IsBound())
    return DisplayType::None;
  switch (static_cast<int>(weapon->GetWeaponType())) {
  case 1:
    return DisplayType::Sword;
  case 2:
    return DisplayType::Dagger;
  case 3:
    return DisplayType::WarAxe;
  case 4:
    return DisplayType::Mace;
  case 5:
    return DisplayType::GreatSword;
  case 6: {
    const auto keyword =
        RE::TESForm::LookupByEditorID<RE::BGSKeyword>("WeapTypeWarhammer");
    return keyword && weapon->HasKeyword(keyword) ? DisplayType::Warhammer
                                                  : DisplayType::BattleAxe;
  }
  case 7:
    return DisplayType::Bow;
  case 8:
    return DisplayType::Staff;
  case 9:
    return DisplayType::Crossbow;
  default:
    return DisplayType::None;
  }
}
const char *ModelPath(RE::TESBoundObject *item) {
  if (const auto armor = item->As<RE::TESObjectARMO>()) {
    const auto male =
        armor->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
    if (male && *male)
      return male;
    return armor->worldModels[RE::TESBipedModelForm::Sexes::kFemale].GetModel();
  }
  const auto model = item->As<RE::TESModel>();
  return model ? model->GetModel() : nullptr;
}
RE::NiNode *FindAuthoredNode(RE::NiAVObject *object, std::string_view name,
                             RE::NiNode *slotScope = nullptr) {
  if (!object)
    return nullptr;
  const auto objectName = object->name.c_str();
  const std::string_view nodeName = objectName ? objectName : "";
  if (nodeName == DisplayName ||
      (slotScope && object != slotScope && nodeName.starts_with("RackPivot")))
    return nullptr;
  auto *node = object->AsNode();
  if (!node)
    return nullptr;
  if (nodeName == name)
    return node;
  for (const auto &child : node->GetChildren())
    if (auto *found = FindAuthoredNode(child.get(), name, slotScope))
      return found;
  return nullptr;
}
struct Slot {
  RE::NiNode *scope{};
  bool authored{};
};
std::vector<Slot> FindSlots(RE::TESObjectREFR *reference) {
  std::vector<Slot> result;
  const auto root = reference ? reference->Get3D() : nullptr;
  if (!root)
    return result;
  for (const auto name :
       {"RackPivotSingle", "RackPivotDual01", "RackPivotDual02"})
    if (auto *node = FindAuthoredNode(root, name))
      result.push_back({node, true});
  // A partial authored layout cannot safely represent all occupancy modes.
  if (result.size() != MaxRackSlots)
    result.clear();
  // The original model is one physical slot, regardless of how many
  // alternative item pivots it contains.
  if (result.empty() && root->AsNode())
    result.push_back({root->AsNode(), false});
  return result;
}
RE::NiNode *FindPivot(const Slot &slot, RE::TESBoundObject *item) {
  const auto kind = TypeOf(item);
  if (kind == DisplayType::None)
    return nullptr;
  // A measured type offset on a staff anchor is more accurate than an
  // unrelated fallback pivot. Explicit type pivots still win.
  if (auto *exact = FindAuthoredNode(slot.scope, NodeName(kind),
                                     slot.authored ? slot.scope : nullptr))
    return exact;
  if (auto *staff = FindAuthoredNode(slot.scope, "StaffPivot01",
                                     slot.authored ? slot.scope : nullptr))
    if (kind != DisplayType::Shield)
      return staff;
  const auto name = SelectPivot(kind, [&](std::string_view candidate) {
    return FindAuthoredNode(slot.scope, candidate,
                            slot.authored ? slot.scope : nullptr) != nullptr;
  });
  if (!name.empty())
    return FindAuthoredNode(slot.scope, name,
                            slot.authored ? slot.scope : nullptr);
  // A named physical slot is itself an explicit generic display transform.
  return slot.authored ? slot.scope : nullptr;
}
bool Fits(const std::vector<Slot> &slots, RE::TESBoundObject *item) {
  const auto path = item ? ModelPath(item) : nullptr;
  return path && *path && std::ranges::any_of(slots, [&](const Slot &slot) {
           return FindPivot(slot, item) != nullptr;
         });
}
RE::NiPointer<RE::NiAVObject> LoadDisplay(RE::TESBoundObject *item) {
  RE::NiPointer<RE::NiAVObject> model{Models::LoadWorldModel(item)};
  if (!model)
    return {};
  RE::BSVisit::TraverseScenegraphObjects(model.get(), [](RE::NiAVObject *node) {
    node->collisionObject.reset();
    node->SetUserData(nullptr);
    return RE::BSVisit::BSVisitControl::kContinue;
  });
  if (item->As<RE::TESObjectWEAP>()) {
    // Strip only from the private visual, never the cached NIF or real item.
    // Collect first: detaching while traversing invalidates the child array.
    std::vector<RE::NiPointer<RE::NiAVObject>> scabbards;
    RE::BSVisit::TraverseScenegraphObjects(
        model.get(), [&](RE::NiAVObject *node) {
          const auto name = node->name.c_str();
          if (node != model.get() && name && IsScabbardBranch(name))
            scabbards.emplace_back(node);
          return RE::BSVisit::BSVisitControl::kContinue;
        });
    for (const auto &scabbard : scabbards)
      if (scabbard->parent)
        scabbard->parent->DetachChild(scabbard.get());
  }
  // This visual-only model does not participate in reference-driven fading.
  model->SetAppCulled(false);
  model->GetFlags().set(RE::NiAVObject::Flag::kIgnoreFade);
  if (auto *fade = model->AsFadeNode())
    fade->GetRuntimeData().currentFade = 1.F;
  model->name = RE::BSFixedString(DisplayName);
  model->local.translate = {};
  model->local.rotate.SetEulerAnglesXYZ({});
  model->local.scale = 1.F;
  return model;
}
void Refresh(Rack &rack) {
  const auto owner = rack.handle.get();
  const auto root = owner ? owner->Get3D() : nullptr;
  auto *rootNode = root ? root->AsNode() : nullptr;
  if (!owner || owner->IsDeleted() || owner->IsDisabled() || !rootNode) {
    rack.ClearDisplay();
    return;
  }
  if (!rack.dirty && rack.root.get() == rootNode)
    return;
  rack.dirty = false;
  if (!ManagedContainer(owner.get()) &&
      !SafeBase(owner->GetBaseObject()->As<RE::TESObjectCONT>())) {
    rack.ClearDisplay();
    return;
  }
  if (rack.root.get() != rootNode) {
    rack.ClearDisplay();
    rack.root.reset(rootNode);
  }
  const auto slots = FindSlots(owner.get());
  const auto inventory = owner->GetInventory();
  struct Item {
    RE::TESBoundObject *base;
    int remaining;
  };
  std::vector<Item> items;
  for (const auto &[item, data] : inventory)
    if (data.first > 0 && Fits(slots, item))
      items.push_back({item, data.first});
  std::ranges::sort(items, {},
                    [](const Item &item) { return item.base->GetFormID(); });
  const auto displayCount = slots.size() == 3 ? 4 : slots.size();
  if (rack.displays.size() != displayCount) {
    rack.ClearDisplay();
    rack.root.reset(rootNode);
    rack.displays.resize(displayCount);
  }
  std::vector<RE::TESBoundObject *> assignments(displayCount);
  int count{};
  bool hasShield{};
  for (const auto &item : items) {
    if (TypeOf(item.base) != DisplayType::Shield)
      count += std::min(item.remaining, 3);
    hasShield |= TypeOf(item.base) == DisplayType::Shield;
  }
  const auto active = slots.size() == 1 ? std::array{count > 0, false, false}
                                        : ActiveRackPivots(count);
  // Shield has an independent display attached to Single.
  if (hasShield)
    for (auto &item : items)
      if (item.remaining > 0 && TypeOf(item.base) == DisplayType::Shield) {
        assignments[displayCount - 1] = item.base;
        --item.remaining;
        break;
      }
  // Preserve weapon positions where the new occupancy layout allows it.
  for (std::size_t i = 0; i < slots.size(); ++i)
    if (active[i] && !assignments[i])
      for (auto &item : items)
        if (item.remaining > 0 && TypeOf(item.base) != DisplayType::Shield &&
            rack.displays[i].base == item.base->GetFormID() &&
            FindPivot(slots[i], item.base)) {
          assignments[i] = item.base;
          --item.remaining;
          break;
        }
  for (std::size_t i = 0; i < slots.size(); ++i)
    if (active[i] && !assignments[i])
      for (auto &item : items)
        if (item.remaining > 0 && TypeOf(item.base) != DisplayType::Shield &&
            FindPivot(slots[i], item.base)) {
          assignments[i] = item.base;
          --item.remaining;
          break;
        }
  for (std::size_t i = 0; i < displayCount; ++i) {
    auto &display = rack.displays[i];
    auto *item = assignments[i];
    auto *pivot =
        item ? FindPivot(slots[i < slots.size() ? i : 0], item) : nullptr;
    if (!item || !pivot) {
      display.Clear();
      continue;
    }
    if (display.base == item->GetFormID() && display.pivot.get() == pivot &&
        display.model)
      continue;
    display.Clear();
    auto model = LoadDisplay(item);
    if (!model)
      continue;
    const auto &slot = slots[i < slots.size() ? i : 0];
    const auto pivotName = pivot->name.c_str();
    if ((slot.authored && pivot == slot.scope) ||
        (pivotName && std::string_view(pivotName) == "StaffPivot01")) {
      const auto offset = StaffRelativeOffset(TypeOf(item));
      model->local.translate = {offset.translation[0], offset.translation[1],
                                offset.translation[2]};
      for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
          model->local.rotate.entry[row][col] = offset.rotation[row * 3 + col];
    }
    // Keep reference-specific additions out of the native/cached rack graph.
    // The separate attachment follows the read-only authored pivot in world
    // space.
    display.attachment.reset(RE::NiNode::Create(1));
    if (!display.attachment)
      continue;
    display.attachment->name = RE::BSFixedString(DisplayName);
    display.attachment->AttachChild(model.get(), true);
    display.pivot.reset(pivot);
    display.model = std::move(model);
    display.base = item->GetFormID();
    display.Sync(rootNode);
  }
}
void QueueRefresh() {
  if (refreshQueued.exchange(true))
    return;
  const auto generation = epoch.load();
  if (const auto tasks = SKSE::GetTaskInterface())
    tasks->AddTask([generation] {
      if (generation != epoch.load())
        return;
      refreshQueued.store(false);
      for (auto &[id, rack] : racks) {
        rack.dirty = true;
        Refresh(rack);
      }
    });
  else
    refreshQueued.store(false);
}
class ContainerEvents : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESContainerChangedEvent *event,
               RE::BSTEventSource<RE::TESContainerChangedEvent> *) override {
    const auto current = index.load();
    if (event && (current->contains(event->newContainer) ||
                  current->contains(event->oldContainer)))
      QueueRefresh();
    return RE::BSEventNotifyControl::kContinue;
  }
};
bool Configure(RE::StaticFunctionTag *, RE::TESObjectREFR *reference) {
  if (!reference ||
      (!ManagedContainer(reference) &&
       !SafeBase(reference->GetBaseObject()->As<RE::TESObjectCONT>())))
    return false;
  const auto handle = reference->GetHandle();
  const auto generation = epoch.load();
  const auto tasks = SKSE::GetTaskInterface();
  if (!tasks)
    return false;
  tasks->AddTask([handle, generation] {
    const auto object = handle.get();
    if (!object || generation != epoch.load())
      return;
    auto &rack = racks[object->GetFormID()];
    rack.handle = handle;
    rack.dirty = true;
    PublishIndex();
    Refresh(rack);
  });
  return true;
}
void Forget(RE::StaticFunctionTag *, RE::TESObjectREFR *reference) {
  if (!reference)
    return;
  const auto id = reference->GetFormID();
  const auto generation = epoch.load();
  if (const auto tasks = SKSE::GetTaskInterface())
    tasks->AddTask([id, generation] {
      if (generation != epoch.load())
        return;
      if (const auto found = racks.find(id); found != racks.end()) {
        const auto object = found->second.handle.get();
        // An old Papyrus OnUnload may finish after the new OnLoad registered
        // this rack. Do not erase a newer, already loaded display.
        if (object && object->Is3DLoaded() && !object->IsDisabled())
          return;
        found->second.ClearDisplay();
        racks.erase(found);
        PublishIndex();
      }
    });
}
using Processor = RE::FxDelegateHandler::CallbackProcessor;
using Callback = RE::FxDelegateHandler::CallbackFn;
using Accept = void (*)(RE::ContainerMenu *, Processor *);
REL::Relocation<Accept> originalAccept;
Callback *originalTransfer{};
void Transfer(const RE::FxDelegateArgs &args) {
  const auto target = RE::TESObjectREFR::LookupByHandle(
      RE::ContainerMenu::GetTargetRefHandle());
  const auto current = index.load();
  const auto found =
      target ? current->find(target->GetFormID()) : current->end();
  if (found == current->end()) {
    if (originalTransfer)
      originalTransfer(args);
    return;
  }
  if (args.GetArgCount() != 2 || !args[0].IsNumber() || !args[1].IsBool())
    return;
  if (!std::isfinite(args[0].GetNumber()) || args[0].GetNumber() < 1)
    return;
  const bool taking = args[1].GetBool();
  double amount = args[0].GetNumber();
  if (!taking) {
    const auto menu = RE::UI::GetSingleton()->GetMenu<RE::ContainerMenu>();
    auto *list = menu ? menu->GetRuntimeData().itemList : nullptr;
    auto *selected = list ? list->GetSelectedItem() : nullptr;
    const auto entry = selected ? selected->data.objDesc : nullptr;
    const auto slots = FindSlots(target.get());
    if (!entry || entry->IsQuestObject() || !Fits(slots, entry->object)) {
      Notify("This item does not fit the rack.");
      return;
    }
  }
  RE::GFxValue values[2]{args[0], args[1]};
  values[0].SetNumber(amount);
  struct Forward : RE::FxDelegateArgs {
    Forward(const RE::FxDelegateArgs &source, const RE::GFxValue *values)
        : RE::FxDelegateArgs(source) {
      _args = values;
      _numArgs = 2;
    }
  } forward(args, values);
  // Keep Skyrim's selected-entry transfer. Never replace it with a base-form
  // AddItem/RemoveItem pair: that would lose exact inventory-instance
  // selection.
  if (originalTransfer)
    originalTransfer(forward);
  QueueRefresh();
}
struct Proxy : Processor {
  Processor *destination;
  explicit Proxy(Processor *value) : destination(value) {}
  void Process(const RE::GString &name, Callback *callback) override {
    if (std::string_view(name.c_str()) == "ItemTransfer") {
      originalTransfer = callback;
      destination->Process(name, Transfer);
    } else
      destination->Process(name, callback);
  }
};
void AcceptMenu(RE::ContainerMenu *menu, Processor *processor) {
  Proxy proxy(processor);
  originalAccept(menu, &proxy);
}
} // namespace
bool RegisterPapyrus(RE::BSScript::IVirtualMachine *vm) {
  if (!vm)
    return false;
  vm->RegisterFunction("ConfigureWeaponRack", "SkyBuildNative", Configure);
  vm->RegisterFunction("ForgetWeaponRack", "SkyBuildNative", Forget);
  return true;
}
void Initialize() {
  static ContainerEvents listener;
  if (const auto events = RE::ScriptEventSourceHolder::GetSingleton())
    events->AddEventSink<RE::TESContainerChangedEvent>(&listener);
  REL::Relocation<std::uintptr_t> table{RE::VTABLE_ContainerMenu[0]};
  originalAccept = table.write_vfunc(1, AcceptMenu);
}
void Restore() {
  const auto generation = epoch.load();
  if (const auto tasks = SKSE::GetTaskInterface())
    tasks->AddTask([generation] {
      if (generation != epoch.load()) return;
      auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
      auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
      if (!policy) return;
      // OnLoad need not repeat for references restored in the saved cell.
      for (const auto &ref : BuildingManager::GetResolvedPlacedObjects()) {
        const auto *base = ref->GetBaseObject();
        if (!base || !base->Is(RE::FormType::Container) || ref->IsDeleted()) continue;
        RE::BSTSmartPointer<RE::BSScript::Object> script;
        const auto handle = policy->GetHandleForObject(ref->GetFormType(), ref.get());
        if (vm->FindBoundObject(handle, "SkyBuildCombinedWeaponRack", script))
          Configure(nullptr, ref.get());
      }
    });
}
void Update() {
  if (racks.empty())
    return;
  const auto now = std::chrono::steady_clock::now();
  const bool refresh = now >= nextUpdate;
  if (refresh)
    nextUpdate = now + std::chrono::milliseconds(500);
  for (auto &[id, rack] : racks) {
    if (refresh)
      Refresh(rack);
    const auto owner = rack.handle.get();
    if (!owner || owner->IsDeleted() || owner->IsDisabled()) {
      rack.ClearDisplay();
      continue;
    }
    for (auto &display : rack.displays)
      display.Sync(rack.root.get());
  }
}
void Reset() {
  ++epoch;
  refreshQueued.store(false);
  removalPending = false;
  for (auto &[id, rack] : racks)
    rack.ClearDisplay();
  racks.clear();
  PublishIndex();
  nextUpdate = {};
}
bool PrepareNewPlacement(RE::TESObjectREFR *ref) {
  auto *base = ref->GetBaseObject()->As<RE::TESObjectCONT>();
  if (!base)
    return true;
  if (!base->numContainerObjects)
    return true;

  // Materialize leveled loot, then remove the resolved items from this new
  // instance. LVLI forms themselves are templates, not inventory items.
  for (const auto &[item, count] : ref->GetInventoryCounts()) {
    if (count > 0 && !item->Is(RE::FormType::LeveledItem))
      ref->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr,
                      nullptr);
  }
  if (!CanRemove(ref)) {
    Notify("Could not empty the new container.");
    return false;
  }
  return true;
}
bool CanRemove(RE::TESObjectREFR *reference) {
  if (!reference || !reference->GetBaseObject()->As<RE::TESObjectCONT>())
    return true;
  for (const auto &[item, data] : reference->GetInventory())
    if (data.first > 0 && !item->Is(RE::FormType::LeveledItem)) {
      return false;
    }
  return true;
}

namespace {
void TakeContents(RE::TESObjectREFR *container, RE::PlayerCharacter *player) {
  // Preserve each inventory instance's enchantment, tempering, name and extras.
  // GetInventory owns a snapshot of the lists, so transfers cannot invalidate
  // the iteration over the container's live inventory.
  auto inventory = container->GetInventory();
  for (auto &[item, data] : inventory) {
    auto &[count, entry] = data;
    if (count <= 0 || !entry || item->Is(RE::FormType::LeveledItem))
      continue;
    int remaining = count;
    if (entry->extraLists)
      for (auto *extra : *entry->extraLists) {
        if (!extra || remaining <= 0)
          continue;
        const int amount = std::clamp(extra->GetCount(), 0, remaining);
        if (amount)
          container->RemoveItem(item, amount,
                                RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                extra, player);
        remaining -= amount;
      }
    if (remaining > 0)
      container->RemoveItem(item, remaining,
                            RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr,
                            player);
  }
}
class RemovalChoice final : public RE::IMessageBoxCallback {
public:
  RE::ObjectRefHandle handle;
  RE::FormID cell{};
  std::uint64_t generation{};
  std::function<void()> remove;
  void Run(std::uint8_t button) override {
    if (auto *tasks = SKSE::GetTaskInterface())
      tasks->AddTask([handle = handle, cell = cell, generation = generation,
                      remove = std::move(remove), button] {
        if (generation != epoch.load())
          return;
        removalPending = false;
        if (button != 0)
          return;
        const auto object = handle.get();
        auto *player = RE::PlayerCharacter::GetSingleton();
        if (!object || !player || object->IsDeleted() || object->IsDisabled() ||
            !object->GetParentCell() ||
            object->GetParentCell()->GetFormID() != cell ||
            !object->GetParentCell()->IsAttached())
          return;
        TakeContents(object.get(), player);
        // Scripts may reject a transfer or insert something new. Never disable
        // a container that still holds inventory, even after confirmation.
        if (!CanRemove(object.get())) {
          Notify(
              "Some items could not be taken. The container was not removed.");
          return;
        }
        if (remove)
          remove();
      });
  }
};
} // namespace
void RequestRemoval(RE::TESObjectREFR *reference,
                    std::function<void()> remove) {
  if (!reference || removalPending)
    return;
  if (CanRemove(reference)) {
    if (remove)
      remove();
    return;
  }
  auto *choice = new RemovalChoice;
  choice->handle = reference->GetHandle();
  choice->cell =
      reference->GetParentCell() ? reference->GetParentCell()->GetFormID() : 0;
  choice->generation = epoch.load();
  choice->remove = std::move(remove);
  RE::BSTSmartPointer<RE::IMessageBoxCallback> callback{choice};
  RE::BSString message(
      "This container contains items and cannot be removed yet. Take all items and remove it?");
  RE::BSTArray<RE::BSString> buttons;
  buttons.push_back(RE::BSString("Take Items"));
  buttons.push_back(RE::BSString("Cancel"));
  removalPending = true;
  if (!RE::MessageBoxMenu::Create(message, callback, 0, 0, 0, buttons))
    removalPending = false;
}
} // namespace SkyBuild::Containers
