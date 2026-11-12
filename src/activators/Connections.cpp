#include "ui/thumbnails/ThumbnailGenerator.h"
#include "activators/Connections.h"
#include "activators/ConnectionIndex.h"
#include "naming/ReferenceNames.h"
#include "building/BuildingManager.h"
#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "physics/Raycast.h"
#include "terrain/TerrainTool.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/shared/AutosaveQueue.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>

#undef GetObject // Windows API macro conflicts with BSScript::Variable.

namespace SkyBuild::Activators {
namespace {
using Registry = std::vector<Connection>;
std::atomic<std::shared_ptr<const ConnectionSnapshot>> registry{
    std::make_shared<ConnectionSnapshot>()};
std::atomic_uint64_t epoch{1};
std::atomic_uint32_t pendingSource{}, requestedSource{};
std::atomic_bool available{};
UI::Native::Model::WindowInterface *window{}, *connectionWindow{};
std::atomic<std::shared_ptr<const Connection>> requestedConnection;
std::unique_ptr<UI::Native::Model::HudElement> hud;
struct Object {
  RE::FormID id{};
  std::string name;
  std::string displayName;
};
struct PropertyDraft {
  RE::FormID id;
  std::string name;
};
UI::AutosaveQueue<PropertyDraft> edits;
std::atomic_bool editPumpQueued{};
void FlushEdits();
void RequestEditPump();
struct View {
  std::uint64_t generation{};
  Registry connections;
  std::vector<Object> sources, items;
  std::map<RE::FormID, std::string> targetNames;
  std::map<RE::FormID, std::string> targetIcons;
  std::string notice;
};
std::atomic<std::shared_ptr<const View>> view{std::make_shared<View>()};
std::string notice;
std::optional<std::string>
    pendingName; // Game-thread draft while choosing a world target.
struct Wire {
  RE::NiPoint3 source, end;
  float matrix[4][4]{};
  RE::NiRect<float> viewport;
  std::string target;
  bool valid{};
};
std::atomic<std::shared_ptr<const Wire>> wire;
RE::TESObjectREFR *Resolve(RE::FormID id) {
  auto *ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
  return ref && !ref->IsDeleted() && !ref->IsDisabled() &&
                 BuildingManager::IsPlacedObject(id)
             ? ref
             : nullptr;
}
std::optional<Target> Kind(RE::TESObjectREFR *ref) {
  if (!ref || !ref->GetBaseObject())
    return {};
  if (ref->GetBaseObject()->As<RE::TESObjectDOOR>())
    return Target::Door;
  if (ref->GetBaseObject()->As<RE::TESObjectCONT>())
    return Target::Container;
  if (SupportsProperties(ref))
    return Target::Usable;
  return {};
}
std::string Name(RE::FormID id) {
  auto *ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
  if (ref) {
    if (const auto *name = ref->GetDisplayFullName(); name && *name)
      return name;
    if (const auto *base = ref->GetBaseObject()) {
      if (const auto *name = base->GetName(); name && *name)
        return name;
      if (const auto *editor = base->GetFormEditorID(); editor && *editor)
        return editor;
      using Lookup = const char *(*)(std::uint32_t);
      static const auto lookup = []() -> Lookup {
        const auto module = GetModuleHandleW(L"po3_Tweaks.dll");
        return module ? reinterpret_cast<Lookup>(
                            GetProcAddress(module, "GetFormEditorID"))
                      : nullptr;
      }();
      if (lookup)
        if (const auto *editor = lookup(base->GetFormID()); editor && *editor)
          return editor;
    }
  }
  return std::format("Object {:08X}", id);
}
void CollectItems(View &model, RE::TESObjectREFR *reference) {
  if (!reference)
    return;
  for (const auto &[object, count] : reference->GetInventoryCounts())
    if (object && count > 0 && object->GetName() && *object->GetName() &&
        std::ranges::find(model.items, object->GetFormID(), &Object::id) ==
            model.items.end())
      model.items.push_back({object->GetFormID(), object->GetName()});
  std::ranges::sort(model.items, {}, &Object::name);
}
void Publish(RE::FormID itemSource = 0, RE::FormID connectionSource = 0) {
  auto next = std::make_shared<View>();
  next->generation = epoch.load();
  next->connections = registry.load()->connections;
  next->notice = notice;
  const auto *player = RE::PlayerCharacter::GetSingleton();
  constexpr float propertiesRadius = 8192.F;
  for (const auto &ref : BuildingManager::GetResolvedPlacedObjects())
    if (player && SupportsProperties(ref.get()) && !ref->IsDisabled() &&
        (ref->GetParentCell() == player->GetParentCell() ||
         (player->GetWorldspace() &&
          ref->GetWorldspace() == player->GetWorldspace())) &&
        ref->GetPosition().GetDistance(player->GetPosition()) <=
            propertiesRadius)
      next->sources.push_back(
          {ref->GetFormID(), Name(ref->GetFormID()),
           ref->GetDisplayFullName() ? ref->GetDisplayFullName() : ""});
  for (const auto &c : next->connections) {
    next->targetNames[c.target] = Name(c.target);
    next->targetNames[c.source] = Name(c.source);
  }
  if (itemSource)
    next->targetNames[itemSource] = Name(itemSource);
  if (connectionSource)
    next->targetNames[connectionSource] = Name(connectionSource);
  for (const auto &[id, name] : next->targetNames)
    if (const auto ref = Resolve(id); ref && ref->GetBaseObject())
      if (const auto path = UI::ThumbnailGenerator::GetRelativePath(
              ref->GetBaseObject()->GetFormID())) {
        next->targetIcons[id] = *path;
      }
  CollectItems(*next, RE::PlayerCharacter::GetSingleton());
  if (auto *target = Resolve(itemSource);
      target && target->GetBaseObject()->As<RE::TESObjectCONT>())
    CollectItems(*next, target);
  std::ranges::sort(next->sources, [](const Object &a, const Object &b) {
    return a.name != b.name ? a.name < b.name : a.id < b.id;
  });
  std::map<std::string, std::size_t> nameCounts;
  for (const auto &object : next->sources)
    ++nameCounts[object.name];
  for (auto &object : next->sources)
    if (nameCounts.at(object.name) > 1)
      object.name = std::format("{} ({:08X})", object.name, object.id);
  view.store(std::move(next));
}
void Edit(RE::FormID source, std::optional<std::string> nameDraft = {}) {
  FlushEdits();
  Cancel();
  requestedSource.store(source);
  notice.clear();
  const auto current = registry.load();
  const auto found =
      std::ranges::find(current->connections, source, &Connection::source);
  Publish(found != current->connections.end() ? found->target : 0);
  if (nameDraft) {
    auto model = std::make_shared<View>(*view.load());
    const auto object = std::ranges::find(model->sources, source, &Object::id);
    if (object != model->sources.end())
      object->displayName = *nameDraft;
    view.store(std::move(model));
  }
  if (window)
    window->IsOpen.store(true);
}
RE::NiPointer<RE::TESObjectREFR> Aim() {
  for (const auto &hit :
       Physics::CastSelectionRay(2048.F, [](RE::TESObjectREFR *ref) {
         return BuildingManager::IsPlacedObject(ref->GetFormID()) &&
                SupportsProperties(ref);
       })) {
    if (!hit.didHit)
      continue;
    // Temporary placement previews must not become connection endpoints.
    if (hit.hitReference &&
        !BuildingManager::IsPlacedObject(hit.hitReference->GetFormID()))
      return {};
    return hit.hitReference;
  }
  return {};
}
class Result final : public RE::BSScript::IStackCallbackFunctor {
public:
  explicit Result(std::function<void(RE::BSScript::Variable)> fn)
      : callback(std::move(fn)) {}
  void operator()(RE::BSScript::Variable result) override { callback(result); }
  void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object> &) override {}

private:
  std::function<void(RE::BSScript::Variable)> callback;
};
template <class... Args>
bool Call(RE::TESObjectREFR *ref, const char *method,
          RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result,
          Args... args) {
  auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
  auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  if (!policy || !ref)
    return false;
  RE::BSScript::Variable receiver;
  receiver.Pack(ref);
  auto object = receiver.GetObject();
  if (!object)
    return false;
  return vm->DispatchMethodCall(
      object, method, RE::MakeFunctionArguments(std::move(args)...), result);
}
bool Matches(RE::TESBoundObject *object, const Connection &c) {
  switch (c.filter) {
  case Filter::All:
    return true;
  case Filter::Ingredients:
    return object->As<RE::IngredientItem>();
  case Filter::Food: {
    auto *item = object->As<RE::AlchemyItem>();
    return item && item->IsFood();
  }
  case Filter::Potions: {
    auto *item = object->As<RE::AlchemyItem>();
    return item && !item->IsFood() && !item->IsPoison();
  }
  case Filter::Poisons: {
    auto *item = object->As<RE::AlchemyItem>();
    return item && item->IsPoison();
  }
  case Filter::Weapons:
    return object->As<RE::TESObjectWEAP>();
  case Filter::Armor:
    return object->As<RE::TESObjectARMO>();
  case Filter::Ammunition:
    return object->As<RE::TESAmmo>();
  case Filter::Books:
    return object->As<RE::TESObjectBOOK>();
  case Filter::SoulGems:
    return object->As<RE::TESSoulGem>();
  case Filter::SpecificItem:
    return object->GetFormID() == c.item;
  case Filter::SmithingMaterials:
    return object->HasKeywordByEditorID("VendorItemOreIngot") ||
           object->HasKeywordByEditorID("VendorItemAnimalHide") ||
           object->HasKeywordByEditorID("VendorItemLeather") ||
           object->GetFormID() == 0x800E4; // Leather strips.
  case Filter::Valuables:
    return object->HasKeywordByEditorID("VendorItemGem") ||
           object->HasKeywordByEditorID("VendorItemJewelry");
  }
  return false;
}
// Carry the reset epoch through deferred calls so actions cannot cross loads.
void Execute(const Connection &c, std::uint64_t generation) {
  if (epoch.load() != generation || !c.enabled ||
      !SupportsProperties(Resolve(c.source)))
    return;
  auto *target = Resolve(c.target);
  const auto kind = Kind(target);
  auto *player = RE::PlayerCharacter::GetSingleton();
  if (!player || !kind || *kind != c.kind) {
    RE::SendHUDMessage::ShowHUDMessage(
        "SkyBuild: connection target is unavailable.");
    return;
  }
  if (c.action == Action::Activate) {
    target->ActivateRef(Resolve(c.source), 0, nullptr, 1, false);
    return;
  }
  if (c.kind == Target::Door) {
    if (c.action == Action::ToggleDoor) {
      RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{
          new Result([id = c.target,
                      generation](RE::BSScript::Variable result) {
            if (!result.IsInt())
              return;
            const bool open = result.GetSInt() != 1 && result.GetSInt() != 2;
            SKSE::GetTaskInterface()->AddTask([id, generation, open] {
              if (epoch.load() == generation)
                if (auto *ref = Resolve(id))
                  Call(ref, "SetOpen", {}, open);
            });
          })};
      if (!Call(target, "GetOpenState", std::move(callback)))
        RE::SendHUDMessage::ShowHUDMessage(
            "SkyBuild: door action could not be started.");
    } else if (c.action == Action::OpenDoor || c.action == Action::CloseDoor) {
      Call(target, "SetOpen", {}, c.action == Action::OpenDoor);
    } else {
      const bool locked =
          c.action == Action::LockDoor ||
          (c.action == Action::ToggleLock && !target->IsLocked());
      Call(target, "Lock", {}, locked, false);
    }
    return;
  }
  if (target->IsLocked()) {
    RE::SendHUDMessage::ShowHUDMessage(
        "SkyBuild: the connected container is locked.");
    return;
  }
  if (c.action == Action::OpenContainer) {
    target->ActivateRef(player, 0, nullptr, 1, true);
    return;
  }
  auto *from = c.action == Action::Deposit
                   ? static_cast<RE::TESObjectREFR *>(player)
                   : target;
  auto *to = c.action == Action::Deposit
                 ? target
                 : static_cast<RE::TESObjectREFR *>(player);
  std::int64_t moved = 0;
  const auto inventory = from->GetInventory();
  for (const auto &[object, entry] : inventory) {
    const auto &[count, data] = entry;
    if (!object || count <= 0 || !data || !Matches(object, c) ||
        data->IsQuestObject() || data->IsWorn() || data->IsFavorited())
      continue;
    const int amount =
        c.limit ? std::min(count, static_cast<int>(c.limit - moved)) : count;
    if (amount <= 0)
      break;
    from->RemoveItem(object, amount, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                     nullptr, to);
    moved += amount;
    if (c.limit && moved >= c.limit)
      break;
  }
  RE::SendHUDMessage::ShowHUDMessage(
      std::format("SkyBuild: transferred {} item{}.", moved,
                  moved == 1 ? "" : "s")
          .c_str());
}
class ActivationListener final : public RE::BSTEventSink<RE::TESActivateEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESActivateEvent *event,
               RE::BSTEventSource<RE::TESActivateEvent> *) override {
    if (event && event->objectActivated) {
      const auto current = registry.load();
      for (const auto index :
           current->Outgoing(event->objectActivated->GetFormID())) {
        const auto connection = current->connections[index];
        const auto generation = epoch.load();
        SKSE::GetTaskInterface()->AddTask(
            [connection, generation] { Execute(connection, generation); });
      }
    }
    return RE::BSEventNotifyControl::kContinue;
  }
};
void Store(Connection c) {
  if (!available.load() || !Valid(c) ||
      !SupportsProperties(Resolve(c.source)) ||
      Kind(Resolve(c.target)) != c.kind) {
    notice = "The source or target is no longer available, or the connection "
             "is invalid.";
    Publish();
    return;
  }
  if (CreatesCycle(registry.load()->connections, c)) {
    notice = "This connection would create an activation loop.";
    Publish();
    return;
  }
  auto next = std::make_shared<Registry>(registry.load()->connections);
  auto found = std::ranges::find_if(*next, [&](const auto &entry) {
    return ConnectionKey(entry) == ConnectionKey(c);
  });
  if (found == next->end()) {
    if (next->size() >= MaxConnections) {
      notice = "Connection limit reached.";
      Publish();
      return;
    }
    next->push_back(c);
  } else
    *found = c;
  registry.store(std::make_shared<ConnectionSnapshot>(std::move(*next)));
  notice = "Connection saved.";
  Publish(c.target);
}
void Remove(Connection connection) {
  auto next = std::make_shared<Registry>(registry.load()->connections);
  std::erase_if(*next, [&](const auto &c) {
    return ConnectionKey(c) == ConnectionKey(connection);
  });
  registry.store(std::make_shared<ConnectionSnapshot>(std::move(*next)));
  notice =
      "Connection removed. The object keeps its normal activation behavior.";
  Publish();
}

constexpr std::array<const char *, 10> actionNames{"Open",
                                                   "Close",
                                                   "Toggle open / closed",
                                                   "Lock",
                                                   "Unlock",
                                                   "Toggle locked / unlocked",
                                                   "Open container",
                                                   "Deposit matching items",
                                                   "Retrieve matching items",
                                                   "Activate"};
constexpr std::array<const char *, 13> filterNames{
    "All items",    "Ingredients", "Food",
    "Potions",      "Poisons",     "Smithing materials",
    "Weapons",      "Armor",       "Ammunition",
    "Books",        "Soul gems",   "Gems and jewelry",
    "Specific item"};
void SaveProperties(RE::FormID id, std::string name) {
  auto *object = Resolve(id);
  const auto cleaned = CleanObjectName(std::move(name));
  if (!object || !SupportsProperties(object) || !cleaned) {
    notice = "Choose a usable placed object and enter a name (1-128 bytes).";
    Publish();
    return;
  }
  const std::string oldName =
      object->GetDisplayFullName() ? object->GetDisplayFullName() : "";
  if (*cleaned != oldName) {
    const auto *text = object->extraList.GetByType<RE::ExtraTextDisplayData>();
    if ((text && (text->ownerQuest || text->displayNameText)) ||
        !object->SetDisplayName(RE::BSFixedString(*cleaned), false)) {
      notice = "This object's name is controlled by another game system.";
      Publish();
      return;
    }
    object->AddChange(RE::TESObjectREFR::ChangeFlags::kCreatedOnlyExtra |
                      RE::TESObjectREFR::ChangeFlags::kGameOnlyExtra);
  }
  Naming::Set(id, *cleaned);
  notice = "Properties saved.";
  Publish();
}
void FlushEdits() {
  for (auto &edit : edits.Take(true))
    SaveProperties(edit.id, std::move(edit.name));
}
void RequestEditPump() {
  if (!edits.HasPending() || editPumpQueued.exchange(true))
    return;
  const auto generation = epoch.load();
  SKSE::GetTaskInterface()->AddTask([generation] {
    if (epoch.load() == generation)
      FlushEdits();
    editPumpQueued.store(false);
  });
}
void OpenConnection(Connection connection) {
  FlushEdits();
  notice.clear();
  Publish(connection.target, connection.source);
  requestedConnection.store(std::make_shared<Connection>(connection));
  if (connectionWindow)
    connectionWindow->IsOpen.store(true);
}
void __stdcall DrawConnection() {
  using namespace ImGui;
  namespace W = UI::Widgets;
  if (!connectionWindow || !connectionWindow->IsOpen.load())
    return;
  const auto model = view.load();
  static Connection draft;
  static std::uint64_t generation{};
  if (const auto request = requestedConnection.exchange(nullptr)) {
    draft = *request;
    generation = model->generation;
  }
  W::WorkspaceTheme theme;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowSize(
      {std::min(520.F, screen.x - 60.F), std::min(590.F, screen.y - 60.F)},
      ImGuiCond_FirstUseEver);
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_Appearing,
                   {.5F, .5F});
  bool open = true;
  if (ImGui::Begin("Connection", &open,
                   ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoSavedSettings)) {
    W::WorkspaceFont();
    const auto label = [&](RE::FormID id) {
      const auto found = model->targetNames.find(id);
      return found == model->targetNames.end()
                 ? std::format("Object {:08X}", id)
                 : found->second;
    };
    BeginChild("ConnectionFields", {0, -GetFrameHeightWithSpacing()});
    W::FieldLabel("Activated object");
    TextWrapped("%s", label(draft.source).c_str());
    W::FieldLabel("Affected object");
    TextWrapped("%s", label(draft.target).c_str());
    W::FieldLabel("Effect");
    Checkbox("Enabled", &draft.enabled);
    W::FieldLabel("Action");
    if (W::Combo("##ConnectionAction", actionNames[unsigned(draft.action)])) {
      for (unsigned i = 0; i < actionNames.size(); ++i)
        if (SupportsAction(draft.kind, Action(i)) &&
            Selectable(actionNames[i], unsigned(draft.action) == i))
          draft.action = Action(i);
      EndCombo();
    }
    if (draft.action == Action::Deposit || draft.action == Action::Retrieve) {
      W::FieldLabel("Item filter");
      if (W::Combo("##ItemFilter", filterNames[unsigned(draft.filter)])) {
        for (unsigned i = 0; i < filterNames.size(); ++i)
          if (Selectable(filterNames[i], unsigned(draft.filter) == i))
            draft.filter = Filter(i);
        EndCombo();
      }
      if (draft.filter == Filter::SpecificItem) {
        const auto item =
            std::ranges::find(model->items, draft.item, &Object::id);
        const auto preview =
            item == model->items.end()
                ? (draft.item ? std::format("Item {:08X}", draft.item)
                              : "Choose an item")
                : item->name;
        W::FieldLabel("Item");
        if (W::Combo("##SpecificItem", preview.c_str())) {
          for (const auto &choice : model->items)
            if (Selectable(
                    std::format("{}##{}", choice.name, choice.id).c_str(),
                    choice.id == draft.item))
              draft.item = choice.id;
          EndCombo();
        }
      }
      W::FieldLabel("Maximum items per activation");
      InputInt("##TransferLimit", &draft.limit);
      draft.limit = std::clamp(draft.limit, 0, 100000);
      W::Help("0 transfers all matches. Quest, equipped and favorited "
              "items are always excluded. Transfers are between you and "
              "the connected container.");
    }

    if (W::FormButton("Remove connection", true))
      OpenPopup("Remove connection?");
    if (BeginPopupModal("Remove connection?", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
      TextUnformatted("Remove this connection?");
      if (W::PrimaryButton("Remove")) {
        const auto expected = generation;
        SKSE::GetTaskInterface()->AddTask([connection = draft, expected] {
          if (epoch.load() == expected) {
            Remove(connection);
          }
        });
        open = false;
        CloseCurrentPopup();
      }
      SameLine();
      if (W::QuietButton("Cancel"))
        CloseCurrentPopup();
      if (W::EscapePressed())
        CloseCurrentPopup();
      EndPopup();
    }
    if (!model->notice.empty())
      TextWrapped("%s", model->notice.c_str());
    EndChild();
    BeginDisabled(!Valid(draft) || generation != epoch.load());
    if (W::PrimaryButton("Done", {100, 0})) {
      const auto expected = generation;
      SKSE::GetTaskInterface()->AddTask([connection = draft, expected] {
        if (epoch.load() == expected)
          Store(connection);
      });
      open = false;
    }
    EndDisabled();
    SameLine();
    if (W::QuietButton("Cancel"))
      open = false;
  }
  const bool escape = W::EscapePressed();
  End();
  if (!open || escape) {
    connectionWindow->IsOpen.store(false);
  }
}
void __stdcall DrawEditor() {
  using namespace ImGui;
  namespace W = UI::Widgets;
  if (!window || !window->IsOpen.load())
    return;
  const auto current = view.load();
  static RE::FormID selected{};
  static std::string propertyName;
  static std::uint64_t generation{};
  const auto select = [&](RE::FormID id) {
    selected = id;
    const auto object = std::ranges::find(current->sources, id, &Object::id);
    propertyName = object != current->sources.end() ? object->displayName : "";
    generation = current->generation;
  };
  if (const auto request = requestedSource.exchange(0))
    select(request);
  if (generation != current->generation)
    select(0);
  W::WorkspaceTheme theme;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowSize(
      {std::min(1040.F, screen.x - 80.F), std::min(700.F, screen.y - 80.F)},
      ImGuiCond_FirstUseEver);
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_FirstUseEver,
                   {.5F, .5F});
  bool open = true;
  if (ImGui::Begin("Edit Properties", &open,
                   ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoSavedSettings)) {
    W::WorkspaceFont();
    if (BeginTable("PropertyPanels", 2, ImGuiTableFlags_Resizable)) {
      TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 280.F);
      TableSetupColumn("Properties", ImGuiTableColumnFlags_WidthStretch);
      TableNextRow();
      TableSetColumnIndex(0);
      BeginChild("Objects", {0, -1}, ImGuiChildFlags_AlwaysUseWindowPadding);
      W::Section("Nearby usable objects");
      for (const auto &object : current->sources) {
        PushID(static_cast<int>(object.id));
        if (W::SidebarItem(object.name.c_str(), selected == object.id)) {
          RequestEditPump();
          select(object.id);
        }
        PopID();
      }
      EndChild();
      TableSetColumnIndex(1);
      BeginChild("PropertyFields", {0, -1},
                 ImGuiChildFlags_AlwaysUseWindowPadding);
      const auto source =
          std::ranges::find(current->sources, selected, &Object::id);
      if (source != current->sources.end()) {
        W::Section("Properties");
        W::FieldLabel("Name");
        W::Input("##ObjectName", propertyName, -1);
        if (generation == epoch.load()) {
          if (CleanObjectName(propertyName))
            edits.Stage(selected, PropertyDraft{selected, propertyName});
          else {
            edits.Erase(selected);
            TextDisabled("Enter a valid name.");
          }
        }
        W::SpaceBefore();
        BeginDisabled(!available.load());
        if (W::PrimaryButton("Create New Connection")) {
          const auto expected = generation;
          SKSE::GetTaskInterface()->AddTask([id = selected, expected] {
            if (epoch.load() == expected) {
              FlushEdits();
              Begin(Resolve(id));
            }
          });
        }
        EndDisabled();
        const auto table = [&](bool outgoing) {
          W::Section(outgoing ? "\xE2\xA4\xB4 Outgoing"
                              : "\xE2\xA4\xB5 Incoming");
          if (BeginTable(outgoing ? "Outgoing" : "Incoming", 3,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX |
                             ImGuiTableFlags_SizingStretchProp)) {
            TableSetupColumn(outgoing ? "Target" : "Source",
                             ImGuiTableColumnFlags_WidthStretch, 1.5F);
            TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 1.F);
            TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.F);
            TableHeadersRow();
            bool any = false;
            for (const auto &connection : current->connections) {
              if ((outgoing ? connection.source : connection.target) !=
                  selected)
                continue;
              any = true;
              const auto other =
                  outgoing ? connection.target : connection.source;
              const auto found = current->targetNames.find(other);
              const auto label = found == current->targetNames.end()
                                     ? std::format("Object {:08X}", other)
                                     : found->second;
              PushID(static_cast<int>(connection.source));
              PushID(static_cast<int>(connection.target));
              TableNextRow(0, W::TableRowHeight);
              TableSetColumnIndex(0);
              const auto rowPosition = GetCursorScreenPos();
              const float iconSize = GetFrameHeight();
              if (Selectable("##Connection", false,
                             ImGuiSelectableFlags_SpanAllColumns,
                             {0, iconSize})) {
                RequestEditPump();
                const auto expected = generation;
                SKSE::GetTaskInterface()->AddTask([connection, expected] {
                  if (epoch.load() == expected)
                    OpenConnection(connection);
                });
              }
              GetWindowDrawList()->PushClipRect(
                  rowPosition,
                  {rowPosition.x + GetContentRegionAvail().x,
                   rowPosition.y + iconSize},
                  true);
              if (const auto icon = current->targetIcons.find(other);
                  icon != current->targetIcons.end())
                if (const auto texture = W::Texture(W::Root() / icon->second))
                  GetWindowDrawList()->AddImage(
                      texture, rowPosition,
                      {rowPosition.x + iconSize, rowPosition.y + iconSize});
              GetWindowDrawList()->AddText(
                  {rowPosition.x + iconSize + GetStyle().ItemSpacing.x,
                   rowPosition.y + (iconSize - GetTextLineHeight()) * .5F},
                  GetColorU32(ImGuiCol_Text), label.c_str());
              GetWindowDrawList()->PopClipRect();
              TableSetColumnIndex(1);
              AlignTextToFramePadding();
              TextUnformatted(actionNames[unsigned(connection.action)]);
              TableSetColumnIndex(2);
              AlignTextToFramePadding();
              TextDisabled("%s", connection.enabled ? "Enabled" : "Disabled");
              PopID();
              PopID();
            }
            if (!any) {
              TableNextRow(0, W::TableRowHeight);
              TableSetColumnIndex(0);
              TextDisabled("No connections");
            }
            EndTable();
          }
        };
        table(true);
        table(false);
      } else
        W::Help("Select a nearby usable object.");
      if (!current->notice.empty()) {
        W::SpaceBefore();
        TextWrapped("%s", current->notice.c_str());
      }
      EndChild();
      EndTable();
    }
  }
  const bool escape = W::EscapePressed();
  End();
  if (!open || escape) {
    RequestEditPump();
    window->IsOpen.store(false);
  }
}
void __stdcall DrawWire() {
  using namespace ImGui;
  const auto frame = wire.load();
  if (!frame)
    return;
  const auto display = GetIO().DisplaySize;
  const auto project = [&](RE::NiPoint3 point, ImVec2 &out) {
    float x{}, y{}, z{};
    if (!RE::NiCamera::WorldPtToScreenPt3(frame->matrix, frame->viewport, point,
                                          x, y, z, .000001F))
      return false;
    out = {x * display.x, (1 - y) * display.y};
    return std::isfinite(out.x) && std::isfinite(out.y);
  };
  ImVec2 start, end;
  auto *draw = GetForegroundDrawList();
  const auto color = frame->valid ? 0xFF85DC75 : 0xFF55CDE2;
  if (project(frame->source, start) && project(frame->end, end)) {
    ImDrawListManager::AddLine(draw, start, end, 0xBB111111, 5.F);
    ImDrawListManager::AddLine(draw, start, end, color, 2.F);
    draw->AddCircle(end, 9.F, color, 24, 2.F);
  }
  const std::string hint = frame->valid
                               ? std::format("Connect: {}", frame->target)
                               : "Aim at a nearby SkyBuild usable object";
  const auto extent = CalcTextSize(hint.c_str());
  ImDrawListManager::AddText(
      draw, {display.x * .5F - extent.x * .5F, display.y * .5F + 32.F}, color,
      hint.c_str());
}
} // namespace
void Initialize() {
  window = UI::Native::AddWindow(DrawEditor);
  connectionWindow = UI::Native::AddWindow(DrawConnection);
  hud = std::make_unique<UI::Native::Model::HudElement>(DrawWire);
  static ActivationListener listener;
  auto *events = RE::ScriptEventSourceHolder::GetSingleton();
  if (!events)
    return;
  events->AddEventSink<RE::TESActivateEvent>(&listener);
  available.store(true);
}
bool SupportsProperties(RE::TESObjectREFR *object) {
  const auto *base = object ? object->GetBaseObject() : nullptr;
  return base &&
         (base->As<RE::TESObjectDOOR>() || base->As<RE::TESFurniture>() ||
          base->As<RE::TESObjectCONT>() || base->As<RE::TESObjectACTI>());
}
void OpenProperties(RE::TESObjectREFR *object) {
  if (!BuildingMode::IsHammerReady() || Terrain::Ready() || !object ||
      !Resolve(object->GetFormID()) || !SupportsProperties(object))
    return;
  const auto nameDraft =
      pendingSource.load() == object->GetFormID() ? pendingName : std::nullopt;
  Edit(object->GetFormID(), nameDraft);
}
void Begin(RE::TESObjectREFR *source) {
  if (!available.load() || !BuildingMode::IsHammerReady() || !source ||
      (!SupportsProperties(source) || source->IsDeleted() ||
       !BuildingManager::IsPlacedObject(source->GetFormID())))
    return;
  if (window)
    window->IsOpen.store(false);
  if (connectionWindow)
    connectionWindow->IsOpen.store(false);
  pendingSource.store(source->GetFormID());
  UI::CancelToolWheel();
}
void Cancel() {
  pendingName.reset();
  pendingSource.store(0);
  wire.store(nullptr);
}
bool IsConnecting() { return pendingSource.load() != 0; }
bool HasTarget() {
  const auto frame = wire.load();
  return frame && frame->valid;
}
bool HandleButton(const ControlBindings::Event *button) {
  using namespace ControlBindings;
  const auto match = [&](const auto &binding) {
    return ControlBindings::Matches(*button, binding);
  };
  if (!IsConnecting())
    return false;
  if (match(modifyControls[1])) {
    if (button->IsDown())
      OpenProperties(Resolve(pendingSource.load()));
    return true;
  }
  if ((button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
       button->GetIDCode() == 1) ||
      match(secondaryAction)) {
    if (button->IsDown())
      Cancel();
    return true;
  }
  if (match(placeAction)) {
    if (button->IsDown()) {
      const auto target = Aim();
      const auto kind = Kind(target.get());
      const auto source = pendingSource.load();
      if (kind && target->GetFormID() != source) {
        // Stage a draft in the editor only: activation remains unchanged until
        // the editor closes.
        const auto nameDraft = pendingName;
        Edit(source, nameDraft);
        Connection draft{source, target->GetFormID(), *kind,
                         *kind == Target::Door        ? Action::ToggleDoor
                         : *kind == Target::Container ? Action::OpenContainer
                                                      : Action::Activate};
        const auto live = registry.load();
        const auto prior =
            std::ranges::find_if(live->connections, [&](const auto &entry) {
              return ConnectionKey(entry) == ConnectionKey(draft);
            });
        if (prior != live->connections.end())
          draft = *prior;
        OpenConnection(draft);
      }
    }
    return true;
  }
  return false;
}
void Update() {
  if ((!window || !window->IsOpen.load()) &&
      (!connectionWindow || !connectionWindow->IsOpen.load()))
    FlushEdits();
  if (!BuildingMode::IsHammerReady()) {
    Cancel();
    FlushEdits();
    if (window)
      window->IsOpen.store(false);
    if (connectionWindow)
      connectionWindow->IsOpen.store(false);
    return;
  }
  if (!IsConnecting())
    return;
  if (Terrain::Ready() || UI::IsToolWheelOpen()) {
    Cancel();
    return;
  }
  if (UI::IsGameMenuBlocking(true)) {
    wire.store(nullptr);
    return;
  }
  auto *source =
      RE::TESForm::LookupByID<RE::TESObjectREFR>(pendingSource.load());
  if (source && (source->IsDeleted() ||
                 !BuildingManager::IsPlacedObject(source->GetFormID()))) {
    Cancel();
    return;
  }
  if (source && source->IsDisabled()) {
    wire.store(nullptr);
    return;
  }
  const auto camera = RE::PlayerCamera::GetSingleton();
  const auto renderCamera = RE::Main::WorldRootCamera();
  if (!source || !camera || !camera->cameraRoot || !renderCamera) {
    Cancel();
    return;
  }
  auto frame = std::make_shared<Wire>();
  frame->source = source->GetPosition() + RE::NiPoint3{0, 0, 32};
  frame->end = camera->cameraRoot->world.translate +
               camera->cameraRoot->world.rotate.GetVectorY() * 32.F;
  const auto target = Aim();
  frame->valid = target && Kind(target.get()).has_value() &&
                 target->GetFormID() != source->GetFormID();
  if (frame->valid)
    frame->target = Name(target->GetFormID());
  std::memcpy(frame->matrix, renderCamera->GetRuntimeData().worldToCam,
              sizeof(frame->matrix));
  frame->viewport = renderCamera->GetRuntimeData2().port;
  wire.store(std::move(frame));
}
void Reset() {
  ++epoch;
  edits.Clear();
  requestedConnection.store(nullptr);
  Cancel();
  if (window)
    window->IsOpen.store(false);
  if (connectionWindow)
    connectionWindow->IsOpen.store(false);
  requestedSource.store(0);
  registry.store(std::make_shared<ConnectionSnapshot>());
  notice.clear();
  view.store(std::make_shared<View>());
}
void Save(SKSE::SerializationInterface *serialization) {
  FlushEdits();
  Registry saved;
  for (const auto &c : registry.load()->connections)
    if (BuildingManager::IsPlacedObject(c.source) &&
        BuildingManager::IsPlacedObject(c.target))
      saved.push_back(c);
  const auto data = Encode(saved).dump();
  if (!serialization->OpenRecord(SaveRecord, 1) ||
      !serialization->WriteRecordData(data.data(),
                                      static_cast<std::uint32_t>(data.size())))
    spdlog::error("Could not save activator connections.");
}
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length) {
  if (version != 1 || length > MaxRecordBytes) {
    spdlog::warn("Unsupported activator connection record.");
    return;
  }
  std::string data(length, '\0');
  if (serialization->ReadRecordData(data.data(), length) != length)
    return;
  try {
    auto loaded = std::make_shared<Registry>();
    for (auto c : Decode(nlohmann::json::parse(data))) {
      RE::FormID source{}, target{}, item{};
      if (!serialization->ResolveFormID(c.source, source) ||
          !serialization->ResolveFormID(c.target, target) ||
          (c.item && !serialization->ResolveFormID(c.item, item)))
        continue;
      c.source = source;
      c.target = target;
      c.item = item;
      if (Valid(c))
        loaded->push_back(c);
    }
    registry.store(std::make_shared<ConnectionSnapshot>(std::move(*loaded)));
  } catch (const std::exception &e) {
    spdlog::error("Invalid activator connections: {}", e.what());
  }
}
} // namespace SkyBuild::Activators
