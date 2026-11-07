#include "ui/pack-editor/PackDocument.h"
#include "building/PackCategories.h"
#include "activators/Connections.h"
#include "ui/imgui/UI.h"
#include "building/BuildingHistory.h"
#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "building/ModifyMode.h"
#include "building/PickerMode.h"
#include "building/PlacementPreview.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "map-markers/MapMarkers.h"
#include "terrain/TerrainTool.h"
#include "ui/fine-adjustments/FineAdjustments.h"
#include "ui/imgui/StripAnimation.h"
#include "ui/imgui/Widgets.h"
#include "ui/pack-editor/PackEditor.h"
#include "ui/search/SearchMenu.h"
#include "ui/settings/Settings.h"
#include "ui/thumbnails/ThumbnailGenerator.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include <numbers>
#include <span>
namespace SkyBuild::UI {
namespace {
using Tool = BuildingMode::InteractionMode;
struct Card {
  RE::FormID id{};
  std::string name, pack, displayPack, category, icon;
};
struct State {
  bool visible{}, building{}, moving{}, snapping{true}, wheel{}, terrain{},
      mainWheel{}, quickWheel{true};
  Terrain::Tool terrainTool{Terrain::Tool::Sculpt};
  std::shared_ptr<const std::vector<BuildCosts::Requirement>> requirements;
  bool canUndo{}, canRedo{}, canPlace{}, canItems{}, canCategories{},
      canPacks{}, canBrush{};
  std::string menuKey{"KeyB"};
  std::string counterclockwiseKey;
  std::string secondaryKey{"MouseRight"};
  std::array<std::string, 2> modifyKeys{"KeyC", "KeyE"};
  bool canCopy{}, canProperties{}, canRemove{};
  bool connecting{}, connectionTarget{};
  std::array<std::string, 2> brushKeys{"MouseWheelUp", "MouseWheelDown"};
  float brushRadius{96};
  Tool tool{Tool::kIdle};
  RE::FormID selected{};
  std::string pack;
  std::array<std::string, 11> keys;
  std::array<std::string, 2> terrainKeys{"KeyZ", "KeyX"};
  float opacity{1}, iconOpacity{.5F}, brightness{1}, wheelX{}, wheelY{};
  int sector{-1};
};
std::mutex stateMutex;
State state;
std::atomic_bool visible{};
std::atomic_bool changingTool{};
bool requestedVisible{};
std::shared_ptr<const std::vector<Card>> catalog =
    std::make_shared<const std::vector<Card>>();
std::unique_ptr<SkyBuild::UI::Native::Model::HudElement> hud;
constexpr std::array<const char *, 4> labels{"BUILD", "MODIFY", "IDLE",
                                             "SEARCH"};
constexpr std::array<const char *, 4> icons{"build", "break", "cancel",
                                            "search"};
constexpr std::array<const char *, 5> terrainLabels{"SCULPT", "LEVEL", "SMOOTH",
                                                    "IDLE", "GRASS"};
constexpr std::array<const char *, 5> terrainIcons{"raise", "flatten", "smooth",
                                                   "cancel", "grass"};
// Clockwise from the top: Settings stays at the bottom; the two left
// slices place Pack Editor immediately below Update Navmesh.
constexpr std::array<const char *, 6> mainLabels{
    "CONSTRUCTION", "TERRAIN",     "MAP MARKERS",
    "SETTINGS",     "PACK EDITOR", "UPDATE NAVMESH"};
constexpr std::array<const char *, 6> mainIcons{
    "build", "landscape", "mapmarker", "settings", "pack-editor", "navmesh"};
const char *TerrainName(Terrain::Tool tool) {
  switch (tool) {
  case Terrain::Tool::Sculpt:
    return "Sculpt";
  case Terrain::Tool::Level:
    return "Level";
  case Terrain::Tool::Smooth:
    return "Smooth";
  case Terrain::Tool::Grass:
    return "Grass";
  default:
    return "Idle";
  }
}
const char *Name(Tool tool) {
  switch (tool) {
  case Tool::kBuild:
    return "Build";
  case Tool::kModify:
    return "Modify";
  case Tool::kPicker:
    return "Picker";
  default:
    return "Idle";
  }
}
void Wheel(const State &s, ImGui::ImVec2 screen) {
  using namespace ImGui;
  using Items = std::span<const char *const>;
  const auto wheelLabels = s.mainWheel ? Items(mainLabels)
                           : s.terrain ? Items(terrainLabels)
                                       : Items(labels);
  const auto wheelIcons = s.mainWheel ? Items(mainIcons)
                          : s.terrain ? Items(terrainIcons)
                                      : Items(icons);
  const int count = static_cast<int>(wheelLabels.size());
  Widgets::WorkspaceFont(16);
  const float step = 2 * std::numbers::pi_v<float> / count;
  const ImVec2 center{screen.x * .5F, screen.y * .5F};
  auto *draw = GetForegroundDrawList();
  int sector = s.sector;
  if (!s.quickWheel) {
    const auto mouse = GetIO().MousePos;
    const float x = mouse.x - center.x, y = mouse.y - center.y;
    const float distance = std::hypot(x, y);
    const float angle =
        std::fmod(std::atan2(y, x) + 2.5F * std::numbers::pi_v<float>,
                  2.F * std::numbers::pi_v<float>);
    sector = distance >= 66 && distance <= 235
                 ? int(std::round(angle / step)) % count
                 : -1;
    {
      std::scoped_lock lock(stateMutex);
      if (state.wheel && state.mainWheel == s.mainWheel)
        state.sector = sector;
    }
    if (IsKeyPressed(ImGuiKey_Escape))
      SKSE::GetTaskInterface()->AddTask([] { CancelToolWheel(); });
  }
  const auto point = [&](float radius, float angle) {
    return ImVec2{center.x + std::cos(angle) * radius,
                  center.y + std::sin(angle) * radius};
  };
  static std::array<float, 8> highlight{};
  const float blend = 1.F - std::exp(-18.F * GetIO().DeltaTime);
  for (int i = 0; i < count; ++i) {
    highlight[i] += ((sector == i ? 1.F : 0.F) - highlight[i]) * blend;
    const float angle = i * step - std::numbers::pi_v<float> / 2.F;
    // Area centroid of the annular slice, on its angular bisector.
    const float halfAngle = step * .5F - .0349066F;
    const float radius = 2.F * std::sin(halfAngle) / (3.F * halfAngle) *
                         (235.F * 235.F * 235.F - 66.F * 66.F * 66.F) /
                         (235.F * 235.F - 66.F * 66.F);
    const auto pos = point(radius, angle);
    const bool active = sector == i;
    const float begin = angle - step * .5F + .0349066F,
                end = angle + step * .5F - .0349066F;
    const auto shade = static_cast<std::uint32_t>(10 + 45 * highlight[i]);
    const auto fill = 0xDC000000U | shade | (shade << 8) | (shade << 16);
    const auto border = active ? 0xCCFFFFFFU : 0x33FFFFFFU;
    // One concave polygon has only perimeter antialiasing; separate quads
    // antialias every shared radial edge and produce visible seams.
    std::array<ImVec2, 50> outline;
    for (int n = 0; n <= 24; ++n) {
      outline[n] = point(235, begin + (end - begin) * n / 24.F);
      outline[25 + n] = point(66, end - (end - begin) * n / 24.F);
    }
    ImDrawListManager::AddConcavePolyFilled(
        draw, outline.data(), static_cast<int>(outline.size()), fill);
    ImDrawListManager::PathArcTo(draw, center, 235, begin, end, 24);
    ImDrawListManager::PathArcTo(draw, center, 66, end, begin, 24);
    ImDrawListManager::PathStroke(draw, border, ImDrawFlags_Closed, 1.F);
    const auto extent = CalcTextSize(wheelLabels[i]);
    // Scaling artwork never changes the fixed icon slot or label position.
    constexpr float iconSlot = 50.F, gap = 8.F;
    const float groupTop = pos.y - (iconSlot + gap + extent.y) * .5F;
    const float iconY = groupTop + iconSlot * .5F;
    const float labelY = groupTop + iconSlot + gap;
    const std::string_view icon = wheelIcons[i];
    const float size = icon == "navmesh"     ? 62.5F
                       : icon == "landscape" ? 60.F
                                             : 50.F;
    ImVec2 dimensions, visibleCenter;
    if (auto texture =
            Widgets::Texture(Widgets::Root() / "assets/icons" /
                                 (std::string(wheelIcons[i]) + ".png"),
                             1.F, &dimensions, &visibleCenter, {size, size})) {
      const auto fb = GetIO().DisplayFramebufferScale;
      const ImVec2 imageSize{dimensions.x / fb.x, dimensions.y / fb.y};
      const ImVec2 start{
          std::round((pos.x - imageSize.x * visibleCenter.x) * fb.x) / fb.x,
          std::round((iconY - imageSize.y * visibleCenter.y) * fb.y) / fb.y};
      ImDrawListManager::AddImage(
          draw, texture, start, {start.x + imageSize.x, start.y + imageSize.y},
          {0, 0}, {1, 1}, 0xFFFFFFFF);
    }
    ImDrawListManager::AddText(draw, {pos.x - extent.x * .5F, labelY},
                               active ? 0xFFFFFFFF : 0xB3FFFFFF,
                               wheelLabels[i]);
    const int current = s.terrain
                            ? (s.terrainTool == Terrain::Tool::Sculpt   ? 0
                               : s.terrainTool == Terrain::Tool::Level  ? 1
                               : s.terrainTool == Terrain::Tool::Smooth ? 2
                               : s.terrainTool == Terrain::Tool::Grass  ? 4
                                                                        : 3)
                            : (s.tool == Tool::kBuild    ? 0
                               : s.tool == Tool::kModify ? 1
                                                         : 2);
    if (!s.mainWheel && i == current && !active)
      ImDrawListManager::AddLine(
          draw, {pos.x - extent.x * .5F, labelY + 2 + extent.y},
          {pos.x + extent.x * .5F, labelY + 2 + extent.y}, 0x99FFFFFF, 1.F);
  }
  const auto *label =
      sector < 0 ? (s.mainWheel ? "MAIN MENU" : "TOOLS") : wheelLabels[sector];
  const auto extent = CalcTextSize(label);
  ImDrawListManager::AddText(
      draw, {center.x - extent.x * .5F, center.y - extent.y * .5F}, 0xFFFFFFFF,
      label);
  const char *hint = s.quickWheel ? "Move to select - release to choose"
                                  : "Click a choice - Esc to close";
  const auto hintSize = CalcTextSize(hint);
  ImDrawListManager::AddText(
      draw, {center.x - hintSize.x * .5F, center.y + 256}, 0xFFD3D8D9, hint);
}
void __stdcall Draw() {
  if (!visible.load() || changingTool.load() || FineAdjustments::IsOpen() ||
      SearchMenu::IsOpen() || PackEditor::IsOpen() ||
      Navmesh::GenerationPresentationActive())
    return;
  State s;
  std::shared_ptr<const std::vector<Card>> items;
  {
    std::scoped_lock lock(stateMutex);
    s = state;
    items = catalog;
  }
  if (!s.visible)
    return;
  Widgets::BeginFrame();
  Widgets::Theme theme(s.opacity);
  const auto screen = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(screen);
  ImGui::Begin("SkyBuildHUDLayout", nullptr,
               ImGui::ImGuiWindowFlags_NoDecoration |
                   ImGui::ImGuiWindowFlags_NoBackground |
                   ImGui::ImGuiWindowFlags_NoInputs |
                   ImGui::ImGuiWindowFlags_NoSavedSettings);
  struct EndLayout {
    ~EndLayout() { ImGui::End(); }
  } endLayout;
  Widgets::WorkspaceFont();
  if (s.wheel) {
    Wheel(s, screen);
    return;
  }
  using namespace ImGui;
  if ((s.terrain && s.terrainTool != Terrain::Tool::Idle) ||
      (!s.terrain && (s.tool == Tool::kBuild || s.tool == Tool::kModify)))
    ImDrawListManager::AddCircleFilled(GetForegroundDrawList(),
                                       {screen.x * .5F, screen.y * .5F}, 2.2F,
                                       0xFFF2F4F2, 12);
  const float margin = 24.F;
  const float sideWidth = std::clamp(screen.x * .12F, 260.F, 300.F);
  const bool adjusting = s.tool == Tool::kBuild || s.moving;
  const bool toolActive =
      s.terrain ? s.terrainTool != Terrain::Tool::Idle : s.tool != Tool::kIdle;
  const int controlRows =
      s.connecting ? 5
      : s.terrain  ? 2 + (toolActive ? 4 : 0) +
                        ((s.terrainTool == Terrain::Tool::Sculpt ||
                          s.terrainTool == Terrain::Tool::Grass)
                             ? 1
                             : 0)
      : s.tool == Tool::kPicker
          ? 4
          : 2 + (toolActive ? 3 : 0) +
                (s.tool == Tool::kModify && !s.moving ? 3 : 0) +
                (adjusting ? 3 + (!s.counterclockwiseKey.empty() ? 1 : 0) : 0) +
                (s.moving ? 1 : 0);
  // Wide keycaps (wheel, space, modifiers) need real horizontal room.
  float controlsWidth = sideWidth;
  const auto fitPair = [&](std::string_view first, std::string_view second,
                           const char *label) {
    controlsWidth = std::max(
        controlsWidth, Widgets::KeyWidth(first) + Widgets::KeyWidth(second) +
                           14.F + CalcTextSize(label).x + 36.F);
  };
  if (toolActive && s.terrain) {
    const auto brushLabel = std::format("Brush size: {:.0f}", s.brushRadius);
    fitPair(s.brushKeys[0], s.brushKeys[1], brushLabel.c_str());
  }
  const float controlsHeight = 83.F + controlRows * 43.F;
  if (Widgets::Panel("SkyBuildControls",
                     {screen.x - margin - controlsWidth,
                      (screen.y - controlsHeight) * .5F},
                     {controlsWidth, controlsHeight})) {
    const auto origin = GetCursorScreenPos();
    const auto title = CalcTextSize("CONTROLS");
    SetCursorScreenPos(
        {screen.x - margin - (controlsWidth + title.x) * .5F, origin.y});
    TextDisabled("CONTROLS");
    SetCursorScreenPos({origin.x, origin.y + 28});
    TextDisabled("%s", s.connecting ? "Connect activator"
                       : s.terrain  ? TerrainName(s.terrainTool)
                                    : Name(s.tool));
    int row = 0;
    const auto control = [&](const std::string &key, const char *label,
                             bool available = true) {
      SetCursorScreenPos({origin.x, origin.y + 64 + row++ * 43.F});
      PushStyleVar(ImGuiStyleVar_Alpha,
                   GetStyle().Alpha * (available ? 1.F : .5F));
      Widgets::Key(key, label);
      PopStyleVar();
    };
    if (s.connecting) {
      control(s.keys[7], "Connect target", s.connectionTarget);
      control(s.secondaryKey, "Cancel connection");
      control(s.modifyKeys[1], "Edit properties");
    } else if (s.terrain) {
      if (s.terrainTool != Terrain::Tool::Idle)
        control(s.keys[7],
                s.terrainTool == Terrain::Tool::Sculpt   ? "Raise (Hold)"
                : s.terrainTool == Terrain::Tool::Grass  ? "Remove grass (Hold)"
                : s.terrainTool == Terrain::Tool::Level  ? "Level (Hold)"
                : s.terrainTool == Terrain::Tool::Smooth ? "Smooth (Hold)"
                                                         : "Smooth (Hold)",
                s.canBrush);
      if (s.terrainTool == Terrain::Tool::Sculpt ||
          s.terrainTool == Terrain::Tool::Grass)
        control(s.secondaryKey,
                s.terrainTool == Terrain::Tool::Grass ? "Restore grass (Hold)"
                                                      : "Lower (Hold)",
                s.canBrush);
      if (s.terrainTool != Terrain::Tool::Idle) {
        const auto label = std::format("Brush size: {:.0f}", s.brushRadius);
        SetCursorScreenPos({origin.x, origin.y + 64 + row++ * 43.F});
        Widgets::Key(s.brushKeys[0], "");
        SameLine(0, 4);
        Widgets::Key(s.brushKeys[1], label.c_str());
      }
      if (toolActive) {
        control(s.terrainKeys[0], "Undo stroke", s.canUndo);
        control(s.terrainKeys[1], "Redo stroke", s.canRedo);
      }
    } else if (s.tool == Tool::kPicker) {
      control(s.keys[7], "Add to pack", s.canPlace);
      control(s.secondaryKey, "Cancel selection");
    } else {
      if (toolActive)
        control(s.keys[7],
                s.tool == Tool::kModify && !s.moving ? "Move" : "Place",
                s.canPlace);
      if (s.tool == Tool::kModify && !s.moving) {
        control(s.secondaryKey, "Remove", s.canRemove);
        control(s.modifyKeys[0], "Copy piece", s.canCopy);
        control(s.modifyKeys[1], "Edit properties", s.canProperties);
      }
      if (adjusting) {
        control(s.keys[6], "Rotate Clockwise");
        if (!s.counterclockwiseKey.empty())
          control(s.counterclockwiseKey, "Rotate Counterclockwise");
        control(s.keys[10], "Fine Adjustments");
        control(s.keys[9], s.snapping ? "Snapping: On" : "Snapping: Off");
      }
      if (s.moving)
        control(s.secondaryKey, "Cancel move");
      if (toolActive) {
        control(s.terrainKeys[0], "Undo", s.canUndo);
        control(s.terrainKeys[1], "Redo", s.canRedo);
      }
    }
    control(s.keys[8], "Tool Menu");
    control(s.menuKey, "Main Menu");
  }
  End();
  if (s.connecting || s.terrain || s.tool != Tool::kBuild || !items ||
      items->empty())
    return;
  static std::shared_ptr<const std::vector<Card>> previous;
  static RE::FormID lastID{};
  static std::string lastPack;
  static const Card *selected{};
  static std::vector<const Card *> cards;
  static std::vector<std::string> packs, categories;
  if (previous != items || lastID != s.selected || lastPack != s.pack) {
    previous = items;
    lastID = s.selected;
    lastPack = s.pack;
    selected = nullptr;
    cards.clear();
    packs.clear();
    categories.clear();
    for (const auto &item : *items)
      if (item.id == s.selected && item.pack == s.pack) {
        selected = &item;
        break;
      }
    if (selected) {
      std::set<std::string> seenPacks, seenCategories;
      for (const auto &item : *items) {
        if (seenPacks.insert(item.displayPack).second)
          packs.push_back(item.displayPack);
        if (item.displayPack != selected->displayPack)
          continue;
        if (seenCategories.insert(item.category).second)
          categories.push_back(item.category);
        if (item.category == selected->category)
          cards.push_back(&item);
      }
    }
  }
  if (!selected)
    return;
  std::stable_partition(
      categories.begin(), categories.end(),
      [](const auto &name) { return Packs::CategoryKey(name) != "other"; });
  const float packHeight =
      100.F + std::min<std::size_t>(packs.size(), 8) * 34.F;
  if (Widgets::Panel("SkyBuildPacks", {margin, (screen.y - packHeight) * .5F},
                     {sideWidth, packHeight})) {
    const auto origin = GetCursorScreenPos();
    const auto title = CalcTextSize("PACKS");
    SetCursorScreenPos({margin + (sideWidth - title.x) * .5F, origin.y});
    TextDisabled("PACKS");
    const float packNextWidth = Widgets::KeyWidth(s.keys[5]);
    const float packPreviousWidth = Widgets::KeyWidth(s.keys[4]);
    const float packKeysX =
        margin + (sideWidth - packPreviousWidth - packNextWidth - 10.F) * .5F;
    SetCursorScreenPos({packKeysX, origin.y + 27});
    Widgets::Key(s.keys[4], "");
    SetCursorScreenPos({packKeysX + packPreviousWidth + 10.F, origin.y + 27});
    Widgets::Key(s.keys[5], "");
    int row = 0;
    for (const auto &pack : packs) {
      if (row == 8)
        break;
      SetCursorScreenPos({origin.x, origin.y + 68 + row++ * 34});
      const auto pos = GetCursorScreenPos();
      if (pack == selected->displayPack) {
        ImDrawListManager::AddRectFilled(GetWindowDrawList(), pos,
                                         {pos.x + sideWidth - 36, pos.y + 30},
                                         0x22FFFFFF, 0, 0);
        ImDrawListManager::AddLine(GetWindowDrawList(), pos,
                                   {pos.x, pos.y + 30}, 0xFFFFFFFF, 2);
      }
      SetCursorScreenPos({pos.x + 10, pos.y + 6});
      PushClipRect(pos, {pos.x + sideWidth - 36, pos.y + 30}, true);
      TextColored(pack == selected->displayPack ? ImVec4{1, 1, 1, 1}
                                                : ImVec4{1, 1, 1, .55F},
                  "%s", pack.c_str());
      PopClipRect();
    }
  }
  End();
  const float width = screen.x - 2 * (sideWidth + margin + 16);
  const float left = (screen.x - width) * .5F;
  const float cardSize = std::clamp(screen.y * .111F, 96.F, 160.F);
  const float carouselHeight = cardSize + 46;
  const bool showCosts = s.tool == Tool::kBuild && !s.moving &&
                         s.requirements && !s.requirements->empty();
  const float carouselY =
      screen.y - 32 - carouselHeight - (s.tool == Tool::kBuild ? 68.F : 0.F);
  // Category tabs are one centered row, with enough height for the key icons.
  const auto activeCategory =
      std::find(categories.begin(), categories.end(), selected->category);
  const int categoryIndex =
      static_cast<int>(activeCategory - categories.begin());
  const int categoryFirst = 0;
  const int categoryLast = static_cast<int>(categories.size());
  const float categoryPreviousWidth = Widgets::KeyWidth(s.keys[2]);
  const float categoryNextWidth = Widgets::KeyWidth(s.keys[3]);
  const float categoryLeftInset = categoryPreviousWidth + 16.F;
  const float categoryRightInset = categoryNextWidth + 16.F;
  float categoryWidth = categoryLeftInset + categoryRightInset;
  for (int i = categoryFirst; i < categoryLast; ++i)
    categoryWidth +=
        CalcTextSize(categories[i].empty() ? "Other" : categories[i].c_str())
            .x +
        24;
  categoryWidth = std::min(categoryWidth, width);
  float selectedStart = 0;
  for (int i = 0; i < categoryIndex; ++i)
    selectedStart +=
        CalcTextSize(categories[i].empty() ? "Other" : categories[i].c_str())
            .x +
        24;
  const float selectedWidth =
      CalcTextSize(selected->category.empty() ? "Other"
                                              : selected->category.c_str())
          .x;
  static StripAnimation categoryAnimation, itemAnimation;
  static std::string animationPack, animationCategory;
  static int lastStripFrame = -2;
  const int stripFrame = GetFrameCount();
  const bool resetStrips = lastStripFrame != stripFrame - 1 ||
                           animationPack != selected->displayPack;
  const bool resetItems =
      resetStrips || animationCategory != selected->category;
  lastStripFrame = stripFrame;
  animationPack = selected->displayPack;
  animationCategory = selected->category;
  const float categoryCenter = categoryAnimation.Update(
      selectedStart + selectedWidth * .5F, GetIO().DeltaTime, resetStrips);
  const float categoryLeft = (screen.x - categoryWidth) * .5F;
  if (categories.size() > 1) {
    if (Widgets::Panel("SkyBuildCategories", {categoryLeft, carouselY - 68},
                       {categoryWidth, 60})) {
      const float y = carouselY - 68 + (60.F - 30.F) * .5F;
      SetCursorScreenPos({categoryLeft + 12, y});
      Widgets::Key(s.keys[2], "");
      PushClipRect({categoryLeft + categoryLeftInset, y},
                   {categoryLeft + categoryWidth - categoryRightInset, y + 34},
                   true);
      float x = categoryLeft + categoryLeftInset +
                (categoryWidth - categoryLeftInset - categoryRightInset) * .5F -
                categoryCenter;
      for (int i = categoryFirst; i < categoryLast; ++i) {
        const auto label =
            categories[i].empty() ? "Other" : categories[i].c_str();
        const auto extent = CalcTextSize(label);
        SetCursorScreenPos({x, y + (30.F - extent.y) * .5F});
        TextColored(i == categoryIndex ? ImVec4{1, 1, 1, 1}
                                       : ImVec4{1, 1, 1, .55F},
                    "%s", label);
        if (i == categoryIndex)
          ImDrawListManager::AddLine(GetWindowDrawList(), {x, y + 29},
                                     {x + extent.x, y + 29}, 0xFFFFFFFF, 2);
        x += extent.x + 24;
      }
      PopClipRect();
      SetCursorScreenPos(
          {categoryLeft + categoryWidth - categoryNextWidth - 8.F, y});
      Widgets::Key(s.keys[3], "");
    }
    End();
  }
  if (Widgets::Panel("SkyBuildCarousel", {left, carouselY},
                     {width, carouselHeight})) {
    const auto at = std::find(cards.begin(), cards.end(), selected);
    const int index = static_cast<int>(at - cards.begin());
    const float animatedIndex = itemAnimation.Update(
        static_cast<float>(index), GetIO().DeltaTime, resetItems);
    const float imageY = carouselY + 10;
    const float previousWidth = Widgets::KeyWidth(s.keys[0]);
    const float nextWidth = Widgets::KeyWidth(s.keys[1]);
    const float clipLeft = left + previousWidth + 16.F;
    const float clipRight = left + width - nextWidth - 16.F;
    PushClipRect({clipLeft, imageY}, {clipRight, carouselY + carouselHeight},
                 true);
    // Center the selected card, even at either end of the catalog.
    const int visibleCards = static_cast<int>(width / (cardSize + 12)) / 2 + 1;
    for (int i = std::max(0, static_cast<int>(std::floor(animatedIndex)) -
                                 visibleCards);
         i < std::min(static_cast<int>(cards.size()),
                      static_cast<int>(std::ceil(animatedIndex)) +
                          visibleCards + 1);
         ++i) {
      const float x = screen.x * .5F - cardSize * .5F +
                      (i - animatedIndex) * (cardSize + 12);
      const auto *card = cards[i];
      SetCursorScreenPos({x, imageY});
      ImDrawListManager::AddRectFilled(GetWindowDrawList(), {x, imageY},
                                       {x + cardSize, imageY + cardSize},
                                       0x40000000, 0, 0);
      Widgets::Picture(Widgets::Root() / card->icon, cardSize,
                       i == index ? 1.F : s.iconOpacity, s.brightness);
      ImDrawListManager::AddRect(
          GetWindowDrawList(), {x, imageY}, {x + cardSize, imageY + cardSize},
          i == index ? 0xFFFFFFFF : 0x80FFFFFF, 0, 0, i == index ? 3.F : 1.F);
      const auto textSize = CalcTextSize(card->name.c_str());
      PushClipRect({x, imageY + cardSize + 5},
                   {x + cardSize, carouselY + carouselHeight}, true);
      ImDrawListManager::AddText(
          GetWindowDrawList(),
          {x + (cardSize - textSize.x) * .5F, imageY + cardSize + 7},
          0xFFFFFFFF, card->name.c_str());
      PopClipRect();
    }
    PopClipRect();
    const float keyY = carouselY + (carouselHeight - 30.F) * .5F;
    SetCursorScreenPos({left + 8, keyY});
    Widgets::Key(s.keys[0], "");
    SetCursorScreenPos({left + width - nextWidth - 8.F, keyY});
    Widgets::Key(s.keys[1], "");
  }
  End();
  if (showCosts) {
    constexpr float iconSize = 42.F, gap = 7.F, spacing = 20.F;
    float contentWidth = 0;
    for (const auto &cost : *s.requirements)
      contentWidth +=
          iconSize + gap +
          CalcTextSize(std::to_string(cost.material.amount).c_str()).x +
          spacing;
    contentWidth = std::max(0.F, contentWidth - spacing);
    const float panelWidth =
        std::min(screen.x - 2 * margin, contentWidth + 32.F);
    const ImVec2 origin{(screen.x - panelWidth) * .5F, screen.y - 92.F};
    if (Widgets::Panel("SkyBuildRequiredMaterials", origin,
                       {panelWidth, 60.F})) {
      float x = origin.x + 16.F;
      PushClipRect({origin.x + 8, origin.y},
                   {origin.x + panelWidth - 8, origin.y + 60}, true);
      for (const auto &cost : *s.requirements) {
        const auto path = PackDocument::IconRelative(
            ".resources", cost.material.plugin, cost.material.local);
        SetCursorScreenPos({x, origin.y + (60.F - iconSize) * .5F});
        Widgets::Picture(Widgets::Root() / path, iconSize);
        const auto text = std::to_string(cost.material.amount);
        const auto extent = CalcTextSize(text.c_str());
        ImDrawListManager::AddText(
            GetWindowDrawList(),
            {x + iconSize + gap, origin.y + (60.F - extent.y) * .5F},
            cost.Affordable() ? 0xFFFFFFFF : 0xFF5555FF, text.c_str());
        x += iconSize + gap + extent.x + spacing;
      }
      PopClipRect();
    }
    End();
  }
}
class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::MenuOpenCloseEvent *,
               RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override {
    RefreshPresentation();
    return RE::BSEventNotifyControl::kContinue;
  }
} menuSink;
} // namespace
bool Initialize() {
  if (hud)
    return true;

  PackEditor::Initialize();
  hud = std::make_unique<SkyBuild::UI::Native::Model::HudElement>(Draw);
  if (auto *ui = RE::UI::GetSingleton())
    ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);
  RefreshControlBindings();
  return true;
}
void LoadPlaceableCarousel() {
  auto items = std::make_shared<std::vector<Card>>();
  for (const auto &item : PlacementPreview::GetPlaceables())
    items->push_back(
        {item.formID, std::string(item.name.empty() ? "Placeable" : item.name),
         std::string(item.packName),
         item.extendBasePack ? "SkyBuild" : std::string(item.packName),
         std::string(item.category),
         ThumbnailGenerator::GetRelativePath(item.formID, item.packName)
             .value_or("")});
  {
    std::scoped_lock lock(stateMutex);
    catalog = std::move(items);
  }
  RefreshPresentation();
}
nlohmann::json GetPlaceableCatalog() {
  nlohmann::json result = nlohmann::json::array();
  std::scoped_lock lock(stateMutex);
  for (const auto &item : *catalog)
    result.push_back({{"formId", std::format("{:08X}", item.id)},
                      {"label", item.name},
                      {"pack", item.pack},
                      {"displayPack", item.displayPack},
                      {"category", item.category},
                      {"icon", item.icon}});
  return result;
}
nlohmann::json GetPlaceableAppearance() {
  std::scoped_lock lock(stateMutex);
  return nlohmann::json{{"opacity", state.opacity},
                        {"iconOpacity", state.iconOpacity},
                        {"iconBrightness", state.brightness}};
}
bool IsGameMenuBlocking(bool ignoreFine) {
  if (!ignoreFine && FineAdjustments::IsOpen())
    return true;
  if (Settings::IsMenuOpen() || PackEditor::IsOpen() || SearchMenu::IsOpen() ||
      MapMarkers::IsOpen())
    return true;
  auto *ui = RE::UI::GetSingleton();
  if (!ui)
    return false;
  if (ui->GameIsPaused() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen() ||
      ui->IsApplicationMenuOpen())
    return true;
  for (const auto name : {"Dialogue Menu", "Console", "Crafting Menu",
                          "Loading Menu", "Main Menu"})
    if (ui->IsMenuOpen(name))
      return true;
  for (const auto &menu : ui->menuStack)
    if (menu && (menu->UsesCursor() || menu->PausesGame() || menu->Modal() ||
                 menu->InventoryItemMenu() || menu->ApplicationMenu()))
      return true;
  return false;
}
void RefreshPresentation() {
  const bool blocked = IsGameMenuBlocking();
  const auto &preview = PlacementPreview::GetState();
  std::scoped_lock lock(stateMutex);
  const bool previousTerrain = state.terrain;
  state.terrain = Terrain::Ready();
  state.terrainTool = Terrain::GetTool();
  state.connecting = Activators::IsConnecting();
  state.connectionTarget = Activators::HasTarget();
  state.canUndo =
      state.terrain ? Terrain::CanUndo() : BuildingHistory::CanUndo();
  state.brushRadius = Terrain::BrushRadius();
  state.canRedo =
      state.terrain ? Terrain::CanRedo() : BuildingHistory::CanRedo();
  const bool toolVisible = requestedVisible || state.terrain;
  state.visible = (toolVisible || (state.wheel && state.mainWheel)) && !blocked;
  state.tool = BuildingMode::GetTool();
  state.moving = PlacementPreview::IsMovingObject();
  state.building = BuildingMode::IsActive();
  state.snapping = preview.snappingEnabled;
  state.pack = PlacementPreview::GetActivePack();
  state.selected = preview.baseObject ? preview.baseObject->GetFormID() : 0;
  state.requirements = PlacementPreview::GetRequirements();
  const auto player = RE::PlayerCharacter::GetSingleton();
  state.canBrush = player && player->GetWorldspace();
  state.canPlace = state.tool == Tool::kPicker ? PickerMode::HasTarget()
                   : state.tool == Tool::kModify && !state.moving
                       ? ModifyMode::HasTarget()
                       : (preview.selectionPending ? state.canPlace
                                                   : (preview.validPlacement &&
                                                      preview.enoughMaterials));
  state.canRemove =
      state.tool == Tool::kModify && !state.moving && ModifyMode::HasTarget();
  state.canCopy =
      state.tool == Tool::kModify && !state.moving && ModifyMode::CanCopy();
  state.canProperties = state.tool == Tool::kModify && !state.moving &&
                        ModifyMode::CanEditProperties();
  state.canItems = state.canCategories = state.canPacks = false;
  const auto selected = std::ranges::find_if(*catalog, [&](const auto &card) {
    return card.id == state.selected && card.pack == state.pack;
  });
  if (selected != catalog->end())
    for (const auto &card : *catalog) {
      if (card.displayPack != selected->displayPack)
        state.canPacks = true;
      else if (card.category != selected->category)
        state.canCategories = true;
      else if (card.id != selected->id || card.pack != selected->pack)
        state.canItems = true;
    }

  if (blocked || !BuildingMode::IsHammerReady() ||
      (!state.mainWheel && (!toolVisible || previousTerrain != state.terrain)))
    state.wheel = false;
  visible.store(state.visible);
}
void SetVisible(bool value) {
  requestedVisible = value;
  RefreshPresentation();
}
void RefreshControlBindings() {
  using namespace ControlBindings;
  const std::array<const KeyboardBinding *, 11> keys{
      &scrollLeft,   &scrollRight,    &categoryLeft,   &categoryRight,
      &packPrevious, &packNext,       &rotate,         &placeAction,
      &switchMode,   &toggleSnapping, &fineAdjustments};
  std::scoped_lock lock(stateMutex);
  for (std::size_t i = 0; i < keys.size(); ++i)
    state.keys[i] = keys[i]->iconKey;
  state.terrainKeys = {historyUndo.iconKey, historyRedo.iconKey};
  state.menuKey = openMenu.iconKey;
  state.counterclockwiseKey = rotateCounterclockwise.iconKey;
  state.secondaryKey = secondaryAction.iconKey;
  for (std::size_t i = 0; i < modifyControls.size(); ++i)
    state.modifyKeys[i] = modifyControls[i].iconKey;
  state.brushKeys = {brushIncrease.iconKey, brushDecrease.iconKey};
}
void SetOpacity(float value) {
  std::scoped_lock lock(stateMutex);
  state.opacity = std::clamp(value, 0.F, 1.F);
}
void SetSnappingEnabled(bool value) {
  std::scoped_lock lock(stateMutex);
  state.snapping = value;
}
void SetIconAppearance(float alpha, float light) {
  std::scoped_lock lock(stateMutex);
  state.iconOpacity = alpha;
  state.brightness = light;
}
void SetActivePlaceable(RE::FormID id) {
  std::scoped_lock lock(stateMutex);
  state.selected = id;
  state.pack = PlacementPreview::GetActivePack();
  state.requirements = PlacementPreview::GetRequirements();
}
void SetThumbnailStatus(std::string_view text) {
  ThumbnailGenerator::SetProgressStatus(text);
}
void SetThumbnailPreview(RE::FormID, std::string_view path) {
  Widgets::Invalidate(Widgets::Root() / path);
}
bool IsToolWheelOpen() {
  std::scoped_lock lock(stateMutex);
  return state.wheel;
}
bool WheelHasCursor() {
  std::scoped_lock lock(stateMutex);
  return state.wheel && !state.quickWheel;
}
void OpenMainMenu() {
  if (!BuildingMode::IsHammerReady() || IsGameMenuBlocking())
    return;
  std::scoped_lock lock(stateMutex);
  if (state.wheel && state.mainWheel) {
    state.wheel = false;
    return;
  }
  state.wheel = true;
  state.mainWheel = true;
  state.quickWheel = Settings::QuickSelectMainMenu();
  state.wheelX = state.wheelY = 0;
  state.sector = -1;
  state.visible = true;
  visible.store(true);
}
void OpenToolWheel() {
  if ((!BuildingMode::IsHammerReady() && !Terrain::Ready()) ||
      IsGameMenuBlocking())
    return;
  std::scoped_lock lock(stateMutex);
  if (state.wheel && !state.mainWheel && !state.quickWheel) {
    state.wheel = false;
    return;
  }
  state.wheel = true;
  state.mainWheel = false;
  state.quickWheel = Settings::QuickSelectToolMenu();
  state.wheelX = state.wheelY = 0;
  state.sector = -1;
}
void MoveToolWheel(float x, float y) {
  if (!std::isfinite(x) || !std::isfinite(y))
    return;
  std::scoped_lock lock(stateMutex);
  if (!state.wheel)
    return;
  state.wheelX += x * .65F;
  state.wheelY += y * .65F;
  const float distance = std::hypot(state.wheelX, state.wheelY);
  if (distance < (state.sector < 0 ? 16.F : 10.F)) {
    state.sector = -1;
    return;
  }
  if (distance > 90) {
    state.wheelX *= 90 / distance;
    state.wheelY *= 90 / distance;
  }
  const float angle = std::fmod(std::atan2(state.wheelY, state.wheelX) * 180 /
                                        std::numbers::pi_v<float> +
                                    450,
                                360.F);
  const int count = state.mainWheel ? static_cast<int>(mainLabels.size())
                    : state.terrain ? static_cast<int>(terrainLabels.size())
                                    : static_cast<int>(labels.size());
  const float step = 360.F / count;
  const int candidate = static_cast<int>(std::round(angle / step)) % count;
  if (state.sector >= 0 && candidate != state.sector) {
    const float separation =
        std::abs(std::fmod(angle - state.sector * step + 540, 360.F) - 180);
    if (separation < step * .6F)
      return;
  }
  state.sector = candidate;
}
void FinishToolWheel(bool clicked) {
  changingTool.store(true);
  struct Finish {
    ~Finish() { changingTool.store(false); }
  } finish;
  int selected;
  bool terrain, mainMenu;
  {
    std::scoped_lock lock(stateMutex);
    if (!state.wheel)
      return;
    if (!clicked && !state.quickWheel)
      return;
    mainMenu = state.mainWheel;
    selected = state.sector;
    terrain = state.terrain;
    state.wheel = false;
  }
  if (selected < 0)
    return;
  if (!BuildingMode::IsHammerReady())
    return;
  if (mainMenu) {
    if (selected == 0) {
      Terrain::SetEnabled(false);
      BuildingMode::SetEnabled(true);
      BuildingMode::SelectTool(Tool::kBuild);
    } else if (selected == 1) {
      BuildingMode::SetEnabled(false);
      Terrain::SetEnabled(true);
      Terrain::SelectWheelAction(0);
    } else if (selected == 2)
      MapMarkers::Open();
    else if (selected == 3)
      Settings::SetMenuOpen(true);
    else if (selected == 4)
      PackEditor::Open();
    else if (selected == 5)
      SKSE::GetTaskInterface()->AddTask([] { Navmesh::RequestGeneration(); });
    RefreshPresentation();
    return;
  }
  if (terrain) {
    Terrain::SelectWheelAction(selected);
    return;
  }
  if (selected == 3) {
    SearchMenu::Open();
    return;
  }
  constexpr std::array<Tool, 3> tools{Tool::kBuild, Tool::kModify, Tool::kIdle};
  if (selected >= static_cast<int>(tools.size()))
    return;
  BuildingMode::SelectTool(tools[selected]);
}
void CancelToolWheel() {
  std::scoped_lock lock(stateMutex);
  state.wheel = false;
}
} // namespace SkyBuild::UI
