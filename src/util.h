#pragma once

namespace SkyBuild::Util {
void SetUI3DCamera(RE::UI3DSceneManager *a_sceneManager,
                   const RE::NiPoint3 &a_position,
                   const RE::NiMatrix3 &a_rotation,
                   std::optional<float> a_fov = std::nullopt);

[[nodiscard]]
bool PlaySoundAt(const RE::TESObjectREFR *refr, RE::FormID a_soundFormID,
                 float volume);

void EnableWithFade(RE::TESObjectREFR *a_reference, bool a_fadeIn);
} // namespace SkyBuild::Util
