#pragma once
#include <atomic>
#include <imgui.h>
#include <memory>
namespace RE {
class InputEvent;
}

namespace ImGui {
using ::ImDrawFlags_Closed;
using ::ImDrawList;
using ::ImGuiCol_Border;
using ::ImGuiCol_Button;
using ::ImGuiCol_ButtonActive;
using ::ImGuiCol_ButtonHovered;
using ::ImGuiCol_CheckMark;
using ::ImGuiCol_ChildBg;
using ::ImGuiCol_FrameBg;
using ::ImGuiCol_FrameBgActive;
using ::ImGuiCol_FrameBgHovered;
using ::ImGuiCol_Header;
using ::ImGuiCol_HeaderActive;
using ::ImGuiCol_HeaderHovered;
using ::ImGuiCol_PopupBg;
using ::ImGuiCol_SliderGrab;
using ::ImGuiCol_TableBorderLight;
using ::ImGuiCol_TableBorderStrong;
using ::ImGuiCol_TableHeaderBg;
using ::ImGuiCol_TableRowBg;
using ::ImGuiCol_TableRowBgAlt;
using ::ImGuiCol_Text;
using ::ImGuiCol_TextDisabled;
using ::ImGuiCol_WindowBg;
using ::ImGuiCond_Always;
using ::ImGuiCond_Appearing;
using ::ImGuiHoveredFlags_AllowWhenDisabled;
using ::ImGuiHoveredFlags_DelayNormal;
using ::ImGuiListClipper;
using ::ImGuiSliderFlags_NoInput;
using ::ImGuiSortDirection_Descending;
using ::ImGuiStyleVar_Alpha;
using ::ImGuiStyleVar_ButtonTextAlign;
using ::ImGuiStyleVar_CellPadding;
using ::ImGuiStyleVar_ChildBorderSize;
using ::ImGuiStyleVar_FrameBorderSize;
using ::ImGuiStyleVar_FramePadding;
using ::ImGuiStyleVar_FrameRounding;
using ::ImGuiStyleVar_ItemSpacing;
using ::ImGuiStyleVar_SelectableTextAlign;
using ::ImGuiStyleVar_WindowPadding;
using ::ImGuiStyleVar_WindowRounding;
using ::ImGuiTableBgTarget_CellBg;
using ::ImGuiTableColumnFlags_NoSort;
using ::ImGuiTableColumnFlags_WidthFixed;
using ::ImGuiTableColumnFlags_WidthStretch;
using ::ImGuiTableFlags_BordersInnerH;
using ::ImGuiTableFlags_NoSavedSettings;
using ::ImGuiTableFlags_PadOuterX;
using ::ImGuiTableFlags_Resizable;
using ::ImGuiTableFlags_RowBg;
using ::ImGuiTableFlags_ScrollY;
using ::ImGuiTableFlags_SizingStretchSame;
using ::ImGuiTableFlags_Sortable;
using ::ImGuiTreeNodeFlags_SpanAvailWidth;
using ::ImGuiWindowFlags_AlwaysAutoResize;
using ::ImGuiWindowFlags_NoBackground;
using ::ImGuiWindowFlags_NoCollapse;
using ::ImGuiWindowFlags_NoDecoration;
using ::ImGuiWindowFlags_NoInputs;
using ::ImGuiWindowFlags_NoMove;
using ::ImGuiWindowFlags_NoResize;
using ::ImGuiWindowFlags_NoSavedSettings;
using ::ImGuiWindowFlags_NoScrollbar;
using ::ImGuiWindowFlags_NoScrollWithMouse;
using ::ImGuiWindowFlags_NoTitleBar;
using ::ImU32;
using ::ImVec2;
using ::ImVec4;
namespace ImDrawListManager {
inline void AddCircleFilled(ImDrawList *self, const ImVec2 center, float radius,
                            ImU32 col, int num_segments) {
  self->AddCircleFilled(center, radius, col, num_segments);
}
inline void AddConcavePolyFilled(ImDrawList *self, const ImVec2 *points,
                                 int num_points, ImU32 col) {
  self->AddConcavePolyFilled(points, num_points, col);
}
inline void AddConvexPolyFilled(ImDrawList *self, const ImVec2 *points,
                                int num_points, ImU32 col) {
  self->AddConvexPolyFilled(points, num_points, col);
}
inline void AddImage(ImDrawList *self, ImTextureID user_texture_id,
                     const ImVec2 p_min, const ImVec2 p_max,
                     const ImVec2 uv_min, const ImVec2 uv_max, ImU32 col) {
  self->AddImage(user_texture_id, p_min, p_max, uv_min, uv_max, col);
}
inline void AddLine(ImDrawList *self, const ImVec2 p1, const ImVec2 p2,
                    ImU32 col, float thickness) {
  self->AddLine(p1, p2, col, thickness);
}
inline void AddRect(ImDrawList *self, const ImVec2 p_min, const ImVec2 p_max,
                    ImU32 col, float rounding, ImDrawFlags flags,
                    float thickness) {
  self->AddRect(p_min, p_max, col, rounding, flags, thickness);
}
inline void AddRectFilled(ImDrawList *self, const ImVec2 p_min,
                          const ImVec2 p_max, ImU32 col, float rounding,
                          ImDrawFlags flags) {
  self->AddRectFilled(p_min, p_max, col, rounding, flags);
}
inline void AddText(ImDrawList *self, const ImVec2 pos, ImU32 col,
                    const char *text_begin, const char *text_end = 0) {
  self->AddText(pos, col, text_begin, text_end);
}
inline void AddText(ImDrawList *self, ImFont *font, float font_size,
                    const ImVec2 pos, ImU32 col, const char *text_begin,
                    const char *text_end = 0, float wrap_width = 0.0f,
                    const ImVec4 *cpu_fine_clip_rect = nullptr) {
  self->AddText(font, font_size, pos, col, text_begin, text_end, wrap_width,
                cpu_fine_clip_rect);
}
inline void PathArcTo(ImDrawList *self, const ImVec2 center, float radius,
                      float a_min, float a_max, int num_segments) {
  self->PathArcTo(center, radius, a_min, a_max, num_segments);
}
inline void PathStroke(ImDrawList *self, ImU32 col, ImDrawFlags flags,
                       float thickness) {
  self->PathStroke(col, flags, thickness);
}
} // namespace ImDrawListManager
namespace ImGuiListClipperManager {
inline ImGuiListClipper *Create() { return new ImGuiListClipper; }
inline void Destroy(ImGuiListClipper *c) { delete c; }
inline void Begin(ImGuiListClipper *c, int n, float h) { c->Begin(n, h); }
inline bool Step(ImGuiListClipper *c) { return c->Step(); }
} // namespace ImGuiListClipperManager
} // namespace ImGui

namespace SkyBuild::UI::Native {
using Callback = void(__stdcall *)();
namespace Model {
struct WindowInterface {
  std::atomic_bool IsOpen{false}, BlockUserInput{true};
};
enum EventType { kOpenMenu, kCloseMenu };
using EventCallback = void(__stdcall *)(EventType);
struct Event {
  explicit Event(EventCallback, float = 0);
  ~Event();
  EventCallback callback;
};
struct HudElement {
  explicit HudElement(Callback);
  ~HudElement();
  Callback callback;
};
} // namespace Model
bool Install();
void RegisterMenu();
bool CaptureInputEvent(RE::InputEvent *);
void ResetInput();
Model::WindowInterface *AddWindow(Callback, bool block = true);
Model::WindowInterface *GetMainWindow();
bool IsAnyBlockingWindowOpened();
void SetSection(const char *);
void AddSectionItem(const char *, Callback);
} // namespace SkyBuild::UI::Native
