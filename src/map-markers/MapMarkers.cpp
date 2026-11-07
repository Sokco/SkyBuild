#include "map-markers/MapMarkers.h"
#include "building/BuildingMode.h"
#include "building/PackCategories.h"
#include "map-markers/MarkerIcons.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/search/SearchView.h"
#include "ui/shared/AutosaveQueue.h"
#include <atomic>

namespace SkyBuild::MapMarkers {
namespace {
struct Entry {
  Marker data;
  RE::TESObjectREFRPtr marker, arrival;
};
struct Row {
  Marker data;
  std::string world;
};
struct View {
  std::vector<Row> rows;
  bool canAdd{};
  std::string location, message;
  std::uint32_t edited{};
  std::uint64_t revision{}, generation{}, session{};
};
std::vector<Entry> entries;
std::atomic<std::shared_ptr<const View>> view;
UI::Native::Model::WindowInterface *window{};
std::atomic_bool open{}, pending{};
RE::FormID lastWorld{};
std::uint64_t revision{};
std::atomic_uint64_t editEpoch{1}, editSession{1};
struct Draft {
  RE::FormID id{};
  std::string name, icon;
};
UI::AutosaveQueue<Draft> edits;
std::atomic_bool pumpQueued{}, forceEdits{};
void FlushEdits(bool force);

bool CanAdd() {
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto cell = player ? player->GetParentCell() : nullptr;
  return player && cell && !cell->IsInteriorCell() && player->GetWorldspace();
}
void Publish(std::string message = {}, RE::FormID edited = 0) {
  auto state = std::make_shared<View>();
  state->canAdd = CanAdd();
  state->message = std::move(message);
  state->edited = edited;
  state->revision = ++revision;
  state->generation = editEpoch.load();
  state->session = editSession.load();
  const auto player = RE::PlayerCharacter::GetSingleton();
  if (const auto world = player ? player->GetWorldspace() : nullptr)
    state->location = world->GetName();
  for (const auto &entry : entries) {
    const auto world =
        RE::TESForm::LookupByID<RE::TESWorldSpace>(entry.data.world);
    state->rows.push_back(
        {entry.data, world ? world->GetName() : "Unknown world"});
  }
  view.store(std::move(state));
}
void Unregister(RE::TESObjectREFR *marker) {
  if (!marker)
    return;
  if (const auto player = RE::PlayerCharacter::GetSingleton()) {
    auto &markers = player->GetPlayerRuntimeData().currentMapMarkers;
    const auto handle = marker->GetHandle();
    for (auto i = markers.begin(); i != markers.end();)
      if (*i == handle)
        i = markers.erase(i);
      else
        ++i;
  }
}
RE::TESWorldSpace *MapWorld(RE::TESWorldSpace *world) {
  // Walled cities can share Tamriel's map while retaining their own arrival
  // worldspace. Solstheim and independent mod worldspaces keep separate maps.
  for (int depth = 0;
       world && world->parentWorld && depth < 32 &&
       world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseMapData);
       ++depth)
    world = world->parentWorld;
  return world;
}
void RegisterCurrentWorld() {
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto world = player ? player->GetWorldspace() : nullptr;
  if (!world)
    return;
  auto &markers = player->GetPlayerRuntimeData().currentMapMarkers;
  for (const auto &entry : entries) {
    if (!entry.marker || entry.marker->IsDeleted())
      continue;
    if (MapWorld(RE::TESForm::LookupByID<RE::TESWorldSpace>(
            entry.data.world)) != MapWorld(world)) {
      Unregister(entry.marker.get());
      continue;
    }
    const auto handle = entry.marker->GetHandle();
    if (std::find(markers.begin(), markers.end(), handle) == markers.end())
      markers.push_back(handle);
  }
}
bool Apply(Entry &entry) {
  auto *reference = entry.marker.get();
  if (!reference || !entry.arrival || reference->IsDeleted() ||
      entry.arrival->IsDeleted())
    return false;
  auto *extra = reference->extraList.GetByType<RE::ExtraMapMarker>();
  const bool attach = !extra;
  if (!extra)
    extra = RE::BSExtraData::Create<RE::ExtraMapMarker>();
  if (!extra)
    return false;
  if (!extra->mapData) {
    // Both blocks belong to Skyrim's allocator and are released by its extra
    // data destructor. Install TESFullName's native vtable before exposing it.
    extra->mapData = RE::calloc<RE::MapMarkerData>(1);
    if (!extra->mapData) {
      if (attach)
        delete extra;
      return false;
    }
    *reinterpret_cast<std::uintptr_t *>(&extra->mapData->locationName) =
        RE::TESFullName::VTABLE[0].address();
  }
  extra->mapData->locationName.fullName = entry.data.name;
  extra->mapData->type =
      static_cast<RE::MARKER_TYPE>(Icons::Resolve(entry.data));
  extra->mapData->SetVisible(true);
  extra->mapData->flags.set(RE::MapMarkerData::Flag::kCanTravelTo);
  if (attach)
    reference->extraList.Add(extra);
  auto *origin = reference->extraList.GetByType<RE::ExtraStartingWorldOrCell>();
  if (!origin) {
    origin = RE::BSExtraData::Create<RE::ExtraStartingWorldOrCell>();
    if (!origin)
      return false;
    reference->extraList.Add(origin);
  }
  origin->startingWorldOrCell =
      RE::TESForm::LookupByID<RE::TESWorldSpace>(entry.data.world);
  reference->extraList.SetLinkedRef(entry.arrival.get(), nullptr);
  reference->AddChange(RE::TESObjectREFR::ChangeFlags::kCreatedOnlyExtra |
                       RE::TESObjectREFR::ChangeFlags::kGameOnlyExtra);
  return origin->startingWorldOrCell != nullptr;
}
void Retire(Entry &entry) {
  Unregister(entry.marker.get());
  if (entry.marker)
    if (auto *extra = entry.marker->extraList.GetByType<RE::ExtraMapMarker>();
        extra && extra->mapData) {
      extra->mapData->SetVisible(false);
      extra->mapData->flags.reset(RE::MapMarkerData::Flag::kCanTravelTo);
    }
  for (const auto &reference : {entry.marker, entry.arrival})
    if (reference) {
      reference->Disable();
      reference->SetDelete(true);
    }
}
void Commit(RE::FormID id, std::string name, std::string iconKey) {
  name = CleanName(std::move(name));
  if (name.empty()) {
    Publish("Enter a name of 1 to 256 characters.");
    return;
  }
  const auto icons = Icons::Catalog();
  const auto icon = icons
                        ? std::ranges::find(*icons, iconKey, &Icons::Icon::key)
                        : std::vector<Icons::Icon>::const_iterator{};
  if (!icons || icon == icons->end()) {
    Publish("Choose an available icon.");
    return;
  }
  if (id) {
    const auto found = std::ranges::find(
        entries, id, [](const Entry &e) { return e.data.reference; });
    if (found == entries.end()) {
      Publish("This marker no longer exists.");
      return;
    }
    const auto original = found->data;
    found->data.name = std::move(name);
    found->data.customIcon = icon->custom ? icon->key : "";
    if (!icon->custom)
      found->data.vanillaIcon = icon->type;
    if (!Apply(*found)) {
      found->data = original;
      Publish("The marker could not be updated.");
      return;
    }
    Publish("Marker updated.");
    return;
  }
  if (!CanAdd()) {
    Publish("Go outdoors to add a world map marker.");
    return;
  }
  if (entries.size() >= MaxMarkers) {
    Publish("The marker limit has been reached.");
    return;
  }
  auto *player = RE::PlayerCharacter::GetSingleton();
  auto *markerBase = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x10);
  auto *arrivalBase = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x34);
  if (!markerBase || !arrivalBase) {
    Publish("Skyrim's map marker forms are unavailable.");
    return;
  }
  Entry entry;
  entry.marker = player->PlaceObjectAtMe(markerBase, true);
  entry.arrival = player->PlaceObjectAtMe(arrivalBase, true);
  if (!entry.marker || !entry.arrival) {
    Retire(entry);
    Publish("Skyrim could not create the marker.");
    return;
  }
  const auto position = player->GetPosition();
  const RE::NiPoint3 facing{0, 0, player->GetAngleZ()};
  entry.marker->SetPosition(position);
  entry.arrival->SetPosition(position);
  entry.arrival->SetAngle(facing);
  entry.data = {entry.marker->GetFormID(),
                entry.arrival->GetFormID(),
                player->GetWorldspace()->GetFormID(),
                std::move(name),
                icon->custom ? std::uint16_t{11} : icon->type,
                icon->custom ? icon->key : ""};
  if (!Apply(entry)) {
    Retire(entry);
    Publish("The marker could not be configured.");
    return;
  }
  const auto created = entry.data.reference;
  entries.push_back(std::move(entry));
  RegisterCurrentWorld();
  Publish("Marker added at your position and facing direction.", created);
}
void FlushEdits(bool force) {
  for (auto &edit : edits.Take(force))
    Commit(edit.id, std::move(edit.name), std::move(edit.icon));
}
void RequestEditPump(bool force = false) {
  if (force)
    forceEdits.store(true);
  if (!(forceEdits.load() ? edits.HasPending() : edits.HasReady()) ||
      pumpQueued.exchange(true))
    return;
  const auto generation = editEpoch.load();
  SKSE::GetTaskInterface()->AddTask([generation] {
    if (editEpoch.load() == generation)
      FlushEdits(forceEdits.exchange(false));
    pumpQueued.store(false);
  });
}
template <class Task> void Submit(Task task) {
  if (pending.exchange(true))
    return;
  const auto generation = editEpoch.load();
  SKSE::GetTaskInterface()->AddTask([task = std::move(task), generation] {
    if (editEpoch.load() == generation && IsOpen() &&
        BuildingMode::IsHammerReady())
      task();
    pending.store(false);
  });
}
void IconPicker(std::string &key,
                const std::shared_ptr<const std::vector<Icons::Icon>> &icons) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  static std::string search;
  static int source{};
  const auto screen = GetIO().DisplaySize;
  SetNextWindowSize(
      {std::min(820.F, screen.x * .9F), std::min(580.F, screen.y * .85F)},
      ImGuiCond_Appearing);
  SetNextWindowSizeConstraints(
      {std::min(620.F, screen.x * .9F), std::min(420.F, screen.y * .85F)},
      screen);
  bool shown = true;
  if (!BeginPopupModal("Choose map icon", &shown, ImGuiWindowFlags_NoCollapse))
    return;
  W::WorkspaceFont(16);
  BeginChild("IconSources", {130, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
  W::SidebarHeading("ICONS");
  for (int i = 0; i < 3; ++i)
    if (W::SidebarItem(std::array{"All", "Skyrim", "CoMAP"}[i], source == i))
      source = i;
  EndChild();
  SameLine();
  BeginChild("IconBrowser", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
  W::FieldLabel("Search map icons");
  W::Input("##SearchIcons", search, -1);

  Separator();
  BeginChild("IconGrid", {0, 0});
  const auto filter = Packs::CategoryKey(search);
  std::vector<const Icons::Icon *> filtered;
  if (icons)
    for (const auto &icon : *icons)
      if ((source == 0 || icon.custom == (source == 2)) &&
          (filter.empty() ||
           Packs::CategoryKey(icon.label).find(filter) != std::string::npos))
        filtered.push_back(&icon);
  constexpr float width = 98, height = 88;
  const int columns =
      std::max(1, static_cast<int>(GetContentRegionAvail().x /
                                   (width + GetStyle().ItemSpacing.x)));
  W::Rows((filtered.size() + columns - 1) / columns, [&](std::size_t row) {
    for (int column = 0; column < columns; ++column) {
      const auto index = row * columns + column;
      if (index >= filtered.size())
        break;
      const auto &icon = *filtered[index];
      if (column)
        SameLine();
      const auto pos = GetCursorScreenPos();
      PushID(icon.key.c_str());
      if (Selectable("##Icon", key == icon.key, ImGuiSelectableFlags_None,
                     {width, height})) {
        key = icon.key;
        CloseCurrentPopup();
      }
      Icons::Thumbnail(icon, pos.x + (width - 38) * .5F, pos.y + 9, 38);
      const auto label = UI::SearchView::FitCardLabel(icon.label, width - 10);
      const auto extent = CalcTextSize(label.c_str());
      GetWindowDrawList()->AddText(
          {pos.x + (width - extent.x) * .5F, pos.y + 58},
          GetColorU32(ImGuiCol_Text), label.c_str());
      if (IsItemHovered())
        SetTooltip("%s%s", icon.label.c_str(), icon.custom ? " (CoMAP)" : "");
      PopID();
    }
  });
  if (filtered.empty())
    W::Help(source == 2
                ? "No CoMAP icons are available. Install CoMAP to add them."
                : "No matching icons.");
  EndChild();
  EndChild();
  if (W::EscapePressed())
    CloseCurrentPopup();
  EndPopup();
}
void __stdcall Draw() {
  using namespace ImGui;
  namespace W = UI::Widgets;
  const auto state = view.load();
  if (!state)
    return;
  W::BeginFrame();
  W::Theme theme;
  W::WorkspaceTheme workspace;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowSize(
      {std::min(1020.F, screen.x * .92F), std::min(620.F, screen.y * .86F)},
      ImGuiCond_Appearing);
  SetNextWindowSizeConstraints(
      {std::min(780.F, screen.x * .92F), std::min(460.F, screen.y * .86F)},
      screen);
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_Appearing,
                   {.5F, .5F});
  static RE::FormID selected{}, removing{};
  static std::string name, key{"11"};
  static std::uint64_t seenRevision{}, seenSession{};
  const auto select = [&](const Marker &marker) {
    selected = marker.reference;
    name = marker.name;
    key = marker.customIcon.empty() ? std::to_string(marker.vanillaIcon)
                                    : marker.customIcon;
  };
  if (seenRevision != state->revision) {
    const bool reopened = seenSession != state->session;
    if (state->edited && state->edited != selected)
      for (const auto &row : state->rows)
        if (row.data.reference == state->edited)
          select(row.data);
    if (reopened) {
      const auto row = std::ranges::find(
          state->rows, selected, [](const Row &r) { return r.data.reference; });
      if (row != state->rows.end())
        select(row->data);
      else if (!state->rows.empty())
        select(state->rows.front().data);
    }
    if (std::ranges::none_of(state->rows, [&](const Row &r) {
          return r.data.reference == selected;
        })) {
      selected = 0;
      name.clear();
      key = "11";
    }
    seenRevision = state->revision;
    seenSession = state->session;
  }
  bool shown = true;
  if (Begin("Map Markers", &shown, ImGuiWindowFlags_NoCollapse)) {
    W::WorkspaceFont();
    const float detailsWidth =
        std::clamp(GetContentRegionAvail().x * .35F, 270.F, 350.F);
    BeginChild(
        "SavedMarkers",
        {GetContentRegionAvail().x - detailsWidth - GetStyle().ItemSpacing.x,
         -GetFrameHeightWithSpacing()},
        ImGuiChildFlags_AlwaysUseWindowPadding);
    AlignTextToFramePadding();
    Text("YOUR MARKERS  %zu", state->rows.size());
    SameLine();
    BeginDisabled(pending.load() || !state->canAdd);
    if (W::QuietButton("New marker"))
      Submit([] {
        FlushEdits(true);
        Commit(0, "New marker", "11");
      });
    EndDisabled();
    if (state->rows.empty())
      TextDisabled("No markers");
    else if (BeginTable("Markers", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_PadOuterX,
                        {0, 0})) {
      TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.3F);
      TableSetupColumn("World", ImGuiTableColumnFlags_WidthStretch, 1.F);
      TableHeadersRow();
      for (const auto &row : state->rows) {
        TableNextRow(0, W::TableRowHeight);
        TableSetColumnIndex(0);
        PushID(static_cast<int>(row.data.reference));
        const auto &label =
            row.data.reference == selected && !CleanName(name).empty()
                ? name
                : row.data.name;
        const auto pos = GetCursorScreenPos();
        const float side = GetFrameHeight();
        if (Selectable("##Marker", selected == row.data.reference,
                       ImGuiSelectableFlags_SpanAllColumns, {0, side})) {
          RequestEditPump(true);
          select(row.data);
        }
        GetWindowDrawList()->PushClipRect(
            pos, {pos.x + GetContentRegionAvail().x, pos.y + side}, true);
        const auto icons = Icons::Catalog();
        const auto iconKey = row.data.reference == selected
                                 ? key
                                 : (row.data.customIcon.empty()
                                        ? std::to_string(row.data.vanillaIcon)
                                        : row.data.customIcon);
        if (icons)
          for (const auto &icon : *icons)
            if (icon.key == iconKey) {
              Icons::Thumbnail(icon, pos.x, pos.y, side);
              break;
            }
        GetWindowDrawList()->AddText(
            {pos.x + side + GetStyle().ItemSpacing.x,
             pos.y + (side - GetTextLineHeight()) * .5F},
            GetColorU32(ImGuiCol_Text), label.c_str());
        GetWindowDrawList()->PopClipRect();
        TableSetColumnIndex(1);
        AlignTextToFramePadding();
        TextDisabled("%s", row.world.c_str());
        PopID();
      }
      EndTable();
    }
    EndChild();
    SameLine();
    BeginChild("MarkerDetails", {0, -GetFrameHeightWithSpacing()},
               ImGuiChildFlags_AlwaysUseWindowPadding);
    W::Section(selected ? "Edit marker" : "Map marker");
    if (selected) {
      const auto oldName = name, oldKey = key;
      W::FieldLabel("Name");
      W::Input("##MarkerName", name, -1);
      W::FieldLabel("Map icon");
      const auto icons = Icons::Catalog();
      const Icons::Icon *chosen{};
      if (icons)
        for (const auto &icon : *icons)
          if (icon.key == key) {
            chosen = &icon;
            break;
          }
      const auto pos = GetCursorScreenPos();
      Dummy({56, 56});
      if (chosen)
        Icons::Thumbnail(*chosen, pos.x + 5, pos.y + 5, 46);
      SameLine();
      BeginGroup();
      TextUnformatted(chosen ? chosen->label.c_str()
                             : "Unavailable CoMAP icon");
      if (W::QuietButton("Choose icon"))
        OpenPopup("Choose map icon");
      EndGroup();
      IconPicker(key, icons);

      if ((oldName != name || oldKey != key) &&
          state->generation == editEpoch.load()) {
        if (!CleanName(name).empty() && icons &&
            std::ranges::any_of(
                *icons, [&](const auto &icon) { return icon.key == key; }))
          edits.Stage(selected, Draft{selected, name, key}, oldKey != key);
        else
          edits.Erase(selected);
      }
      if (CleanName(name).empty())
        TextDisabled("Enter a name to apply the change.");
      if (W::FormButton("Remove marker", true)) {
        removing = selected;
        OpenPopup("Remove map marker?");
      }
      if (BeginPopupModal("Remove map marker?", nullptr,
                          ImGuiWindowFlags_AlwaysAutoResize)) {
        TextUnformatted(
            "Remove this marker and its fast-travel arrival point?");
        if (W::PrimaryButton("Remove")) {
          edits.Erase(removing);
          Submit([id = removing] {
            edits.Erase(id);
            const auto found = std::ranges::find(
                entries, id, [](const Entry &e) { return e.data.reference; });
            if (found != entries.end()) {
              Retire(*found);
              entries.erase(found);
              Publish("Marker removed.");
            }
          });
          CloseCurrentPopup();
        }
        SameLine();
        if (W::QuietButton("Cancel"))
          CloseCurrentPopup();
        if (W::EscapePressed())
          CloseCurrentPopup();
        EndPopup();
      }

    } else
      W::Help(state->canAdd
                  ? "Click New marker to add one at your current position."
                  : "Go outdoors to create a marker.");
    EndChild();
    Separator();
    TextDisabled("%s", state->message.c_str());
  }
  if (W::EscapePressed())
    shown = false;
  End();
  if (!shown || edits.HasPending())
    RequestEditPump(!shown);
  if (!shown)
    Submit([] { Close(); });
}
class MapOpened final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::MenuOpenCloseEvent *event,
               RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override {
    if (event && event->opening && event->menuName == RE::MapMenu::MENU_NAME)
      SKSE::GetTaskInterface()->AddTask([] {
        Icons::Refresh();
        for (auto &entry : entries)
          Apply(entry);
        RegisterCurrentWorld();
      });
    return RE::BSEventNotifyControl::kContinue;
  }
};
} // namespace
void Initialize() {
  static MapOpened sink;
  if (const auto ui = RE::UI::GetSingleton())
    ui->AddEventSink(&sink);
}
void Open() {
  if (!BuildingMode::IsHammerReady())
    return;
  if (!window)
    window = UI::Native::AddWindow(Draw);
  Icons::Refresh();
  ++editSession;
  Publish();
  open.store(true);
  window->IsOpen.store(true);
  UI::RefreshPresentation();
}
void Close() {
  FlushEdits(true);
  open.store(false);
  if (window)
    window->IsOpen.store(false);
  UI::RefreshPresentation();
}
bool IsOpen() { return open.load(); }
void Update() {
  FlushEdits(!IsOpen());
  if (IsOpen() && (!window->IsOpen.load() || !BuildingMode::IsHammerReady()))
    Close();
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto world = player ? player->GetWorldspace() : nullptr;
  const auto id = world ? world->GetFormID() : 0;
  if (lastWorld != id) {
    lastWorld = id;
    RegisterCurrentWorld();
    if (IsOpen())
      Publish();
  }
}
void Restore() {
  Icons::Refresh();
  for (auto &entry : entries) {
    auto *marker =
        RE::TESForm::LookupByID<RE::TESObjectREFR>(entry.data.reference);
    auto *arrival =
        RE::TESForm::LookupByID<RE::TESObjectREFR>(entry.data.arrival);
    // Only recover our saved pair of native marker forms; never recreate a
    // missing pair or adopt unrelated references after a failed ID resolution.
    if (!marker || !arrival || !marker->GetBaseObject() ||
        !arrival->GetBaseObject() ||
        marker->GetBaseObject()->GetFormID() != 0x10 ||
        arrival->GetBaseObject()->GetFormID() != 0x34) {
      spdlog::warn("Saved map marker pair {:08X}/{:08X} is unavailable.",
                   entry.data.reference, entry.data.arrival);
      continue;
    }
    entry.marker = RE::TESObjectREFRPtr(marker);
    entry.arrival = RE::TESObjectREFRPtr(arrival);
    if (!Apply(entry))
      spdlog::warn("Could not restore custom map marker {:08X}.",
                   entry.data.reference);
  }
  RegisterCurrentWorld();
  Publish();
}
void Reset() {
  ++editEpoch;
  edits.Clear();
  forceEdits.store(false);
  Close();
  entries.clear();
  lastWorld = 0;
  Publish();
}
void Save(SKSE::SerializationInterface *serialization) {
  FlushEdits(true);
  std::vector<Marker> markers;
  for (const auto &entry : entries)
    if ((!entry.marker || !entry.marker->IsDeleted()) &&
        (!entry.arrival || !entry.arrival->IsDeleted()))
      markers.push_back(entry.data);
  const auto data = Encode(markers).dump();
  if (!serialization->OpenRecord(SaveRecord, 1) ||
      !serialization->WriteRecordData(data.data(),
                                      static_cast<std::uint32_t>(data.size())))
    spdlog::error("Could not save custom map markers.");
}
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length) {
  if (version != 1 || length > MaxRecordBytes) {
    spdlog::warn("Unsupported custom map marker record.");
    return;
  }
  std::string data(length, '\0');
  if (serialization->ReadRecordData(data.data(), length) != length)
    return;
  try {
    const auto saved = Decode(nlohmann::json::parse(data));
    for (auto marker : saved) {
      RE::FormID reference{}, arrival{}, world{};
      if (!serialization->ResolveFormID(marker.reference, reference) ||
          !serialization->ResolveFormID(marker.arrival, arrival) ||
          !serialization->ResolveFormID(marker.world, world))
        continue;
      marker.reference = reference;
      marker.arrival = arrival;
      marker.world = world;
      entries.push_back({std::move(marker), {}, {}});
    }
  } catch (const std::exception &error) {
    spdlog::error("Invalid custom map marker save: {}", error.what());
  }
}
} // namespace SkyBuild::MapMarkers
