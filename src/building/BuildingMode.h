#pragma once

namespace SkyBuild::BuildingMode {
enum class InteractionMode { kIdle, kBuild, kModify, kPicker };

inline constexpr std::uint32_t HammerSaveRecord = 0x524D4148; // HAMR
void SaveHammerGrant(SKSE::SerializationInterface *serialization);
void LoadHammerGrant(SKSE::SerializationInterface *serialization,
                     std::uint32_t version, std::uint32_t length);
void ResetHammerGrant();
void Initialize();
void SetEnabled(bool enabled);
void SetSceneLoaded(bool a_loaded);
void Update(RE::PlayerCharacter *a_player);

[[nodiscard]] bool IsActive();
[[nodiscard]] bool IsHammerReady();
[[nodiscard]] bool IsBuildMode();
void SelectTool(InteractionMode tool);
[[nodiscard]] InteractionMode GetTool();
} // namespace SkyBuild::BuildingMode
