#include "building/PackCategories.h"
#pragma once
#include "ui/imgui/Widgets.h"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
namespace SkyBuild::UI::SearchView {
using Json = nlohmann::json;
inline std::string PackLabel(const Json &item) {
  auto label = item.value("displayPack", "");
  if (label.find_first_not_of(" \t\r\n") == std::string::npos)
    label = item.value("pack", "");
  return label.find_first_not_of(" \t\r\n") == std::string::npos
             ? "Unnamed pack"
             : label;
}
inline std::string CategoryLabel(const Json &item) {
  const auto label = item.value("category", "");
  return label.find_first_not_of(" \t\r\n") == std::string::npos ? "Other"
                                                                 : label;
}

inline std::string FitCardLabel(std::string text, float width) {
  if (ImGui::CalcTextSize(text.c_str()).x <= width)
    return text;
  while (!text.empty() &&
         ImGui::CalcTextSize((text + "...").c_str()).x > width) {
    auto end = text.size() - 1;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
      --end;
    text.resize(end);
  }
  return text + "...";
}
inline void Draw(const std::shared_ptr<const Json> &items,
                 const std::shared_ptr<const Json> &style,
                 const std::function<void(const Json &)> &choose,
                 const std::function<void()> &close) {
  if (!items)
    return;
  Widgets::BeginFrame();
  Widgets::Theme theme(style ? style->value("opacity", 1.F) : 1.F);
  Widgets::WorkspaceTheme workspace;
  using namespace ImGui;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_Appearing,
                   {.5F, .5F});
  SetNextWindowSize(
      {std::min(1280.F, screen.x * .9F), std::min(820.F, screen.y * .9F)},
      ImGuiCond_Appearing);
  SetNextWindowSizeConstraints(
      {std::min(960.F, screen.x * .9F), std::min(560.F, screen.y * .9F)},
      screen);
  bool shown = true;
  if (Begin("Search##SkyBuildSearch", &shown, ImGuiWindowFlags_NoCollapse)) {
    Widgets::WorkspaceFont();
    static std::string query, last;
    static std::shared_ptr<const Json> previous;
    static std::vector<std::size_t> filtered;
    static std::string selectedPack, selectedCategory;
    std::set<std::string> packs;
    std::vector<std::string> categories;
    std::map<std::string, int> packCounts, categoryCounts;
    for (const auto &item : *items) {
      packs.insert(PackLabel(item));
      ++packCounts[PackLabel(item)];
    }
    if (!selectedPack.empty() && !packs.contains(selectedPack))
      selectedPack.clear();
    bool changed = false;
    const float railWidth = std::min(190.F, GetContentRegionAvail().x * .16F);
    BeginChild("SearchPacks", {railWidth, 0},
               ImGuiChildFlags_AlwaysUseWindowPadding);
    Widgets::SidebarHeading("PACKS");
    if (Widgets::SidebarItem("All##AllPacks", selectedPack.empty(),
                             static_cast<int>(items->size()))) {
      selectedPack.clear();
      selectedCategory.clear();
      changed = true;
    }
    for (const auto &pack : packs) {
      PushID(pack.c_str());
      if (Widgets::SidebarItem(pack.c_str(), selectedPack == pack,
                               packCounts[pack])) {
        selectedPack = pack;
        selectedCategory.clear();
        changed = true;
      }
      PopID();
    }
    EndChild();
    SameLine();
    for (const auto &item : *items)
      if (selectedPack.empty() || PackLabel(item) == selectedPack) {
        const auto category = CategoryLabel(item);
        if (std::find(categories.begin(), categories.end(), category) ==
            categories.end())
          categories.push_back(category);
        ++categoryCounts[CategoryLabel(item)];
      }
    std::stable_partition(
        categories.begin(), categories.end(),
        [](const auto &name) { return Packs::CategoryKey(name) != "other"; });
    if (!selectedCategory.empty() &&
        std::find(categories.begin(), categories.end(), selectedCategory) ==
            categories.end())
      selectedCategory.clear();
    BeginChild("SearchCategories", {railWidth, 0},
               ImGuiChildFlags_AlwaysUseWindowPadding);
    Widgets::SidebarHeading("CATEGORIES");
    if (Widgets::SidebarItem("All##AllCategories", selectedCategory.empty(),
                             selectedPack.empty()
                                 ? static_cast<int>(items->size())
                                 : packCounts[selectedPack])) {
      selectedCategory.clear();
      changed = true;
    }
    for (const auto &category : categories) {
      PushID(category.c_str());
      if (Widgets::SidebarItem(category.c_str(), selectedCategory == category,
                               categoryCounts[category])) {
        selectedCategory = category;
        changed = true;
      }
      PopID();
    }
    EndChild();
    SameLine();
    BeginChild("SearchResults", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
    Widgets::FieldLabel("Search placeables");
    if (IsWindowAppearing())
      SetKeyboardFocusHere();
    changed |= Widgets::Input("##SearchPlaceables", query, -1);
    if (changed || previous != items) {
      previous = items;
      last = query;
      std::ranges::transform(last, last.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      filtered.clear();
      for (std::size_t i = 0; i < items->size(); ++i) {
        auto text = (*items)[i].value("label", "") + " " +
                    PackLabel((*items)[i]) + " " + CategoryLabel((*items)[i]);
        std::ranges::transform(text, text.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
        if ((selectedPack.empty() || PackLabel((*items)[i]) == selectedPack) &&
            (selectedCategory.empty() ||
             CategoryLabel((*items)[i]) == selectedCategory) &&
            text.find(last) != std::string::npos)
          filtered.push_back(i);
      }
    }
    TextDisabled("%zu of %zu placeables", filtered.size(), items->size());
    Separator();
    BeginChild("SearchCards", {0, 0});
    Widgets::WorkspaceFont();
    if (filtered.empty()) {
      TextDisabled("No matching placeables");
    }
    if (changed || IsWindowAppearing())
      SetScrollY(0);
    constexpr float cellWidth = 160.F, iconSize = 112.F;
    constexpr float columnPitch = 172.F;
    const float lineHeight = GetTextLineHeight() + 2.F;
    const float cellHeight = std::max(172.F, 124.F + 2.F * lineHeight + 5.F);
    const float rowHeight = cellHeight + 12.F;
    const auto gridOrigin = GetCursorScreenPos();
    const int columns = std::max(
        1, static_cast<int>((GetContentRegionAvail().x + 12.F) / columnPitch));
    const int rows =
        static_cast<int>((filtered.size() + columns - 1) / columns);
    const int first =
        std::max(0, static_cast<int>(GetScrollY() / rowHeight) - 1);
    const int lastRow = std::min(
        rows, first + static_cast<int>(GetWindowHeight() / rowHeight) + 3);
    for (int row = first; row < lastRow; ++row) {
      for (int col = 0; col < columns; ++col) {
        const auto at = static_cast<std::size_t>(row * columns + col);
        if (at >= filtered.size())
          break;
        const auto &item = (*items)[filtered[at]];
        const ImVec2 pos{gridOrigin.x + col * columnPitch,
                         gridOrigin.y + row * rowHeight};
        SetCursorScreenPos(pos);
        PushID(static_cast<int>(at));
        ImDrawListManager::AddRectFilled(
            GetWindowDrawList(), pos, {pos.x + cellWidth, pos.y + cellHeight},
            GetColorU32(ImVec4{.075F, .09F, .095F, 1.F}), 5.F, 0);
        SetCursorScreenPos({pos.x + (cellWidth - iconSize) * .5F, pos.y + 6.F});
        Widgets::Picture(Widgets::Root() / item.value("icon", ""), iconSize,
                         1.F,
                         style ? style->value("iconBrightness", 1.F) : 1.F);
        SetCursorScreenPos(pos);
        if (InvisibleButton("Pick", {cellWidth, cellHeight})) {
          choose(item);
        }
        if (IsItemHovered()) {
          ImDrawListManager::AddRect(GetWindowDrawList(), pos,
                                     {pos.x + cellWidth, pos.y + cellHeight},
                                     0x99FFFFFF, 4, 0, 1);
          SetTooltip("%s\n%s", item.value("label", "").c_str(),
                     PackLabel(item).c_str());
        }
        const auto label = item.value("label", "");
        std::array<std::string, 2> lines{label, ""};
        constexpr float textWidth = cellWidth - 16.F;
        if (CalcTextSize(label.c_str()).x > textWidth) {
          auto split = label.rfind(' ');
          while (split != std::string::npos &&
                 CalcTextSize(label.substr(0, split).c_str()).x > textWidth)
            split = split ? label.rfind(' ', split - 1) : std::string::npos;
          if (split != std::string::npos)
            lines = {label.substr(0, split), label.substr(split + 1)};
        }
        PushClipRect({pos.x + 8.F, pos.y + 124.F},
                     {pos.x + cellWidth - 8.F, pos.y + cellHeight - 5.F}, true);
        for (int line = 0; line < 2; ++line)
          if (!lines[line].empty()) {
            const auto text = FitCardLabel(lines[line], textWidth);
            const auto extent = CalcTextSize(text.c_str());
            ImDrawListManager::AddText(GetWindowDrawList(),
                                       {pos.x + (cellWidth - extent.x) * .5F,
                                        pos.y + 124.F + line * lineHeight},
                                       GetColorU32(ImGuiCol_Text),
                                       text.c_str());
          }
        PopClipRect();

        PopID();
      }
    }
    SetCursorScreenPos({gridOrigin.x, gridOrigin.y + rows * rowHeight});
    Dummy({0, 0});
    EndChild();
    EndChild();
  }
  if (Widgets::EscapePressed())
    shown = false;
  End();
  if (!shown)
    close();
}
} // namespace SkyBuild::UI::SearchView
