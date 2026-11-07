#pragma once
#include <nlohmann/json.hpp>

namespace SkyBuild::UI {
bool Initialize();

void LoadPlaceableCarousel();
nlohmann::json GetPlaceableCatalog();
nlohmann::json GetPlaceableAppearance();

void SetVisible(bool a_visible);
void RefreshPresentation();
void OpenToolWheel();
void OpenMainMenu();
bool WheelHasCursor();
void FinishToolWheel(bool clicked = false);
void MoveToolWheel(float x, float y);
void CancelToolWheel();
[[nodiscard]] bool IsToolWheelOpen();
void RefreshControlBindings();
void SetOpacity(float a_opacity);
void SetSnappingEnabled(bool a_enabled);
void SetIconAppearance(float a_opacity, float a_brightness);
[[nodiscard]] bool IsGameMenuBlocking(bool ignoreFineAdjustments = false);
void SetActivePlaceable(RE::FormID a_formID);
void SetThumbnailPreview(RE::FormID a_formID, std::string_view a_relativePath);
void SetThumbnailStatus(std::string_view a_status);
} // namespace SkyBuild::UI
