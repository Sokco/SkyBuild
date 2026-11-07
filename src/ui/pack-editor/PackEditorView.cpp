#include "building/AutomaticBuildCosts.h"
#include "ui/pack-editor/PackEditorView.h"
#include "ui/imgui/Widgets.h"
#include "ui/pack-editor/PackDocument.h"
#include "ui/thumbnails/ThumbnailProgress.h"
#include <atomic>
#include <imgui_internal.h>
#include <map>
#include <set>
namespace SkyBuild::UI::PackEditor::View {
namespace {
std::atomic<std::shared_ptr<const Json>> snapshot, assets, results,
    materialResults;
std::atomic_bool openPickedItem{};
SkyBuild::UI::Native::Model::WindowInterface *window{};
void (*submit)(const Json &){};
void (*pump)(){};
std::string filter, search, pluginFilter, recordType, newName, extraAsset;
std::string nameAction, oldName, expectedDraft, framePack;
std::size_t selectedItem{}, nameIndex{};
bool editingDraft{}, editing{}, showBase{}, requestName{}, requestEdit{},
    requestAdd{};
Json editItem, pendingRemoval;
std::string removalLabel;
bool requestRemoval{};
std::uint64_t searchRequest{}, editRevision{}, addRevision{};
bool refreshSearch{};
std::set<std::string> requestedResourceIcons;
const Json &Array(const Json &value, const char *key) {
  static const Json empty = Json::array();
  const auto found = value.find(key);
  return found != value.end() && found->is_array() ? *found : empty;
}
void Send(Json command) {
  command["pack"] = framePack;
  const auto action = command.value("action", "");
  if (command.contains("draft") || action == "add")
    command["source"] = {{"plugin", editItem.at("plugin")},
                         {"formId", editItem.at("formId")}};
  if (action == "categoryCreate" && command.contains("assign"))
    command["assign"]["source"] = {{"plugin", editItem.at("plugin")},
                                   {"formId", editItem.at("formId")}};
  if (submit)
    submit(command);
}
void Edit(Json values) {
  Send({{"action", "edit"},
        {"draft", editingDraft},
        {"index", selectedItem},
        {"values", std::move(values)}});
}
bool AutoButton() {
  using namespace ImGui;
  const auto icon = Widgets::Texture(Widgets::Root() / "assets/icons/auto.png",
                                     1.F, nullptr, nullptr, {18, 18});
  const bool clicked =
      icon ? ImageButton("##Auto", icon, {18, 18}) : Button("Auto");
  if (IsItemHovered())
    SetTooltip("Auto");
  return clicked;
}
void MessageFooter(const Json &state, float actionWidth = 0.F) {
  using namespace ImGui;
  const float width =
      actionWidth ? std::max(1.F, GetContentRegionAvail().x - actionWidth -
                                      GetStyle().ItemSpacing.x)
                  : 0.F;
  BeginChild("MessageFooter", {width, GetFrameHeight()}, ImGuiChildFlags_None,
             ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
  const auto progress = state.value("shellStatus", std::string{});
  const auto message =
      progress.empty() ? state.value("message", std::string{}) : progress;
  AlignTextToFramePadding();
  TextUnformatted(message.c_str());
  if (IsItemHovered() && CalcTextSize(message.c_str()).x > GetWindowWidth())
    SetTooltip("%s", message.c_str());
  EndChild();
}
bool OrderArrow(const char *id, ImGuiDir direction) {
  using namespace ImGui;
  const float side = GetFrameHeight() * .6F;
  const auto pos = GetCursorScreenPos();
  const bool clicked = InvisibleButton(id, {side, GetFrameHeight()});
  const auto top = ImVec2{pos.x, pos.y + (GetFrameHeight() - side) * .5F};
  const auto color = GetColorU32(IsItemActive()    ? ImGuiCol_ButtonActive
                                 : IsItemHovered() ? ImGuiCol_ButtonHovered
                                                   : ImGuiCol_Button);
  RenderFrame(top, {top.x + side, top.y + side}, color, true,
              GetStyle().FrameRounding);
  const auto center = ImVec2{top.x + side * .5F, top.y + side * .5F};
  const float radius = side * .22F;
  const float sign = direction == ImGuiDir_Up ? -1.F : 1.F;
  GetWindowDrawList()->AddTriangleFilled(
      {center.x, center.y + sign * radius},
      {center.x - radius, center.y - sign * radius},
      {center.x + radius, center.y - sign * radius},
      GetColorU32(ImGuiCol_Text));
  if (IsItemHovered())
    SetTooltip("%s", direction == ImGuiDir_Up ? "Move up" : "Move down");
  return clicked;
}
ImTextureID ItemPicture(const Json &item, bool picker) {
  auto path = item.value("icon", "");
  const auto plugin = item.value("plugin", "");
  const auto form = item.value(picker ? "local" : "formId", "");
  if (path.empty()) {
    const auto local = ParseThumbnailFormID(form);
    if (!local)
      return {};
    path =
        PackDocument::IconRelative(".hover", plugin, *local).generic_string();
  }
  const auto texture = Widgets::Texture(Widgets::Root() / path);
  if (!texture && requestedResourceIcons.insert(path).second)
    Send({{"action", "tableIcon"},
          {"plugin", plugin},
          {"formId", form},
          {"picker", picker},
          {"iconYaw", item.value("iconYaw", 0.F)},
          {"iconLight", item.value("iconLight", 1.F)}});
  return texture;
}
bool CellSelectable(const char *label, ImTextureID picture = {},
                    bool reservePicture = false) {
  using namespace ImGui;
  auto *table = GetCurrentTable();
  auto *cellWindow = GetCurrentWindow();
  const auto id = cellWindow->GetID(label);
  ItemSize(ImVec2{0, GetFrameHeight()});
  const auto rect = TableGetCellBgRect(table, table->CurrentColumn);
  bool hovered = false, held = false;
  const bool added = ItemAdd(rect, id);
  const bool clicked = added && ButtonBehavior(rect, id, &hovered, &held);
  if (hovered)
    TableSetBgColor(ImGuiTableBgTarget_CellBg,
                    GetColorU32(ImGuiCol_HeaderHovered));
  const char *end = FindRenderedTextEnd(label);
  const auto size = CalcTextSize(label, end);
  cellWindow->DrawList->PushClipRect(rect.Min, rect.Max, true);
  const float side = GetFrameHeight();
  const float x = rect.Min.x + GetStyle().CellPadding.x;
  const float y = rect.Min.y + (rect.GetHeight() - side) * .5F;
  if (picture)
    cellWindow->DrawList->AddImage(picture, {x, y}, {x + side, y + side},
                                   {0, 0}, {1, 1},
                                   GetColorU32(ImVec4{1, 1, 1, 1}));
  cellWindow->DrawList->AddText(
      {x + (reservePicture ? side + GetStyle().ItemSpacing.x : 0),
       rect.Min.y + (rect.GetHeight() - size.y) * .5F},
      GetColorU32(ImGuiCol_Text), label, end);
  cellWindow->DrawList->PopClipRect();
  return clicked;
}
void ResourcePicture(const std::string &plugin, std::uint32_t local,
                     bool temporary) {
  using namespace ImGui;
  constexpr float size = 34.F;
  const auto path = PackDocument::IconRelative(
      temporary ? ".resource-search" : ".resources", plugin, local);
  if (IsRectVisible({size, size})) {
    if (auto texture = Widgets::Texture(Widgets::Root() / path))
      ImageWithBg(texture, {size, size});
    else {
      Dummy({size, size});
      if (requestedResourceIcons.insert(path.generic_string()).second)
        Send({{"action", "resourceIcon"},
              {"plugin", plugin},
              {"formId", std::format("0x{:06X}", local)},
              {"temporary", temporary}});
    }
  } else
    Dummy({size, size});
  SameLine();
}
void ResourcePicture(const Json &material, bool temporary = false) {
  const auto found = material.find("formId");
  std::optional<std::uint32_t> local;
  if (found != material.end()) {
    if (found->is_string())
      local = ParseThumbnailFormID(found->get<std::string>());
    else if (found->is_number_integer() && *found > 0 && *found <= 0xFFFFFF)
      local = found->get<std::uint32_t>();
  }
  if (local)
    ResourcePicture(material.value("plugin", std::string{}), *local, temporary);
}
void Hint(const char *text) {
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", text);
}
void HoverIcon(const Json &item, bool picker = false) {
  using namespace ImGui;
  if (!IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                     ImGuiHoveredFlags_DelayNormal))
    return;
  std::string path = item.value("icon", "");
  if (picker && item.contains("id")) {
    const auto state = snapshot.load();
    if (state && state->contains("hoverPreview") &&
        (*state)["hoverPreview"].value("id", 0U) == item["id"].get<unsigned>())
      path = (*state)["hoverPreview"].value("path", "");
    static unsigned requested{};
    static double lastRequest{};
    const auto id = item["id"].get<unsigned>();
    if (path.empty() && (requested != id || GetTime() - lastRequest > .75)) {
      requested = id;
      lastRequest = GetTime();
      Send({{"action", "hoverPreview"}, {"id", id}});
    }
  }
  BeginTooltip();
  Widgets::WorkspaceFont();
  if (!path.empty())
    Widgets::Picture(Widgets::Root() / path, 180);
  else
    TextDisabled("Generating preview...");
  TextUnformatted(item.value("name", item.value("editorID", "")).c_str());
  EndTooltip();
}
void Step(const char *label, const char *icon, int &value) {
  using namespace ImGui;
  Widgets::FieldLabel(label, GetContentRegionAvail().x);
  const bool light = std::string_view(icon) == "brightness";
  constexpr std::array<const char *, 4> rotations{"0 degrees", "90 degrees",
                                                  "180 degrees", "270 degrees"};
  constexpr std::array<const char *, 4> lights{"50%", "100%", "150%", "200%"};
  const auto &choices = light ? lights : rotations;
  if (BeginCombo((std::string("##Preview") + icon).c_str(),
                 choices[std::clamp(value, 0, 3)])) {
    for (int i = 0; i < 4; ++i)
      if (Selectable(choices[i], value == i) && value != i) {
        value = i;
        Send({{"action", "adjustIcon"},
              {"draft", editingDraft},
              {"index", selectedItem},
              {"control", light ? "light" : "rotation"},
              {"step", value}});
      }
    EndCombo();
  }
}
void FormSection(const char *title, const char *automatic = nullptr) {
  using namespace ImGui;
  Widgets::SectionHeading(title);
  if (automatic) {
    SameLine();
    if (AutoButton())
      Send({{"action", "autoAssign"},
            {"draft", editingDraft},
            {"index", selectedItem},
            {"section", automatic}});
  }
  Widgets::SectionDivider();
}
void RequiredMaterials() {
  using namespace ImGui;
  static std::string query;
  static std::uint64_t request{};
  static std::optional<std::size_t> replacing;
  static bool focusSearch{};
  const auto searchMaterials = [&] {
    Send({{"action", "materialSearch"},
          {"query", query},
          {"requestId", ++request}});
  };
  FormSection("Building costs", "costs");
  auto required = editItem.value("requiredMaterials", Json::array());
  bool openPicker = false;
  ImVec2 anchor{};
  const auto selectMaterial = [&](std::optional<std::size_t> index) {
    replacing = index;
    query.clear();
    materialResults.store(nullptr);
    searchMaterials();
    focusSearch = true;
    anchor = {GetItemRectMin().x,
              GetItemRectMax().y + GetStyle().ItemSpacing.y};
    openPicker = true;
  };
  BeginDisabled(required.size() >= 64);
  if (Widgets::QuietButton("Add material"))
    selectMaterial(std::nullopt);
  EndDisabled();

  std::optional<std::size_t> remove;
  bool changed = false;
  if (BeginTable("RequiredMaterials", 3,
                 ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX |
                     ImGuiTableFlags_Resizable)) {
    TableSetupColumn("Material", ImGuiTableColumnFlags_WidthStretch);
    TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed,
                     CalcTextSize("0000000").x + 2 * GetStyle().FramePadding.x);
    TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                     CalcTextSize("Remove").x + 2 * GetStyle().FramePadding.x);
    TableHeadersRow();
    for (std::size_t i = 0; i < required.size(); ++i) {
      auto &entry = required[i];
      PushID(static_cast<int>(i));
      const float rowHeight = std::max(34.F, GetFrameHeight());
      TableNextRow(0, rowHeight + GetStyle().CellPadding.y * 2);
      TableSetColumnIndex(0);
      const auto origin = GetCursorScreenPos();
      ResourcePicture(entry, editingDraft);
      SetCursorScreenPos(origin);
      const auto label = entry.value("name", std::string{});
      if (Selectable("##Material", false, 0, {0, rowHeight}))
        selectMaterial(i);
      RenderTextClipped({origin.x + 34.F + GetStyle().ItemSpacing.x, origin.y},
                        GetItemRectMax(), label.c_str(), nullptr, nullptr,
                        {0, .5F});
      TableSetColumnIndex(1);
      SetCursorPosY(GetCursorPosY() + (rowHeight - GetFrameHeight()) * .5F);
      int amount = entry.value("amount", 1);
      SetNextItemWidth(-1);
      if (InputInt("##Amount", &amount, 0, 0)) {
        entry["amount"] = std::clamp(amount, 1, 1000000);
        changed = true;
      }
      TableSetColumnIndex(2);
      SetCursorPosY(GetCursorPosY() + (rowHeight - GetFrameHeight()) * .5F);
      if (Widgets::QuietButton("Remove", true))
        remove = i;
      PopID();
    }
    if (required.empty()) {
      TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
      TableSetColumnIndex(0);
      TextDisabled("No required materials");
    }
    EndTable();
  }
  if (remove) {
    required.erase(required.begin() + *remove);
    changed = true;
  }
  if (changed) {
    editItem["requiredMaterials"] = required;
    Edit({{"requiredMaterials", required}});
  }
  if (openPicker) {
    SetNextWindowPos(anchor);
    OpenPopup("Material picker");
  }
  SetNextWindowSize({std::min(360.F, GetIO().DisplaySize.x - 40.F),
                     std::min(400.F, GetIO().DisplaySize.y - 40.F)});
  if (BeginPopup("Material picker", ImGuiWindowFlags_NoResize)) {
    if (focusSearch) {
      SetKeyboardFocusHere();
      focusSearch = false;
    }
    if (Widgets::Input("##MaterialSearch", query, GetContentRegionAvail().x))
      searchMaterials();
    const auto found = materialResults.load();
    BeginChild("Material choices", {0, 0});
    if (found && found->value("requestId", 0ULL) == request) {
      const auto &entries = Array(*found, "results");
      if (entries.empty())
        TextDisabled("No matching materials");
      for (const auto &entry : entries) {
        const bool added = std::ranges::any_of(required, [&](const auto &cost) {
          return PackDocument::SameItemSource(cost, entry);
        });
        PushID((entry.value("plugin", "") + entry.value("formId", "")).c_str());
        BeginDisabled(added);
        const auto rowStart = GetCursorScreenPos();
        const float rowHeight = std::max(34.F, GetFrameHeight());
        const bool picked =
            Selectable("##MaterialResult", added, 0, {0, rowHeight});
        const auto afterRow = GetCursorScreenPos();
        SetCursorScreenPos(rowStart);
        ResourcePicture(entry, true);
        const auto label = entry.value("name", "");
        GetWindowDrawList()->AddText(
            {GetCursorScreenPos().x,
             rowStart.y + (rowHeight - GetTextLineHeight()) * .5F},
            GetColorU32(ImGuiCol_Text), label.c_str());
        SetCursorScreenPos(afterRow);
        if (picked) {
          auto material = entry;
          material["amount"] = replacing && *replacing < required.size()
                                   ? required[*replacing].value("amount", 1)
                                   : 1;
          if (replacing && *replacing < required.size())
            required[*replacing] = std::move(material);
          else
            required.push_back(std::move(material));
          editItem["requiredMaterials"] = required;
          Edit({{"requiredMaterials", required}});
          CloseCurrentPopup();
        }
        EndDisabled();
        PopID();
      }
    }
    EndChild();
    if (Widgets::EscapePressed())
      CloseCurrentPopup();
    EndPopup();
  }
}
void ItemForm(const Json &state) {
  using namespace ImGui;
  const bool busy =
      state.value("busy", false) ||
      state.value("progress", Json::object()).value("running", false);
  BeginDisabled(state.value("shellAnalyzing", false));
  const float previewWidth =
      std::clamp(GetContentRegionAvail().x * .16F, 138.F, 236.F);
  if (!BeginTable("ItemEditorPanels", 3,
                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                      ImGuiTableFlags_SizingStretchProp,
                  {0, GetContentRegionAvail().y})) {
    EndDisabled();
    return;
  }
  TableSetupColumn("Thumbnail", ImGuiTableColumnFlags_WidthFixed, previewWidth);
  TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, .50F);
  TableSetupColumn("Building costs", ImGuiTableColumnFlags_WidthStretch, .44F);
  TableNextRow();
  TableSetColumnIndex(0);
  BeginChild("ItemPreview", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
  FormSection("Preview");
  const std::string identity =
      editItem.value("plugin", "") + ":" + editItem.value("formId", "");
  static std::string previewIdentity;
  const bool ready = !editingDraft || state.value("draftReady", false);
  if (ready)
    previewIdentity = identity;
  const auto box = GetCursorScreenPos();
  const float imageSize = std::clamp(GetContentRegionAvail().x, 1.F, 176.F);
  const ImVec2 imagePos{box.x + (GetContentRegionAvail().x - imageSize) * .5F,
                        box.y};
  ImDrawListManager::AddRectFilled(
      GetWindowDrawList(), imagePos,
      {imagePos.x + imageSize, imagePos.y + imageSize}, 0xFF171717, 6, 0);
  SetCursorScreenPos(imagePos);
  if (ready || previewIdentity == identity)
    Widgets::Picture(
        Widgets::Root() /
            (editingDraft ? std::string(ThumbnailGenerator::PICKER_PREVIEW_PATH)
                          : editItem.value("icon", "")),
        imageSize);
  else
    Dummy({imageSize, imageSize});
  if (!ready || busy) {
    const char *status = editingDraft && state.value("draftFailed", false)
                             ? "Preview unavailable"
                             : "Updating preview...";
    const auto size = CalcTextSize(status);
    ImDrawListManager::AddRectFilled(
        GetWindowDrawList(), {imagePos.x, imagePos.y + imageSize - 36.F},
        {imagePos.x + imageSize, imagePos.y + imageSize}, 0xDA171717, 6, 0);
    ImDrawListManager::AddText(GetWindowDrawList(),
                               {imagePos.x + (imageSize - size.x) * .5F,
                                imagePos.y + imageSize - 28.F},
                               0xFFDDDDDD, status);
  }
  BeginDisabled(busy && !editingDraft);
  int yaw = static_cast<int>(editItem.value("iconYaw", 0.F) / 90.F);
  Step("Rotation", "rotate", yaw);
  editItem["iconYaw"] = yaw * 90.F;
  int light = std::clamp(
      static_cast<int>(std::round(editItem.value("iconLight", 1.F) * 2)) - 1, 0,
      3);
  Step("Lighting", "brightness", light);
  editItem["iconLight"] = (light + 1) * .5F;
  if (Widgets::FormButton("Regenerate"))
    Send({{"action", "regenerate"},
          {"draft", editingDraft},
          {"index", selectedItem}});
  EndDisabled();
  EndChild();
  TableSetColumnIndex(1);
  BeginChild("ItemProperties", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
  PushItemWidth(-1);
  FormSection("Item details", "details");
  auto name = editItem.value("name", "");
  Widgets::FieldLabel("Name", GetContentRegionAvail().x);
  if (Widgets::Input("##ItemName", name, -1)) {
    editItem["name"] = name;
    Edit({{"name", name}});
  }
  TextDisabled("%s  /  %s", editItem.value("plugin", "").c_str(),
               editItem.value("formId", "").c_str());
  BeginDisabled(busy);
  if (Widgets::FormButton("Recalculate bounds"))
    Send({{"action", "recalculateBounds"},
          {"draft", editingDraft},
          {"index", selectedItem}});
  EndDisabled();
  auto category = editItem.value("category", "");
  Widgets::FieldLabel("Category");
  const float categoryButtonWidth =
      CalcTextSize("New category").x + GetStyle().FramePadding.x * 2;
  SetNextItemWidth(std::max(1.F, GetContentRegionAvail().x -
                                     categoryButtonWidth -
                                     GetStyle().ItemSpacing.x));
  if (Widgets::Combo("##Category",
                     category.empty() ? "Other" : category.c_str())) {
    for (const auto &entry : Array(state, "categories")) {
      const auto text = entry.get<std::string>();
      if (Selectable(text.c_str(), text == category)) {
        editItem["category"] = text;
        Edit({{"category", text}});
      }
    }
    EndCombo();
  }
  SameLine();
  if (Button("New category", {categoryButtonWidth, 0})) {
    newName.clear();
    OpenPopup("Item category");
  }
  if (BeginPopupModal("Item category", nullptr,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
    Widgets::WorkspaceFont();
    Widgets::FieldLabel("Name");
    Widgets::Input("##Name", newName, 300);
    Widgets::SpaceBefore();
    if (Widgets::PrimaryButton("Create")) {
      Send({{"action", "categoryCreate"},
            {"name", newName},
            {"assign", {{"draft", editingDraft}, {"index", selectedItem}}}});
      CloseCurrentPopup();
    }
    SameLine();
    if (Button("Cancel"))
      CloseCurrentPopup();
    if (Widgets::EscapePressed())
      CloseCurrentPopup();
    EndPopup();
  }
  FormSection("Placement");
  const bool door = editItem.value("isDoor", false);
  BeginDisabled(door);
  auto type = editItem.value("type", "basic");
  const auto matching = PackDocument::MatchingSnappingPreset(editItem);
  {
    Widgets::FieldLabel("Preset", GetContentRegionAvail().x);
    SetNextItemWidth(-1);
    if (Widgets::Combo("##Preset", matching.c_str())) {
      for (const auto *preset :
           {"prop", "floor", "foundation", "cell", "wall", "pillar"})
        if (Selectable(preset, matching == preset)) {
          auto values = PackDocument::SnappingPreset(preset);
          values["doorway"] =
              values["type"] == "side" && editItem.value("doorway", false);
          editItem.update(values);
          type = values["type"].get<std::string>();
          Edit(values);
        }
      EndCombo();
    }
    Widgets::FieldLabel("Snapping", GetContentRegionAvail().x);
    SetNextItemWidth(-1);
    if (Widgets::Combo("##Snapping", type.c_str())) {
      constexpr std::array<const char *, 4> modes{"none", "basic", "side",
                                                  "corner"};
      constexpr std::array<const char *, 4> descriptions{
          "Props placed freely without grid snapping.",
          "Pieces occupying a grid cell: floors, foundations and roofs.",
          "Walls and doorways placed on a cell face.",
          "Posts and pillars placed along a cell corner."};
      for (std::size_t i = 0; i < modes.size(); ++i) {
        if (Selectable(modes[i], type == modes[i])) {
          Json values{{"type", modes[i]},
                      {"occupy", std::string_view(modes[i]) == "basic"
                                     ? editItem.value("occupy", Json::array())
                                     : Json::array()},
                      {"doorway", std::string_view(modes[i]) == "side" &&
                                      editItem.value("doorway", false)}};
          editItem.update(values);
          type = modes[i];
          Edit(values);
        }
        if (IsItemHovered())
          SetTooltip("%s", descriptions[i]);
      }
      EndCombo();
    }
  }
  if (type == "none") {
    bool align = editItem.value("alignToWall", false);
    Widgets::SpaceBefore();
    if (Checkbox("Align to wall", &align)) {
      editItem["alignToWall"] = align;
      Edit({{"alignToWall", align}});
    }
  }
  if (type == "basic") {
    Widgets::FieldLabel("Occupancy");
    auto occupancy = editItem.value("occupy", Json::array());
    if (BeginTable("OccupancyOptions",
                   GetContentRegionAvail().x >= 330.F ? 3 : 2,
                   ImGuiTableFlags_NoPadOuterX)) {
      for (const auto *part : {"center", "bottom", "top", "sides", "corners"}) {
        TableNextColumn();
        bool selected = std::find(occupancy.begin(), occupancy.end(),
                                  Json(part)) != occupancy.end();
        if (Checkbox(part, &selected)) {
          if (selected)
            occupancy.push_back(part);
          else
            occupancy.erase(
                std::remove(occupancy.begin(), occupancy.end(), Json(part)),
                occupancy.end());
          editItem["occupy"] = occupancy;
          Edit({{"occupy", occupancy}});
        }
      }
      EndTable();
    }
  }
  if (type == "side") {
    bool doorway = editItem.value("doorway", false);
    Widgets::SpaceBefore();
    if (Checkbox("Doorway", &doorway)) {
      editItem["doorway"] = doorway;
      Edit({{"doorway", doorway}});
    }
    Hint("Allows a door to be placed inside this piece.");
  }
  EndDisabled();
  if (door)
    TextDisabled("Doors snap inside unfilled doorways.");
  FormSection("Transform offsets");
  for (const auto *field : {"rotationOffset", "freePlaceOffset"}) {
    const bool rotation = std::string_view(field) == "rotationOffset";
    Widgets::FieldLabel(rotation ? "Rotation (degrees) - X / Y / Z"
                                 : "Position (units) - X / Y / Z",
                        GetContentRegionAvail().x);
    auto value =
        editItem.value(field, Json{{"x", 0.F}, {"y", 0.F}, {"z", 0.F}});
    float xyz[]{value.value("x", 0.F), value.value("y", 0.F),
                value.value("z", 0.F)};
    if (InputFloat3((std::string("##") + field).c_str(), xyz, "%.2f")) {
      Json replacement{{"x", xyz[0]}, {"y", xyz[1]}, {"z", xyz[2]}};
      editItem[field] = replacement;
      Edit({{field, replacement}});
    }
  }
  PopItemWidth();
  EndChild();
  EndDisabled();
  TableSetColumnIndex(2);
  BeginChild("ItemBuildingCosts", {0, 0},
             ImGuiChildFlags_AlwaysUseWindowPadding);
  BeginDisabled(state.value("shellAnalyzing", false));
  RequiredMaterials();
  EndDisabled();
  // Reserve the row so starting/stopping analysis doesn't shift the content.
  Spacing();
  if (state.value("shellAnalyzing", false)) {
    const float cancelWidth =
        CalcTextSize("Cancel").x + GetStyle().FramePadding.x * 2;
    const float width = std::max(1.F, GetContentRegionAvail().x - cancelWidth -
                                          GetStyle().ItemSpacing.x);
    // Direct2D's union calculation has no intra-call percentage reporting.
    // Show honest indeterminate progress rather than a timer-based estimate.
    ProgressBar(-static_cast<float>(GetTime() + 1), {width, GetFrameHeight()},
                state.value("shellReading", false) ? "Reading mesh..."
                                                   : "Measuring footprints...");
    SameLine();
    if (Button("Cancel", {cancelWidth, GetFrameHeight()}))
      Send({{"action", "cancelCostAnalysis"},
            {"epoch", state.value("shellEpoch", std::uint64_t{})}});
  } else {
    Dummy({0, GetFrameHeight()});
  }
  EndChild();
  EndTable();
}
void SelectEdit(const Json &item, std::size_t index, bool draft,
                const Json &state) {
  editItem = item;
  selectedItem = index;
  editingDraft = draft;
  editing = true;
  editRevision = state.value("revision", std::uint64_t{});
}
struct Node {
  std::map<std::string, Node> children;
  std::vector<std::size_t> files;
  std::set<std::string> users;
  bool checked{}, selectable{}, all{true};
};
void AssetTree(const std::shared_ptr<const Json> &source) {
  const auto &all = *source;
  using namespace ImGui;
  static Node root;
  static std::shared_ptr<const Json> previous;
  static bool previousBase{};
  if (previous != source || previousBase != showBase) {
    previous = source;
    previousBase = showBase;
    root = {};
    for (std::size_t i = 0; i < all.size(); ++i) {
      const auto &entry = all[i];
      if (!showBase && entry.value("gameFile", false))
        continue;
      Node *node = &root;
      const auto path = entry.value("path", "");
      std::size_t at = 0;
      while (true) {
        node->checked |= entry.value("include", false);
        if (entry.value("available", false) && !entry.value("gameFile", false))
          node->all &= entry.value("include", false);
        node->selectable |=
            entry.value("available", false) && !entry.value("gameFile", false);
        for (const auto &user : Array(entry, "usedBy"))
          node->users.insert(user.get<std::string>());
        const auto slash = path.find('/', at);
        if (slash == std::string::npos)
          break;
        node = &node->children[path.substr(at, slash - at)];
        at = slash + 1;
      }
      node->files.push_back(i);
    }
  }
  auto row = [&](const auto &self, const Node &node,
                 const std::string &prefix) -> void {
    for (const auto &[name, child] : node.children) {
      const auto path = prefix + name + "/";
      PushID(path.c_str());
      TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
      TableNextColumn();
      bool selected = child.checked;
      BeginDisabled(!child.selectable);
      PushItemFlag(ImGuiItemFlags_MixedValue, child.checked && !child.all);
      if (Checkbox("##include", &selected))
        Send({{"action", "selectAssetFolder"},
              {"prefix", path},
              {"value", selected}});
      PopItemFlag();
      EndDisabled();
      SameLine();
      const bool expanded =
          TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
      TableNextColumn();
      Text("%zu", child.users.size());
      if (expanded) {
        self(self, child, path);
        TreePop();
      }
      PopID();
    }
    Widgets::Rows(node.files.size(), [&](std::size_t row) {
      const auto i = node.files[row];
      const auto &entry = all[i];
      PushID(static_cast<int>(i));
      TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
      TableNextColumn();
      bool selected = entry.value("include", false);
      BeginDisabled(!entry.value("available", false) ||
                    entry.value("gameFile", false));
      if (Checkbox("##include", &selected))
        Send({{"action", "include"},
              {"kind", "assets"},
              {"path", entry["path"]},
              {"value", selected}});
      SameLine();
      TextUnformatted(std::filesystem::path(entry.value("path", ""))
                          .filename()
                          .string()
                          .c_str());
      EndDisabled();
      TableNextColumn();
      Text("%zu", Array(entry, "usedBy").size());
      PopID();
    });
  };
  PushStyleVar(ImGuiStyleVar_FramePadding, {8, 0});
  if (BeginTable("AssetExplorer", 2,
                 ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersInnerH |
                     ImGuiTableFlags_Resizable)) {
    TableSetupColumn("Name");
    TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, 90);
    TableHeadersRow();
    row(row, root, "");
    EndTable();
  }
  PopStyleVar();
}
void __stdcall Draw() {
  if (pump)
    pump();
  const auto owned = snapshot.load();
  if (!owned)
    return;
  const auto &state = *owned;
  if (!state.value("draft", Json()).is_null() &&
      openPickedItem.exchange(false)) {
    editing = false;
    editingDraft = true;
    expectedDraft = state["draft"].value("plugin", "") + ":" +
                    state["draft"].value("formId", "");
    requestAdd = true;
  }
  framePack = state.value("selected", "");
  Widgets::BeginFrame();
  Widgets::Theme theme;
  Widgets::WorkspaceTheme workspace;
  using namespace ImGui;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_Appearing,
                   {.5F, .5F});
  SetNextWindowSize(
      {std::min(1200.F, screen.x * .94F), std::min(860.F, screen.y * .94F)},
      ImGuiCond_Appearing);
  bool shown = true;
  SetNextWindowSizeConstraints(
      {std::min(960.F, screen.x * .94F), std::min(600.F, screen.y * .94F)},
      screen);
  if (Begin("Pack Editor##SkyBuildPackEditor", &shown,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse)) {
    Widgets::WorkspaceFont();
    PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {.5F, .5F});
    PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.F, .5F});
    const bool loaded = state.value("loaded", false),
               busy = state.value("busy", false);
    BeginDisabled(busy);
    const auto pack = state.value("selected", "");
    const bool unsaved = state.value("dirty", false);
    BeginDisabled(unsaved);
    SetNextItemWidth(270);
    if (Widgets::Combo("##Pack",
                       pack.empty() ? "Select a pack" : pack.c_str())) {
      for (const auto &entry : Array(state, "packs")) {
        const auto name = entry.get<std::string>();
        if (Selectable(name.c_str(), name == pack))
          Send({{"action", "loadPack"}, {"name", name}});
      }
      EndCombo();
    }
    if (unsaved && IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      SetTooltip("Save or revert the current pack before switching packs.");
    SameLine();
    if (Button("New pack")) {
      nameAction = "createPack";
      newName.clear();
      requestName = true;
    }
    EndDisabled();
    if (unsaved && IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      SetTooltip("Save or revert the current pack before switching packs.");
    const float actionWidth =
        CalcTextSize("Save Changes").x + CalcTextSize("Revert").x +
        GetStyle().FramePadding.x * 4 + GetStyle().ItemSpacing.x;
    SameLine(
        std::max(GetCursorPosX(),
                 GetWindowWidth() - GetStyle().WindowPadding.x - actionWidth));
    BeginDisabled(!loaded || !state.value("dirty", false));
    if (Widgets::PrimaryButton("Save Changes"))
      Send({{"action", "save"}});
    EndDisabled();
    SameLine();
    BeginDisabled(!loaded || !state.value("dirty", false));
    if (Widgets::QuietButton("Revert")) {
      pendingRemoval = {{"action", "discard"}};
      removalLabel = "You have unsaved changes. Reverting will discard them "
                     "and reload this pack from disk. Are you sure?";
      requestRemoval = true;
    }
    EndDisabled();
    Separator();
    static int section = 0;
    constexpr const char *sections[]{"Items", "Categories", "Settings",
                                     "Export"};
    BeginChild(
        "PackWorkspace",
        {0, -(GetFrameHeightWithSpacing() * 2 + GetStyle().ItemSpacing.y * 2)},
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    BeginChild(
        "PackNavigation", {164, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Widgets::SidebarHeading("WORKSPACE");
    for (int i = 0; i < 4; ++i)
      if (Widgets::SidebarItem(sections[i], section == i))
        section = i;
    EndChild();
    SameLine();
    PushStyleVar(ImGuiStyleVar_FramePadding, {8, 3});
    PushStyleVar(ImGuiStyleVar_CellPadding, {10, 3});
    PushID(section);
    BeginChild("PackContent", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
    Widgets::PageHeading(sections[section]);
    BeginDisabled(!loaded);
    {
      if (section == 0) {
        if (Widgets::PrimaryButton("Add item")) {
          editing = false;
          editingDraft = true;
          expectedDraft.clear();
          Send({{"action", "clearDraft"}});
          search.clear();
          results.store(nullptr);
          Send({{"action", "search"},
                {"query", ""},
                {"plugin", pluginFilter},
                {"recordType", recordType},
                {"requestId", ++searchRequest}});
          requestAdd = true;
        }
        SameLine();
        if (Button("Add Item From Selection"))
          Send({{"action", "selectWorldItem"}});
        Widgets::SpaceBefore();
        AlignTextToFramePadding();
        TextUnformatted("Filter");
        SameLine();
        if (Widgets::Input("##FilterItems", filter,
                           std::max(1.F, GetContentRegionAvail().x - 170.F -
                                             GetStyle().ItemSpacing.x)))
          Send({{"action", "itemFilter"}, {"value", filter}});
        SameLine();
        SetNextItemWidth(170);
        const auto currentCategory = state.value("categoryFilter", Json());
        const auto categoryLabel = currentCategory.is_null()
                                       ? std::string("All categories")
                                       : currentCategory.get<std::string>();
        if (Widgets::Combo("##CategoryFilter", categoryLabel.empty()
                                                   ? "Other"
                                                   : categoryLabel.c_str())) {
          if (Selectable("All categories", currentCategory.is_null()))
            Send({{"action", "itemFilter"},
                  {"value", filter},
                  {"category", nullptr}});
          for (const auto &entry : Array(state, "usedCategories")) {
            const auto name = entry.get<std::string>();
            if (Selectable(name.c_str(), currentCategory == entry))
              Send({{"action", "itemFilter"},
                    {"value", filter},
                    {"category", name}});
          }
          EndCombo();
        }
        TextDisabled("%zu items", state.value("filteredCount", std::size_t{}));
        if (BeginTable("ItemsOrder", 7,
                       ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                       {0, std::max(80.F, GetContentRegionAvail().y)})) {
          TableSetupScrollFreeze(0, 1);
          TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 84.F);
          TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.2F);
          TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.F);
          TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 1.5F);
          TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthFixed, 95.F);
          TableSetupColumn("Snapping", ImGuiTableColumnFlags_WidthFixed, 75.F);
          TableSetupColumn("Actions",
                           ImGuiTableColumnFlags_NoSort |
                               ImGuiTableColumnFlags_WidthFixed,
                           90);
          TableHeadersRow();
          const auto &rows = Array(state, "items");
          ImGuiListClipper clipper;
          clipper.Begin(static_cast<int>(rows.size()));
          while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                 ++row) {
              const auto &entry = rows[row];
              const auto index = entry["index"].get<std::size_t>();
              const auto &item = entry["item"];
              PushID(static_cast<int>(index));
              TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
              Json source{{"plugin", item["plugin"]},
                          {"formId", item["formId"]}};
              if (!item.value("itemId", "").empty())
                source["itemId"] = item["itemId"];
              TableNextColumn();
              AlignTextToFramePadding();
              Text("%zu", index + 1);
              SameLine();
              BeginDisabled(entry.value("position", std::size_t{}) == 0);
              if (OrderArrow("##up", ImGuiDir_Up))
                Send({{"action", "itemMove"},
                      {"source", source},
                      {"direction", -1}});
              EndDisabled();
              SameLine();
              BeginDisabled(entry.value("position", std::size_t{}) + 1 >=
                            state.value("filteredCount", std::size_t{}));
              if (OrderArrow("##down", ImGuiDir_Down))
                Send({{"action", "itemMove"},
                      {"source", source},
                      {"direction", 1}});
              EndDisabled();
              bool rowHovered = false;
              for (const auto *field :
                   {"name", "category", "plugin", "formId", "type"}) {
                TableNextColumn();
                const auto text = item.value(field, "");
                PushID(field);
                if (CellSelectable((text + "##cell").c_str(),
                                   std::string_view(field) == "name"
                                       ? ItemPicture(item, false)
                                       : nullptr,
                                   std::string_view(field) == "name")) {
                  SelectEdit(item, index, false, state);
                  requestEdit = true;
                }
                rowHovered |= IsItemHovered();
                if (BeginDragDropSource()) {
                  const auto payload = source.dump();
                  SetDragDropPayload("SkyBuildItem", payload.c_str(),
                                     payload.size() + 1);
                  TextUnformatted(item.value("name", "Item").c_str());
                  EndDragDropSource();
                }
                if (BeginDragDropTarget()) {
                  if (const auto payload =
                          AcceptDragDropPayload("SkyBuildItem"))
                    Send({{"action", "itemMove"},
                          {"source", Json::parse(static_cast<const char *>(
                                         payload->Data))},
                          {"before", source}});
                  EndDragDropTarget();
                }
                HoverIcon(item);
                PopID();
              }
              if (rowHovered)
                for (int column = 1; column < 6; ++column)
                  TableSetBgColor(ImGuiTableBgTarget_CellBg,
                                  GetColorU32(ImVec4{.29F, .38F, .41F, .27F}),
                                  column);
              TableNextColumn();
              if (Widgets::QuietButton("Remove", true, true)) {
                pendingRemoval = {
                    {"action", "removeItem"},
                    {"source",
                     {{"plugin", item["plugin"]}, {"formId", item["formId"]}}}};
                removalLabel =
                    "Remove " + item.value("name", "") + " from this pack?";
                requestRemoval = true;
              }
              PopID();
            }
          }
          EndTable();
        }
      }
      if (section == 1) {
        if (Widgets::PrimaryButton("New category")) {
          nameAction = "categoryCreate";
          newName.clear();
          requestName = true;
        }
        const auto categories = Array(state, "categories");
        if (BeginTable("CategoryList", 2,
                       ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg)) {
          TableSetupColumn("Category");
          TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 230);
          TableHeadersRow();
          for (std::size_t i = 0; i < categories.size(); ++i) {
            const auto name = categories[i].get<std::string>();
            const bool builtin = Packs::IsDefaultCategory(name);
            PushID(static_cast<int>(i));
            TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
            TableNextColumn();
            if (CellSelectable(name.c_str()) && !builtin) {
              nameAction = "categoryRename";
              newName = oldName = name;
              nameIndex = i;
              requestName = true;
            }
            if (Packs::CategoryKey(name) != "other" && BeginDragDropSource()) {
              SetDragDropPayload("SkyBuildCategory", name.c_str(),
                                 name.size() + 1);
              TextUnformatted(name.c_str());
              EndDragDropSource();
            }
            if (BeginDragDropTarget()) {
              if (const auto payload =
                      AcceptDragDropPayload("SkyBuildCategory"))
                Send({{"action", "categoryMove"},
                      {"name", static_cast<const char *>(payload->Data)},
                      {"before", name}});
              EndDragDropTarget();
            }
            TableNextColumn();
            BeginDisabled(i == 0 || Packs::CategoryKey(name) == "other");
            if (OrderArrow("##up", ImGuiDir_Up))
              Send({{"action", "categoryMove"},
                    {"name", name},
                    {"before", categories[i - 1]}});
            EndDisabled();
            SameLine();
            BeginDisabled(i + 2 >= categories.size() ||
                          Packs::CategoryKey(name) == "other");
            if (OrderArrow("##down", ImGuiDir_Down))
              Send({{"action", "categoryMove"},
                    {"name", name},
                    {"before", i + 2 < categories.size() ? categories[i + 2]
                                                         : Json("")}});
            EndDisabled();
            SameLine();
            BeginDisabled(builtin);
            if (Widgets::QuietButton("Rename", false, true)) {
              nameAction = "categoryRename";
              newName = oldName = name;
              nameIndex = i;
              requestName = true;
            }
            SameLine();
            if (Widgets::QuietButton("Delete", true, true)) {
              pendingRemoval = {{"action", "categoryDelete"}, {"old", name}};
              removalLabel = "Remove category " + name +
                             "? Items in it will move to Other.";
              requestRemoval = true;
            }
            EndDisabled();
            PopID();
          }
          EndTable();
        }
      }
      if (section == 2) {
        bool extend = state.value("extendBasePack", false);
        if (Checkbox("Include in the SkyBuild base pack", &extend))
          Send({{"action", "extend"}, {"value", extend}});
        Hint("Show this pack's items alongside the base SkyBuild pack.");
        if (Widgets::FormButton("Recalculate all building costs"))
          OpenPopup("Recalculate building costs?");
        if (BeginPopupModal("Recalculate building costs?", nullptr,
                            ImGuiWindowFlags_AlwaysAutoResize)) {
          TextWrapped(
              "Are you sure? This recalculates derived materials for every item in this pack. Separately added materials are preserved.");
          Widgets::SpaceBefore();
          if (Widgets::PrimaryButton("Recalculate")) {
            Send({{"action", "recalculateCosts"}});
            CloseCurrentPopup();
          }
          SameLine();
          if (Button("Cancel") || Widgets::EscapePressed())
            CloseCurrentPopup();
          EndPopup();
        }
        if (Widgets::FormButton("Recalculate all bounds"))
          OpenPopup("Recalculate all bounds?");
        if (BeginPopupModal("Recalculate all bounds?", nullptr,
                            ImGuiWindowFlags_AlwaysAutoResize)) {
          TextWrapped(
              "Are you sure? This reads every item's current mesh and replaces its saved selection bounds.");
          Widgets::SpaceBefore();
          if (Widgets::PrimaryButton("Recalculate")) {
            Send({{"action", "recalculateAllBounds"}});
            CloseCurrentPopup();
          }
          SameLine();
          if (Button("Cancel") || Widgets::EscapePressed())
            CloseCurrentPopup();
          EndPopup();
        }
        Widgets::SpaceBefore(Widgets::Gap::Section);
        Separator();
        Widgets::SpaceBefore();
        static std::string deleteConfirmation, deletePackName;
        BeginDisabled(state["progress"].value("running", false));
        if (Widgets::QuietButton("Delete Pack", true)) {
          deleteConfirmation.clear();
          deletePackName = pack;
          OpenPopup("Delete pack?");
        }
        EndDisabled();
        if (BeginPopupModal("Delete pack?", nullptr,
                            ImGuiWindowFlags_AlwaysAutoResize)) {
          Widgets::WorkspaceFont();
          TextWrapped("Permanently delete pack %s?", deletePackName.c_str());
          TextUnformatted(
              "Unsaved changes will be discarded. This cannot be undone.");
          TextUnformatted(
              "Placed objects, meshes, textures and icons will remain.");
          Widgets::FieldLabel("Type DELETE to confirm");
          if (IsWindowAppearing())
            SetKeyboardFocusHere();
          Widgets::Input("##DeletePackConfirmation", deleteConfirmation, 360);
          BeginDisabled(deleteConfirmation != "DELETE" ||
                        deletePackName != pack || busy);
          if (Widgets::QuietButton("Delete Pack", true)) {
            Send({{"action", "deletePack"},
                  {"confirmation", deleteConfirmation}});
            editing = false;
            expectedDraft.clear();
            CloseCurrentPopup();
          }
          EndDisabled();
          SameLine();
          if (Widgets::QuietButton("Cancel"))
            CloseCurrentPopup();
          if (Widgets::EscapePressed())
            CloseCurrentPopup();
          EndPopup();
        }
      }
      if (section == 3) {
        static std::uint64_t requested = ~0ULL;
        const auto revision = state.value("revision", std::uint64_t{});
        if (!state.value("exportReady", false) && !busy &&
            requested != revision) {
          requested = revision;
          Send({{"action", "prepareExport"}});
        }
        if (Button("Refresh dependencies"))
          Send({{"action", "prepareExport"}});
        SameLine();
        BeginDisabled(!state.value("exportReady", false));
        if (Widgets::PrimaryButton("Export Pack"))
          Send({{"action", "export"}});
        EndDisabled();
        Widgets::Section("Plugins");
        const auto &plugins = Array(state, "exportPlugins");
        if (plugins.empty()) {
          TextDisabled("No plugin dependencies.");
        } else {
          // Small lists fit naturally; larger packs scroll within a bounded
          // area so the asset browser remains visible on the same page.
          const float pluginHeight =
              std::min(30.F + static_cast<float>(plugins.size()) *
                                  Widgets::TableRowHeight,
                       std::max(72.F, GetContentRegionAvail().y * .3F));
          PushStyleVar(ImGuiStyleVar_FramePadding, {8, 0});
          if (BeginTable("Plugins", 2,
                         ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_ScrollY,
                         {0, pluginHeight})) {
            TableSetupColumn("Name");
            TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, 90);
            TableSetupScrollFreeze(0, 1);
            TableHeadersRow();
            for (const auto &entry : Array(state, "exportPlugins")) {
              const auto index = entry["index"].get<int>();
              PushID(index);
              TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
              TableNextColumn();
              bool include = entry.value("include", false);
              BeginDisabled(entry.value("gameFile", false) ||
                            !entry.value("available", false));
              if (Checkbox("##include", &include))
                Send({{"action", "include"},
                      {"kind", "plugins"},
                      {"path", entry["path"]},
                      {"value", include}});
              SameLine();
              TextUnformatted(entry.value("path", "").c_str());
              EndDisabled();
              TableNextColumn();
              AlignTextToFramePadding();
              Text("%zu", Array(entry, "usedBy").size());
              PopID();
            }
            EndTable();
          }
          PopStyleVar();
        }
        Widgets::Section("Assets");
        BeginChild("AssetTreeViewport", {0, -104});
        Widgets::WorkspaceFont();
        if (auto all = assets.load())
          AssetTree(all);
        EndChild();
        Checkbox("Show base game files", &showBase);
        Widgets::FieldLabel("Additional asset path");
        const float addWidth =
            CalcTextSize("Add asset").x + GetStyle().FramePadding.x * 2;
        Widgets::Input("##AdditionalAsset", extraAsset,
                       std::max(80.F, GetContentRegionAvail().x - addWidth -
                                          GetStyle().ItemSpacing.x));
        SameLine();
        if (Button("Add asset"))
          Send({{"action", "addAsset"}, {"path", extraAsset}});
      }
    }
    EndDisabled();
    EndChild();
    PopID();
    PopStyleVar(2);
    EndChild();
    EndDisabled();
    Separator();
    const auto &progress = state.at("progress");
    if (progress.value("running", false)) {
      const auto total = progress.value("total", std::size_t{}),
                 done = progress.value("completed", std::size_t{});
      ProgressBar(total ? static_cast<float>(done) / total : 0, {300, 0});
      SameLine();
      if (Button("Cancel generation"))
        Send({{"action", "cancelGeneration"}});
      TextUnformatted(progress.value("status", "").c_str());
    } else
      MessageFooter(state);
    if (requestEdit) {
      OpenPopup("Edit item");
      requestEdit = false;
    }
    if (requestAdd) {
      OpenPopup("Add item");
      requestAdd = false;
    }
    if (requestName) {
      OpenPopup(nameAction == "categoryRename" ? "Rename" : "New name");
      requestName = false;
    }
    if (requestRemoval) {
      OpenPopup("Are you sure?");
      requestRemoval = false;
    }
    if (BeginPopupModal("Are you sure?", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
      Widgets::WorkspaceFont();
      PushTextWrapPos(480.F);
      TextWrapped("%s", removalLabel.c_str());
      PopTextWrapPos();
      if (Widgets::FormButton(pendingRemoval.value("action", "") == "discard"
                                  ? "Revert"
                                  : "Remove",
                              true)) {
        Send(pendingRemoval);
        pendingRemoval = nullptr;
        CloseCurrentPopup();
      }
      SameLine();
      if (Button("Cancel")) {
        pendingRemoval = nullptr;
        CloseCurrentPopup();
      }
      if (Widgets::EscapePressed())
        CloseCurrentPopup();
      EndPopup();
    }
    if (BeginPopupModal(nameAction == "categoryRename" ? "Rename" : "New name",
                        nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      Widgets::WorkspaceFont();
      Widgets::Input("##Name", newName, 350);
      Widgets::SpaceBefore();
      if (Widgets::PrimaryButton(nameAction == "categoryRename" ? "Done"
                                                                : "Create",
                                 {90, GetFrameHeight()})) {
        Send({{"action", nameAction},
              {"name", newName},
              {"old", oldName},
              {"index", nameIndex}});
        CloseCurrentPopup();
      }
      SameLine();
      if (Button("Cancel", {90, GetFrameHeight()}))
        CloseCurrentPopup();
      if (Widgets::EscapePressed())
        CloseCurrentPopup();
      EndPopup();
    }
    SetNextWindowSize(
        {std::min(1440.F, screen.x * .94F), std::min(820.F, screen.y * .9F)},
        ImGuiCond_Appearing);
    SetNextWindowSizeConstraints(
        {std::min(800.F, screen.x * .94F), std::min(600.F, screen.y * .94F)},
        screen);
    const bool editWasOpen = IsPopupOpen("Edit item");
    bool editShown = true;
    if (BeginPopupModal("Edit item", &editShown, ImGuiWindowFlags_NoCollapse)) {
      Widgets::WorkspaceFont();
      if (!IsAnyItemActive() &&
          state.value("revision", std::uint64_t{}) != editRevision)
        for (const auto &entry : Array(state, "items"))
          if (entry["index"] == selectedItem)
            SelectEdit(entry["item"], selectedItem, false, state);
      BeginChild("EditFormViewport", {0, -(GetFrameHeightWithSpacing() +
                                           GetStyle().ItemSpacing.y * 2)});
      if (editing)
        ItemForm(state);
      EndChild();
      Separator();
      MessageFooter(state, 90.F);
      SameLine();
      if (Widgets::PrimaryButton("Done", {90, 0})) {
        editing = false;
        CloseCurrentPopup();
      }
      if (Widgets::EscapePressed())
        CloseCurrentPopup();
      EndPopup();
    }
    if (editWasOpen && (!editShown || !IsPopupOpen("Edit item")))
      editing = false;
    SetNextWindowSize(
        {std::min(1500.F, screen.x * .94F), std::min(900.F, screen.y * .92F)},
        ImGuiCond_Appearing);
    SetNextWindowSizeConstraints(
        {std::min(1200.F, screen.x * .94F), std::min(650.F, screen.y * .94F)},
        screen);
    const bool addWasOpen = IsPopupOpen("Add item");
    bool addShown = true;
    if (BeginPopupModal("Add item", &addShown, ImGuiWindowFlags_NoCollapse)) {
      Widgets::WorkspaceFont();
      BeginChild("RecordSearchPanel", {GetContentRegionAvail().x * .30F, 0},
                 ImGuiChildFlags_AlwaysUseWindowPadding);
      Widgets::Section("Search records");
      const float filterWidth = std::max(
          1.F,
          (GetContentRegionAvail().x - 2.F * GetStyle().ItemSpacing.x) / 3.F);
      bool changed = Widgets::Input("##SearchRecords", search, filterWidth);
      SameLine();
      SetNextItemWidth(filterWidth);
      if (Widgets::Combo("##Plugin", pluginFilter.empty()
                                         ? "All plugins"
                                         : pluginFilter.c_str())) {
        static std::string query;
        Widgets::Input("Filter plugins", query, 220);
        if (Selectable("All plugins", pluginFilter.empty())) {
          pluginFilter.clear();
          changed = true;
        }
        for (const auto &entry : Array(state, "plugins")) {
          const auto name = entry.get<std::string>();
          if (PackDocument::Lower(name).find(PackDocument::Lower(query)) ==
              std::string::npos)
            continue;
          if (Selectable(name.c_str(), name == pluginFilter)) {
            pluginFilter = name;
            changed = true;
          }
        }
        EndCombo();
      }
      const char *preview =
          recordType.empty() ? "All types" : recordType.c_str();
      SameLine();
      SetNextItemWidth(filterWidth);
      if (Widgets::Combo("##RecordType", preview)) {
        for (const auto *type : {"", "Static", "Movable Static", "Tree",
                                 "Furniture", "Door", "Container", "Activator"})
          if (Selectable(*type ? type : "All types", recordType == type)) {
            recordType = type;
            changed = true;
          }
        EndCombo();
      }
      if (refreshSearch && !busy &&
          state.value("revision", std::uint64_t{}) > addRevision) {
        changed = true;
        refreshSearch = false;
      }
      if (changed)
        Send({{"action", "search"},
              {"query", search},
              {"plugin", pluginFilter},
              {"recordType", recordType},
              {"requestId", ++searchRequest}});
      if (auto found = results.load();
          found &&
          found->value("requestId", std::uint64_t{}) == searchRequest) {
        if (BeginTable("Records", 3,
                       ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                       {0, 0})) {
          TableSetupColumn("Record", ImGuiTableColumnFlags_WidthStretch, 1.8F);
          TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 1.2F);
          TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.F);
          TableSetupScrollFreeze(0, 1);
          TableHeadersRow();
          const auto &records = Array(*found, "results");
          ImGuiListClipper resultClipper;
          resultClipper.Begin(static_cast<int>(records.size()));
          while (resultClipper.Step())
            for (int row = resultClipper.DisplayStart;
                 row < resultClipper.DisplayEnd; ++row) {
              const auto &entry = records[row];
              PushID(static_cast<int>(entry.value("id", 0U)));
              TableNextRow(0, GetFrameHeight() + GetStyle().CellPadding.y * 2);
              BeginDisabled(entry.value("inPack", false));
              const bool selectedRecord =
                  expectedDraft ==
                  entry.value("plugin", "") + ":" + entry.value("local", "");
              bool rowHovered = false;
              for (const auto *field : {"editorID", "plugin", "recordType"}) {
                TableNextColumn();
                PushID(field);
                auto label = entry.value(field, "");
                if (label.empty() && std::string_view(field) == "editorID")
                  label = entry.value("name", entry.value("local", "Record"));
                if (CellSelectable((label + "##record").c_str(),
                                   std::string_view(field) == "editorID"
                                       ? ItemPicture(entry, true)
                                       : nullptr,
                                   std::string_view(field) == "editorID")) {
                  expectedDraft = entry.value("plugin", "") + ":" +
                                  entry.value("local", "");
                  Send({{"action", "pick"}, {"id", entry["id"]}});
                }
                rowHovered |= IsItemHovered();
                HoverIcon(entry, true);
                PopID();
              }
              if (rowHovered || selectedRecord)
                for (int column = 0; column < 3; ++column)
                  TableSetBgColor(
                      ImGuiTableBgTarget_CellBg,
                      GetColorU32(ImVec4{.29F, .38F, .41F,
                                         selectedRecord ? .4F : .27F}),
                      column);
              EndDisabled();
              PopID();
            }
          EndTable();
        }
      }
      EndChild();
      SameLine();
      BeginChild("RecordEditPanel", {0, 0});
      BeginChild("DraftFormViewport", {0, -(GetFrameHeightWithSpacing() +
                                            GetStyle().ItemSpacing.y * 2)});
      if (!expectedDraft.empty() && !state.value("draft", Json()).is_null() &&
          state["draft"].value("plugin", "") + ":" +
                  state["draft"].value("formId", "") ==
              expectedDraft) {
        const auto revision = state.value("revision", std::uint64_t{});
        if (!editing || (!IsAnyItemActive() && revision != editRevision))
          SelectEdit(state["draft"], 0, true, state);
        ItemForm(state);
      } else
        TextDisabled("Select a record on the left to edit it.");
      EndChild();
      Separator();
      MessageFooter(state, 150.F);
      SameLine();
      if (editing && !expectedDraft.empty() &&
          !state.value("draft", Json()).is_null() &&
          state["draft"].value("plugin", "") + ":" +
                  state["draft"].value("formId", "") ==
              expectedDraft) {
        BeginDisabled((!state.value("draftReady", false) &&
                       !state.value("draftFailed", false)) ||
                      busy || state.value("shellAnalyzing", false));
        if (Widgets::PrimaryButton("Add to pack", {150, 0})) {
          Send({{"action", "add"}});
          refreshSearch = true;
          addRevision = state.value("revision", std::uint64_t{});
          expectedDraft.clear();
          editing = false;
        }
        EndDisabled();
      }
      EndChild();
      if (Widgets::EscapePressed())
        CloseCurrentPopup();
      EndPopup();
    }
    if (addWasOpen && (!addShown || !IsPopupOpen("Add item"))) {
      Send({{"action", "clearDraft"}});
      expectedDraft.clear();
      editing = false;
    }
    PopStyleVar(2);
  }
  if (Widgets::EscapePressed())
    shown = false;
  End();
  if (!shown)
    Send({{"action", "close"}});
}
} // namespace
void Initialize(void (*handler)(const Json &), void (*update)()) {
  pump = update;
  submit = handler;
  if (!window)
    window = SkyBuild::UI::Native::AddWindow(Draw);
}
void SetOpen(bool open) {
  if (open && (!window || !window->IsOpen.load()))
    requestedResourceIcons.clear();
  if (window)
    window->IsOpen.store(open);
}
void OpenPickedItem() { openPickedItem.store(true); }
bool IsOpen() { return window && window->IsOpen.load(); }
void Publish(Json state) {
  if (state.contains("exportAssets"))
    assets.store(
        std::make_shared<const Json>(std::move(state["exportAssets"])));
  if (state.contains("assetSelectionUpdate"))
    if (auto previous = assets.load()) {
      auto next = std::make_shared<Json>(*previous);
      const auto &update = state["assetSelectionUpdate"];
      for (auto &entry : *next) {
        const bool match = update.contains("index")
                               ? entry["index"] == update["index"]
                               : PackDocument::Lower(entry.value("path", ""))
                                     .starts_with(PackDocument::Lower(
                                         update.value("prefix", "")));
        if (match && entry.value("available", false) &&
            !entry.value("gameFile", false))
          entry["include"] = update["value"];
      }
      assets.store(std::move(next));
    }
  snapshot.store(std::make_shared<const Json>(std::move(state)));
}
void MaterialResults(Json value) {
  materialResults.store(std::make_shared<const Json>(std::move(value)));
}
void SearchResults(Json value) {
  results.store(std::make_shared<const Json>(std::move(value)));
}
} // namespace SkyBuild::UI::PackEditor::View
