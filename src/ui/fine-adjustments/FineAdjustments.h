#pragma once
namespace SkyBuild::UI::FineAdjustments {
void Open();
void Close();
void Update();
bool IsOpen();
bool HandleButton(RE::ButtonEvent *button);
} // namespace SkyBuild::UI::FineAdjustments
