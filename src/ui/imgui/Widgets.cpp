#include "ui/imgui/Widgets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <im_anim.h>
#include <imgui_internal.h>
#include <mutex>
#include <unordered_map>
namespace SkyBuild::UI::Widgets {
namespace {
ImGuiWindow *backChild{};
bool backHandledByNavigation{};
}
void PrepareGamepadNavigation() {
  const auto &context = *ImGui::GetCurrentContext();
  backChild = nullptr;
  backHandledByNavigation = context.ActiveId != 0 || context.NavLayer != ImGuiNavLayer_Main;
  if (context.OpenPopupStack.Size && context.OpenPopupStack.back().Window &&
      !(context.OpenPopupStack.back().Window->Flags & ImGuiWindowFlags_Modal))
    backHandledByNavigation = true;
  if (!backHandledByNavigation && context.NavWindow &&
      (context.NavWindow->Flags & ImGuiWindowFlags_ChildWindow)) {
    backChild = context.NavWindow;
    backHandledByNavigation = true;
  }
}
bool EscapePressed() {
  static int handledFrame = -1;
  const int frame = ImGui::GetFrameCount();
  if (handledFrame == frame ||
      !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    return false;
  const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
  if (!escape && !back) return false;
  handledFrame = frame;
  if (!escape && backHandledByNavigation) {
    // ImGui already handles ordinary children; modal children need the same step.
    auto &context = *ImGui::GetCurrentContext();
    if (backChild && context.NavWindow == backChild && backChild->ParentWindow) {
      auto *parent = backChild->ParentWindow;
      ImGui::FocusWindow(parent);
      if (backChild->ChildId)
        ImGui::SetNavID(backChild->ChildId, ImGuiNavLayer_Main, 0,
                       ImGui::WindowRectAbsToRel(parent, backChild->Rect()));
    }
    return false;
  }
  return true;
}

namespace {
std::atomic<float> hudBackgroundAlpha{.8F};
std::atomic_bool playstationIcons{false};
} // namespace
void SetGamepadIcons(bool playstation) { playstationIcons.store(playstation); }
void SetHUDBackgroundOpacity(float value) {
  if (std::isfinite(value))
    hudBackgroundAlpha.store(std::clamp(value, 0.F, 1.F));
}
const std::filesystem::path &Root() {
  static const auto root = std::filesystem::current_path() / "Data/SkyBuild/UI";
  return root;
}
void ApplyBaseStyle() {
  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 6;
  style.WindowPadding = {18, 16};
  style.FramePadding = {10, 5};
  style.ItemSpacing = {8, 6};
  style.CellPadding = {12, 5};
  style.ButtonTextAlign = {.5F, .5F};
  style.FrameBorderSize = style.ChildBorderSize = 0;
  style.FrameRounding = style.GrabRounding = 3;
  style.WindowTitleAlign = {.02F, .5F};
  style.Colors[ImGuiCol_TitleBg] = {.045F, .052F, .057F, 1};
  style.Colors[ImGuiCol_TitleBgActive] = {.075F, .09F, .095F, 1};
  style.Colors[ImGuiCol_TitleBgCollapsed] = {.045F, .052F, .057F, 1};
  style.Colors[ImGuiCol_PopupBg] = {.065F, .074F, .079F, 1};
  style.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
  style.Colors[ImGuiCol_ScrollbarGrab] = {.22F, .26F, .28F, .7F};
  style.Colors[ImGuiCol_CheckMark] = {.65F, .82F, .8F, 1};
  style.Colors[ImGuiCol_SliderGrab] = {.3F, .48F, .5F, 1};
  style.Colors[ImGuiCol_SliderGrabActive] = {.45F, .64F, .65F, 1};
  style.Colors[ImGuiCol_Header] = {.14F, .22F, .24F, 1};
  style.Colors[ImGuiCol_HeaderHovered] = {.19F, .28F, .3F, 1};
  style.Colors[ImGuiCol_HeaderActive] = {.23F, .31F, .33F, 1};
  style.Colors[ImGuiCol_Tab] = {.09F, .108F, .116F, 1};
  style.Colors[ImGuiCol_TabSelected] = {.14F, .22F, .24F, 1};
  style.Colors[ImGuiCol_TabHovered] = {.19F, .28F, .3F, 1};
  style.Colors[ImGuiCol_TabSelectedOverline] = {.4F, .6F, .61F, 1};
  style.Colors[ImGuiCol_Separator] = {.23F, .27F, .29F, .4F};
}
Theme::Theme(float opacity) {
  using namespace ImGui;
  PushStyleVar(ImGuiStyleVar_Alpha, opacity);
  PushStyleVar(ImGuiStyleVar_WindowRounding, 8.F);
  PushStyleVar(ImGuiStyleVar_FrameRounding, 4.F);
  PushStyleVar(ImGuiStyleVar_WindowPadding, {18, 16});
  PushStyleVar(ImGuiStyleVar_FramePadding, {10, 5});
  PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 6});
  PushStyleVar(ImGuiStyleVar_CellPadding, {12, 5});
  PushStyleColor(ImGuiCol_PopupBg, {.055F, .055F, .055F, 1.F});
  PushStyleColor(ImGuiCol_WindowBg, {.055F, .055F, .055F, .96F});
  PushStyleColor(ImGuiCol_ChildBg, {.08F, .08F, .08F, .7F});
  PushStyleColor(ImGuiCol_Border, {.65F, .70F, .72F, .2F});
  PushStyleColor(ImGuiCol_Text, {.91F, .92F, .90F, 1});
  PushStyleColor(ImGuiCol_TextDisabled, {.53F, .57F, .58F, 1});
  PushStyleColor(ImGuiCol_FrameBg, {.16F, .18F, .19F, 1});
  PushStyleColor(ImGuiCol_FrameBgHovered, {.24F, .28F, .30F, 1});
  PushStyleColor(ImGuiCol_FrameBgActive, {.30F, .35F, .37F, 1});
  PushStyleColor(ImGuiCol_Button, {.20F, .20F, .20F, 1});
  PushStyleColor(ImGuiCol_ButtonHovered, {.32F, .32F, .32F, 1});
  PushStyleColor(ImGuiCol_ButtonActive, {.40F, .40F, .40F, 1});
  PushStyleColor(ImGuiCol_Header, {.14F, .22F, .24F, 1});
  PushStyleColor(ImGuiCol_HeaderHovered, {.19F, .28F, .3F, 1});
  PushStyleColor(ImGuiCol_HeaderActive, {.23F, .31F, .33F, 1});
  PushStyleColor(ImGuiCol_CheckMark, {.65F, .82F, .8F, 1});
  PushStyleColor(ImGuiCol_SliderGrab, {.3F, .48F, .5F, 1});
  PushStyleColor(ImGuiCol_TableRowBg, {1, 1, 1, .015F});
  PushStyleColor(ImGuiCol_TableRowBgAlt, {1, 1, 1, .045F});
}
Theme::~Theme() {
  ImGui::PopStyleColor(19);
  ImGui::PopStyleVar(7);
}
WorkspaceTheme::WorkspaceTheme()
    : previousFontScale(ImGui::GetIO().FontGlobalScale) {
  using namespace ImGui;
  // Workspace windows share a text scale; HUD panels and tool wheels keep
  // their own sizing. Assign rather than multiply for nested workspaces.
  GetIO().FontGlobalScale = 1.15F;
  PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 6});
  PushStyleVar(ImGuiStyleVar_FramePadding, {10, 5});
  PushStyleVar(ImGuiStyleVar_CellPadding, {12, 5});
  PushStyleVar(ImGuiStyleVar_WindowPadding, {18, 16});
  PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.F);
  PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.F);
  PushStyleVar(ImGuiStyleVar_FrameRounding, 3.F);
  PushStyleColor(ImGuiCol_WindowBg, {.055F, .063F, .067F, .99F});
  PushStyleColor(ImGuiCol_ChildBg, {.065F, .074F, .079F, .7F});
  PushStyleColor(ImGuiCol_FrameBg, {.105F, .121F, .129F, 1.F});
  PushStyleColor(ImGuiCol_Button, {.125F, .148F, .156F, 1.F});
  PushStyleColor(ImGuiCol_ButtonHovered, {.18F, .23F, .25F, 1.F});
  PushStyleColor(ImGuiCol_ButtonActive, {.22F, .29F, .31F, 1.F});
  PushStyleColor(ImGuiCol_Text, {.85F, .88F, .89F, 1.F});
  PushStyleColor(ImGuiCol_TextDisabled, {.48F, .55F, .58F, 1.F});
  PushStyleColor(ImGuiCol_TableHeaderBg, {.09F, .108F, .116F, 1.F});
  PushStyleColor(ImGuiCol_TableRowBg, {0, 0, 0, 0});
  PushStyleColor(ImGuiCol_TableRowBgAlt, {1, 1, 1, .018F});
  PushStyleColor(ImGuiCol_TableBorderLight, {.21F, .25F, .27F, .32F});
  PushStyleColor(ImGuiCol_TableBorderStrong, {.25F, .29F, .31F, .4F});
  PushStyleColor(ImGuiCol_Border, {.23F, .27F, .29F, .3F});
}
WorkspaceTheme::~WorkspaceTheme() {
  ImGui::PopStyleColor(14);
  ImGui::PopStyleVar(7);
  ImGui::GetIO().FontGlobalScale = previousFontScale;
}
void WorkspaceFont(float pixels) {
  // Measure after resetting this window: child font scaling can inherit its
  // parent's scale. A fixed pixel target must not multiply that scale again.
  ImGui::SetWindowFontScale(1.F);
  const auto base = ImGui::GetFontSize();
  if (base > 0)
    ImGui::SetWindowFontScale(pixels * ImGui::GetIO().FontGlobalScale / base);
}
void Help(const char *text) {
  using namespace ImGui;
  PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled));
  PushTextWrapPos(GetCursorPosX() + std::min(620.F, GetContentRegionAvail().x));
  TextUnformatted(text);
  PopTextWrapPos();
  PopStyleColor();
}
// Use the bottom of submitted content so nested helpers cannot add the same gap
// twice.
void SpaceBefore(Gap gap) {
  auto *window = ImGui::GetCurrentWindow();
  const auto &layout = window->DC;
  if (window->SkipItems || layout.CursorMaxPos.y <= layout.CursorStartPos.y)
    return;
  const float distance = gap == Gap::Section ? 24.F : 16.F;
  const float next = layout.CursorMaxPos.y + distance;
  if (next > layout.CursorPos.y)
    ImGui::SetCursorScreenPos({layout.CursorPos.x, next});
}
bool FormButton(const char *label, bool destructive) {
  SpaceBefore();
  return QuietButton(label, destructive);
}
void SectionDivider() {
  ImGui::Separator();
  SpaceBefore();
}
void PageHeading(const char *title) {
  WorkspaceFont(22);
  ImGui::TextUnformatted(title);
  WorkspaceFont();
  SectionDivider();
}
void SidebarHeading(const char *title) {
  ImGui::TextDisabled("%s", title);
  SpaceBefore();
}
void SectionHeading(const char *title) {
  SpaceBefore(Gap::Section);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(title);
}
void Section(const char *title, const char *description) {
  SectionHeading(title);
  if (description)
    Help(description);
  SectionDivider();
}
void FieldLabel(const char *label, float maxWidth) {
  SpaceBefore();
  ImGui::TextDisabled("%s", label);
  ImGui::SetNextItemWidth(
      std::max(1.F, std::min(maxWidth, ImGui::GetContentRegionAvail().x)));
}
bool QuietButton(const char *label, bool destructive, bool compact) {
  using namespace ImGui;
  PushStyleColor(ImGuiCol_Button, destructive
                                      ? ImVec4{.20F, .115F, .115F, 1.F}
                                      : GetStyleColorVec4(ImGuiCol_Button));
  PushStyleColor(ImGuiCol_ButtonHovered, destructive
                                             ? ImVec4{.3F, .14F, .14F, 1.F}
                                             : ImVec4{.16F, .21F, .23F, 1.F});
  PushStyleColor(ImGuiCol_Text, destructive ? ImVec4{.76F, .53F, .5F, 1.F}
                                            : ImVec4{.65F, .72F, .74F, 1.F});
  if (compact)
    PushStyleVar(ImGuiStyleVar_FramePadding, {8, 2});
  const float height = GetFrameHeight();
  const bool clicked = Button(label, {0, height});
  if (compact)
    PopStyleVar();
  PopStyleColor(3);
  return clicked;
}
bool PrimaryButton(const char *label, ImGui::ImVec2 size) {
  using namespace ImGui;
  PushStyleColor(ImGuiCol_Button, {.18F, .32F, .34F, 1.F});
  PushStyleColor(ImGuiCol_ButtonHovered, {.23F, .41F, .43F, 1.F});
  PushStyleColor(ImGuiCol_ButtonActive, {.28F, .46F, .48F, 1.F});
  const bool clicked = Button(label, size);
  PopStyleColor(3);
  return clicked;
}
bool SidebarItem(const char *label, bool selected, int count) {
  using namespace ImGui;
  const auto pos = GetCursorScreenPos();
  const float width = std::max(1.F, GetContentRegionAvail().x);
  const float height = std::max(30.F, GetFontSize() + 12.F);
  PushID(label);
  const float fade =
      iam_tween_float(GetID("##selection"), 0, selected ? 1.F : 0.F, .14F,
                      iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade,
                      GetIO().DeltaTime);
  PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.F);
  PushStyleColor(ImGuiCol_Button, ImVec4{.09F + .05F * fade, .11F + .11F * fade,
                                         .12F + .12F * fade, 1.F});
  PushStyleColor(ImGuiCol_ButtonHovered, {.19F, .26F, .28F, 1.F});
  PushStyleColor(ImGuiCol_ButtonActive, {.23F, .31F, .33F, 1.F});
  const bool clicked = Button("##SidebarItem", {width, height});
  const bool hovered = IsItemHovered();
  PopStyleColor(3);
  PopStyleVar();
  auto *draw = GetWindowDrawList();
  if (fade > .01F)
    ImDrawListManager::AddRectFilled(
        draw, {pos.x, pos.y + 7.F}, {pos.x + 3.F, pos.y + height - 7.F},
        GetColorU32(ImVec4{.53F, .77F, .78F, fade}), 1.F, 0);
  const char *end = std::strstr(label, "##");
  const std::string text(label, end ? end : label + std::strlen(label));
  const auto size = CalcTextSize(text.c_str());
  const auto countText = count < 0 ? std::string{} : std::to_string(count);
  const auto countSize = CalcTextSize(countText.c_str());
  const float right = pos.x + width - 12.F;
  const float textRight =
      right - (countText.empty() ? 0.F : countSize.x + 12.F);
  PushClipRect({pos.x + 14.F, pos.y},
               {std::max(pos.x + 14.F, textRight), pos.y + height}, true);
  ImDrawListManager::AddText(draw,
                             {pos.x + 14.F, pos.y + (height - size.y) * .5F},
                             GetColorU32(ImGuiCol_Text), text.c_str());
  PopClipRect();
  if (!countText.empty())
    ImDrawListManager::AddText(
        draw, {right - countSize.x, pos.y + (height - countSize.y) * .5F},
        GetColorU32(ImGuiCol_TextDisabled), countText.c_str());
  if (hovered && size.x > textRight - pos.x - 14.F)
    SetTooltip("%s", text.c_str());
  PopID();
  return clicked;
}
bool Combo(const char *label, const char *preview) {
  using namespace ImGui;
  if (label[0] != '#') {
    const float labelX = GetCursorPosX();
    AlignTextToFramePadding();
    TextUnformatted(label);
    SameLine(labelX + std::max(110.F, CalcTextSize(label).x + 14.F));
  }
  return BeginCombo((std::string("##") + label).c_str(), preview);
}
bool Input(const char *label, std::string &value, float width) {
  std::array<char, 2048> buffer{};
  std::memcpy(buffer.data(), value.data(),
              std::min(value.size(), buffer.size() - 1));
  using namespace ImGui;
  if (label[0] != '#') {
    const float labelX = GetCursorPosX();
    AlignTextToFramePadding();
    TextUnformatted(label);
    SameLine(labelX + std::max(110.F, CalcTextSize(label).x + 14.F));
  }
  if (width)
    SetNextItemWidth(width);
  if (!InputText((std::string("##") + label).c_str(), buffer.data(),
                 buffer.size()))
    return false;
  value = buffer.data();
  return true;
}
namespace {
ImTextureID KeyTexture(std::string_view icon, ImGui::ImVec2 &dimensions) {
  static const auto keys =
      Root().parent_path().parent_path() / "Interface/ImGuiIcons/Icons";
  std::string file(icon);
  if (file.starts_with("Pad")) {
    file.erase(0, 3);
    if (file != "Up" && file != "Down" && file != "Left" && file != "Right") {
      file = std::string(playstationIcons.load() ? "PS3_" : "360_") + file;
    }
  } else if (file.starts_with("Key"))
    file.erase(0, 3);
  else if (file.starts_with("Digit"))
    file.erase(0, 5);
  else if (file.starts_with("Arrow"))
    file.erase(0, 5);
  static const std::unordered_map<std::string, std::string> names{
      {"MouseLeft", "Mouse1"},
      {"MouseRight", "Mouse2"},
      {"MouseMiddle", "Mouse3"},
      {"MouseWheelUp", "WheelUp"},
      {"MouseWheelDown", "WheelDown"},
      {"MouseWheel", "Wheel"},
      {"Escape", "Esc"},
      {"PageUp", "PgUp"},
      {"PageDown", "PgDn"},
      {"ShiftLeft", "L-Shift"},
      {"ShiftRight", "R-Shift"},
      {"ControlLeft", "L-Ctrl"},
      {"ControlRight", "R-Ctrl"},
      {"AltLeft", "L-Alt"},
      {"AltRight", "R-Alt"},
      {"Minus", "Hyphen"},
      {"BracketLeft", "Bracketleft"},
      {"BracketRight", "Bracketright"},
      {"Quote", "Quotesingle"},
      {"Backquote", "Tilde"},
      {"NumpadEnter", "KeypadEnter"},
      {"NumpadDecimal", "NumPadDec"},
      {"NumpadAdd", "NumPadPlus"},
      {"NumpadSubtract", "NumPadMinus"},
      {"NumpadMultiply", "NumPadMult"},
      {"NumpadDivide", "NumPadDivide"},
      {"Numpad0", "NumPad0"},
      {"Numpad9", "NumPad9"}};
  if (const auto found = names.find(file); found != names.end())
    file = found->second;
  else if (file.starts_with("Numpad"))
    file = "Keypad" + file.substr(6);
  return Texture(keys / (file + ".png"), 1.F, &dimensions, nullptr, {0, 30});
}
float IconWidth(ImGui::ImVec2 dimensions) {
  return dimensions.y > 0 ? 30.F * dimensions.x / dimensions.y : 30.F;
}
} // namespace
namespace {
float ChordGap(std::string_view first, std::string_view second) {
  ImGui::ImVec2 dimensions;
  return 3.F - (KeyTexture(first, dimensions) ? 9.F : 0.F)
             - (KeyTexture(second, dimensions) ? 9.F : 0.F);
}
}
float KeyWidth(std::string_view icon) {
  if (const auto split = icon.find('|'); split != std::string_view::npos)
    return KeyWidth(icon.substr(0, split)) +
           ChordGap(icon.substr(0, split), icon.substr(split + 1)) +
           KeyWidth(icon.substr(split + 1));
  ImGui::ImVec2 dimensions;
  return KeyTexture(icon, dimensions) ? IconWidth(dimensions) + 18.F : 48.F;
}
void DrawKey(std::string_view icon, std::string_view label, ImGui::ImVec2 position) {
  if (const auto split = icon.find('|'); split != std::string_view::npos) {
    const auto first = icon.substr(0, split);
    DrawKey(first, {}, position);
    const auto second = icon.substr(split + 1);
    const auto x = position.x + KeyWidth(first) + ChordGap(first, second);
    DrawKey(second, label, {x, position.y});
    return;
  }
  ImGui::ImVec2 dimensions;
  const auto texture = KeyTexture(icon, dimensions);
  const float imageWidth = IconWidth(dimensions);
  const float width = texture ? imageWidth + 18.F : 48.F;
  const float labelX = width + 10.F;
  auto *draw = ImGui::GetWindowDrawList();
  const auto color =
      0x00FFFFFFU |
      (static_cast<std::uint32_t>(ImGui::GetStyle().Alpha * 255) << 24);
  if (texture) {
    const auto fb = ImGui::GetIO().DisplayFramebufferScale;
    const ImGui::ImVec2 start{std::round((position.x + 9) * fb.x) / fb.x,
                              std::round(position.y * fb.y) / fb.y};
    ImGui::ImDrawListManager::AddImage(
        draw, texture, start,
        {start.x + dimensions.x / fb.x, start.y + dimensions.y / fb.y}, {0, 0},
        {1, 1}, color);
  } else {
    std::string key(icon);
    if (key.starts_with("Pad"))
      key.erase(0, 3);
    else if (key.starts_with("Key"))
      key.erase(0, 3);
    else if (key.starts_with("Digit"))
      key.erase(0, 5);
    if (key == "MouseLeft")
      key = "M1";
    else if (key == "MouseRight")
      key = "M2";
    else if (key == "MouseWheelUp")
      key = "Up";
    else if (key == "MouseWheelDown")
      key = "Dn";
    else if (key == "MouseWheel")
      key = "Scroll";
    else if (key == "PageUp")
      key = "PgUp";
    else if (key == "PageDown")
      key = "PgDn";
    else if (key.starts_with("Arrow"))
      key.erase(0, 5);
    const auto size = ImGui::CalcTextSize(key.c_str());
    ImGui::ImDrawListManager::AddRect(
        draw, position, {position.x + 48, position.y + 30}, color, 4, 0, 1);
    ImGui::ImDrawListManager::AddText(
        draw,
        {position.x + (48 - size.x) * .5F, position.y + (30 - size.y) * .5F},
        color, key.c_str());
  }
  if (!label.empty()) {
    const std::string text(label);
    const auto size = ImGui::CalcTextSize(text.c_str());
    ImGui::ImDrawListManager::AddText(
        draw, {position.x + labelX, position.y + (30 - size.y) * .5F}, color,
        text.c_str());
  }
}
void Key(std::string_view icon, std::string_view label) {
  const auto position = ImGui::GetCursorScreenPos();
  const auto width = KeyWidth(icon);
  const std::string text(label);
  ImGui::Dummy({label.empty() ? width
                             : width + 10.F + ImGui::CalcTextSize(text.c_str()).x,
                30});
  DrawKey(icon, label, position);
}
void Picture(const std::filesystem::path &path, float size, float opacity,
             float brightness) {
  if (auto texture = Texture(path, brightness))
    ImGui::ImageWithBg(texture, {size, size}, {0, 0}, {1, 1}, {},
                       {1, 1, 1, opacity});
  else
    ImGui::Dummy({size, size});
}
bool Panel(const char *name, ImGui::ImVec2 position, ImGui::ImVec2 size,
           bool interactive) {
  using namespace ImGui;
  SetNextWindowPos(position);
  SetNextWindowSize(size);
  auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (!interactive)
    flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar |
             ImGuiWindowFlags_NoScrollWithMouse;
  PushStyleColor(ImGuiCol_WindowBg,
                 {.04F, .04F, .04F, hudBackgroundAlpha.load()});
  const bool shown = Begin(name, nullptr, flags);
  PopStyleColor();
  SetWindowFontScale(1.F);
  return shown;
}
} // namespace SkyBuild::UI::Widgets
