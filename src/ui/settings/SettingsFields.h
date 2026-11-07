#pragma once
#include "ui/imgui/Widgets.h"
#include "ui/settings/SettingsValues.h"
#include <format>

// Rendering-only settings fields, shared by the game and offline UI previews.
namespace SkyBuild::Settings::Fields {
inline constexpr std::array<const char *, 11> actions{
    "Previous item", "Next item",       "Previous category", "Next category",
    "Previous pack", "Next pack",       "Rotate Clockwise",  "Primary action",
    "Tool Menu",     "Toggle snapping", "Fine adjustments"};
inline constexpr std::array<const char *, 14> fineActions{
    "Move left",      "Move right",    "Move forward", "Move backward",
    "Move up",        "Move down",     "Turn left",    "Turn right",
    "Tilt forward",   "Tilt backward", "Roll left",    "Roll right",
    "Decrease scale", "Increase scale"};
inline constexpr int HistoryBindingOffset =
    static_cast<int>(actions.size() + fineActions.size());
inline constexpr int MenuBindingAction = HistoryBindingOffset + 2;
inline constexpr int BrushBindingOffset = MenuBindingAction + 1;
inline constexpr int ModifyBindingOffset = BrushBindingOffset + 2;
inline constexpr int SecondaryBindingAction = ModifyBindingOffset + 2;
inline constexpr int CounterclockwiseBindingAction = SecondaryBindingAction + 1;
template <class LabelFor, class Capture>
inline void Keybinds(const Values &values, LabelFor labelFor, Capture capture) {
  struct Row {
    const char *label;
    std::uint32_t key;
    int action;
  };
  const auto drawBindings = [&](const char *table, const auto &rows) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8, 2});
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {12, 3});
    if (ImGui::BeginTable(
            table, 2,
            ImGui::ImGuiTableFlags_SizingStretchSame |
                ImGui::ImGuiTableFlags_PadOuterX |
                ImGui::ImGuiTableFlags_RowBg |
                ImGui::ImGuiTableFlags_BordersInnerH |
                ImGui::ImGuiTableFlags_NoSavedSettings,
            {std::min(560.F, ImGui::GetContentRegionAvail().x), 0})) {
      ImGui::TableSetupColumn("Control",
                              ImGui::ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Binding",
                              ImGui::ImGuiTableColumnFlags_WidthStretch);
      for (const auto &row : rows) {
        ImGui::TableNextRow(0, UI::Widgets::TableRowHeight);
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.label);
        ImGui::TableSetColumnIndex(1);
        const auto label = std::format("{}###SkyBuildBinding{}",
                                       labelFor(row.key), row.action);
        const bool clearable =
            row.action == CounterclockwiseBindingAction && row.key != 0;
        const float width =
            ImGui::GetContentRegionAvail().x -
            (clearable ? 60.F + ImGui::GetStyle().ItemSpacing.x : 0.F);
        if (ImGui::Button(label.c_str(), {width, 0}))
          capture(row.action);
        if (clearable) {
          ImGui::SameLine();
          if (ImGui::Button("Clear##Counterclockwise", {60.F, 0}))
            capture(-1);
        }
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
  };
  if (ImGui::BeginTabBar("BindingGroups")) {
    if (ImGui::BeginTabItem("Shared")) {
      drawBindings(
          "SharedBindings",
          std::array<Row, 6>{
              {{"Primary action", values.bindings[7], 7},
               {"Secondary action", values.secondaryBinding,
                SecondaryBindingAction},
               {"Tool Menu", values.bindings[8], 8},
               {"Main Menu", values.menuKey, MenuBindingAction},
               {"Undo", values.historyBindings[0], HistoryBindingOffset},
               {"Redo", values.historyBindings[1], HistoryBindingOffset + 1}}});
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Build")) {
      std::array<Row, 10> rows;
      constexpr std::array<int, 9> indices{1, 0, 3, 2, 5, 4, 6, 9, 10};
      for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto index = indices[i];
        rows[i < 7 ? i : i + 1] = {actions[index], values.bindings[index],
                                   index};
      }
      rows[7] = {"Rotate Counterclockwise", values.counterclockwiseBinding,
                 CounterclockwiseBindingAction};
      drawBindings("BuildBindings", rows);
      UI::Widgets::Section("Modify tool");
      drawBindings(
          "ModifyBindings",
          std::array<Row, 2>{
              {{"Copy piece", values.modifyBindings[0], ModifyBindingOffset},
               {"Edit properties", values.modifyBindings[1],
                ModifyBindingOffset + 1}}});
      UI::Widgets::Section("Fine adjustments");
      std::array<Row, 14> fine;
      for (std::size_t i = 0; i < fine.size(); ++i)
        fine[i] = {fineActions[i], values.fineBindings[i],
                   static_cast<int>(actions.size() + i)};
      drawBindings("FineBindings", fine);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Terrain")) {
      drawBindings(
          "TerrainBindings",
          std::array<Row, 2>{{{"Increase brush size", values.brushBindings[0],
                               BrushBindingOffset},
                              {"Decrease brush size", values.brushBindings[1],
                               BrushBindingOffset + 1}}});
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
}
struct NavigationState {
  bool busy{}, canApply{}, canRevert{};
  std::size_t cachedCells{};
  std::string message, replacement;
};
enum class NavigationAction { None, Generate, Apply, Revert };
inline NavigationAction Navigation(const NavigationState &state) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  W::Help("Update NPC navigation after changing your build. Close this menu to "
          "run a command.");
  W::Section("Generation");
  BeginDisabled(state.busy);
  auto action = NavigationAction::None;
  if (W::PrimaryButton("Generate Navmesh"))
    action = NavigationAction::Generate;
  EndDisabled();
  SameLine();
  BeginDisabled(state.busy || !state.canApply);
  if (Button("Apply Cached Navmesh"))
    action = NavigationAction::Apply;
  EndDisabled();
  SameLine();
  BeginDisabled(state.busy || !state.canRevert);
  if (W::QuietButton("Revert Navmesh", true))
    action = NavigationAction::Revert;
  EndDisabled();
  W::Section("Status");
  W::Help(state.message.empty() ? "Ready to generate navigation."
                                : state.message.c_str());
  TextDisabled("Cached cells in this save: %zu", state.cachedCells);
  if (!state.replacement.empty() && CollapsingHeader("Installed navigation"))
    W::Help(state.replacement.c_str());
  return action;
}
template <class NavigationPage>
inline void Advanced(NavigationPage navigation) {
  navigation();
}
inline bool Gameplay(Values &values) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  bool changed = Checkbox("Sandbox mode", &values.sandboxMode);
  W::Help("Build freely without spending or refunding materials.");
  BeginDisabled(values.sandboxMode);
  W::FieldLabel("Building cost multiplier");
  changed |= SliderFloat("##BuildingCostMultiplier",
                         &values.buildingCostMultiplier, .1F, 10.F, "%.2fx");
  EndDisabled();
  return changed;
}
inline bool Appearance(Values &values) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  bool changed{};
  W::Section("Interface");
  changed |= Checkbox("Quick Select - Main Menu", &values.quickSelectMainMenu);
  W::SpaceBefore();
  changed |= Checkbox("Quick Select - Tool Menu", &values.quickSelectToolMenu);
  W::Help(
      "Quick Select: hold the binding, move to a choice, then release. Otherwise click a choice with the cursor.");
  W::FieldLabel("UI opacity");
  changed |= SliderFloat("##UIOpacity", &values.uiOpacity, 0, 1, "%.2f");
  W::FieldLabel("HUD background opacity");
  changed |= SliderFloat("##HUDBackgroundOpacity", &values.hudBackgroundOpacity,
                         0, 1, "%.2f");
  W::Section("Placeable icons");
  W::FieldLabel("Unselected icon opacity");
  changed |= SliderFloat("##IconOpacity", &values.iconOpacity, 0, 1, "%.2f");
  float brightness = values.iconBrightness * 100;
  W::FieldLabel("Brightness");
  if (SliderFloat("##IconBrightness", &brightness, 0, 200, "%.0f%%")) {
    values.iconBrightness = brightness / 100;
    changed = true;
  }
  if (W::FormButton("Reset appearance")) {
    const Values defaults;
    values.uiOpacity = defaults.uiOpacity;
    values.hudBackgroundOpacity = defaults.hudBackgroundOpacity;
    values.iconOpacity = defaults.iconOpacity;
    values.iconBrightness = defaults.iconBrightness;
    changed = true;
  }
  return changed;
}
inline bool Visuals(Values &values) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  bool changed{};
  W::Section("Placement preview");
  W::FieldLabel("Opacity");
  changed |=
      SliderFloat("##PreviewOpacity", &values.previewOpacity, 0, 1, "%.2f");
  W::FieldLabel("Tint");
  changed |= ColorEdit3("##PreviewTint", values.previewTint.data());
  W::Section("Modify tool");
  W::FieldLabel("Opacity");
  changed |=
      SliderFloat("##ModifyOpacity", &values.modifyOpacity, 0, 1, "%.2f");
  W::FieldLabel("Tint");
  changed |= ColorEdit3("##MoveTint", values.moveTint.data());
  if (W::FormButton("Reset visuals")) {
    const Values defaults;
    values.previewOpacity = defaults.previewOpacity;
    values.modifyOpacity = defaults.modifyOpacity;
    values.previewTint = defaults.previewTint;
    values.moveTint = defaults.moveTint;
    changed = true;
  }
  return changed;
}
inline bool Brush(Values &values) {
  using namespace ImGui;
  namespace W = UI::Widgets;
  bool changed{};
  W::Section("Brush");
  W::FieldLabel("Strength");
  changed |= SliderFloat("##TerrainStrength", &values.terrainStrengthPerSecond,
                         Terrain::MinBrushStrengthPerSecond,
                         Terrain::MaxBrushStrengthPerSecond, "%.0f");
  W::Help("Sculpt and Level move surfaces at this rate per second. Smooth "
          "controls how quickly uneven terrain relaxes.");
  return changed;
}
} // namespace SkyBuild::Settings::Fields
