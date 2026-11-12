#pragma once
namespace SkyBuild::ControlBindings { struct Event; }
namespace SkyBuild::UI::FineAdjustments {
void Open();
void Close();
void Update();
bool IsOpen();
bool HandleButton(const ControlBindings::Event *button);
} // namespace SkyBuild::UI::FineAdjustments
