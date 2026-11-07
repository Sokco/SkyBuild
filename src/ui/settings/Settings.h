#pragma once
namespace SkyBuild::Settings {
std::uint32_t MenuBinding(); // Keyboard scan code or 256 + mouse button.
bool SandboxMode();
float BuildingCostMultiplier();
bool QuickSelectMainMenu();
bool QuickSelectToolMenu();
void Initialize();
void RefreshIconPacks();
void SetMenuOpen(bool open);
void ShowIconGenerationProgress(bool autoClose = false);
[[nodiscard]] bool IsMenuOpen();
// Called before Menu Framework processes its open/close shortcuts.
bool CaptureInput(RE::InputEvent *const *events);
} // namespace SkyBuild::Settings
