#pragma once
#include "ui/imgui/Widgets.h"
#include <algorithm>
#include <span>

namespace SkyBuild::UI::SettingsWorkspace {
struct Page {
  std::string name;
  void(__stdcall *draw)();
};
inline void Draw(std::span<const Page> pages, bool &open,
                 std::size_t &selected) {
  using namespace ImGui;
  Widgets::WorkspaceTheme theme;
  const auto screen = GetIO().DisplaySize;
  SetNextWindowPos({screen.x * .5F, screen.y * .5F}, ImGuiCond_Appearing,
                   {.5F, .5F});
  SetNextWindowSize(
      {std::min(1040.F, screen.x * .94F), std::min(760.F, screen.y * .94F)},
      ImGuiCond_FirstUseEver);
  SetNextWindowSizeConstraints(
      {std::min(800.F, screen.x * .94F), std::min(520.F, screen.y * .94F)},
      screen);
  if (Begin("SkyBuild Settings", &open, ImGuiWindowFlags_NoCollapse)) {
    Widgets::WorkspaceFont();
    BeginChild("Settings navigation", {170, 0},
               ImGuiChildFlags_AlwaysUseWindowPadding);
    Widgets::SidebarHeading("SETTINGS");
    for (std::size_t i = 0; i < pages.size(); ++i)
      if (Widgets::SidebarItem(pages[i].name.c_str(), selected == i))
        selected = i;
    EndChild();
    SameLine();
    BeginChild("Settings content", {0, 0},
               ImGuiChildFlags_AlwaysUseWindowPadding);
    if (selected < pages.size()) {
      Widgets::PageHeading(pages[selected].name.c_str());
      pages[selected].draw();
    }
    EndChild();
  }
  if (Widgets::EscapePressed())
    open = false;
  End();
}
} // namespace SkyBuild::UI::SettingsWorkspace
