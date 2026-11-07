#pragma once

#include "ui/imgui/NativeUI.h"
#include <algorithm>

namespace SkyBuild::UI::MenuLayout {
inline void SectionSpacing() {
  ImGui::Dummy({0.0F, std::max(10.0F, ImGui::GetFontSize() * 0.35F)});
}
inline void SpacedSeparator() {
  SectionSpacing();
  ImGui::Separator();
  SectionSpacing();
}
} // namespace SkyBuild::UI::MenuLayout
