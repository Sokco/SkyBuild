#pragma once
namespace SkyBuild::Settings {
bool SandboxMode();
float BuildingCostMultiplier();
bool QuickSelectMainMenu();
bool QuickSelectToolMenu();
void Initialize();
void RefreshIconPacks();
void SetMenuOpen(bool open);
void ShowIconGenerationProgress(bool autoClose = false);
[[nodiscard]] bool IsMenuOpen();
// Captures rebinding before tools or gameplay receive input.
bool CaptureInput(RE::InputEvent *const *events);
} // namespace SkyBuild::Settings
