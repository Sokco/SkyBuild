#pragma once
#include "ui/imgui/NativeUI.h"
#include <filesystem>
#include <string>

namespace SkyBuild::UI::Widgets {
inline constexpr float TableRowHeight = 30.F;
void ApplyBaseStyle();
void SetHUDBackgroundOpacity(float value);
struct Theme {
  explicit Theme(float opacity = 1.F);
  ~Theme();
};
const std::filesystem::path &Root();
ImTextureID Texture(const std::filesystem::path &path, float brightness = 1.F,
                    ImGui::ImVec2 *dimensions = nullptr,
                    ImGui::ImVec2 *visibleCenter = nullptr,
                    ImGui::ImVec2 displaySize = {0, 0});
void Invalidate(const std::filesystem::path &path, bool discard = false);
void BeginFrame();
// Call inside the window, after its children. Popups retain their own focus.
bool EscapePressed();
struct WorkspaceTheme {
  WorkspaceTheme();
  ~WorkspaceTheme();

private:
  float previousFontScale;
};
void WorkspaceFont(float pixels = 18.F);
void Help(const char *text);
enum class Gap { Control, Section };
void SpaceBefore(Gap gap = Gap::Control);
bool FormButton(const char *label, bool destructive = false);
void SectionDivider();
void PageHeading(const char *title);
void SidebarHeading(const char *title);
void SectionHeading(const char *title);
void Section(const char *title, const char *description = nullptr);
void FieldLabel(const char *label, float maxWidth = 560.F);
bool QuietButton(const char *label, bool destructive = false,
                 bool compact = false);
bool PrimaryButton(const char *label, ImGui::ImVec2 size = {0, 0});
bool SidebarItem(const char *label, bool selected, int count = -1);
bool Combo(const char *label, const char *preview);
bool Input(const char *label, std::string &value, float width = 0);
template <class Render> void Rows(std::size_t count, Render render) {
  if (!count)
    return;
  using namespace ImGui;
  auto *clipper = ImGuiListClipperManager::Create();
  struct Release {
    ImGuiListClipper *value;
    ~Release() { ImGuiListClipperManager::Destroy(value); }
  } release{clipper};
  ImGuiListClipperManager::Begin(clipper, static_cast<int>(count), -1);
  while (ImGuiListClipperManager::Step(clipper))
    for (int row = clipper->DisplayStart; row < clipper->DisplayEnd; ++row)
      render(static_cast<std::size_t>(row));
}
float KeyWidth(std::string_view icon);
void Key(std::string_view icon, std::string_view label);
void Picture(const std::filesystem::path &path, float size, float opacity = 1.F,
             float brightness = 1.F);
bool Panel(const char *name, ImGui::ImVec2 position, ImGui::ImVec2 size,
           bool interactive = false);
} // namespace SkyBuild::UI::Widgets
