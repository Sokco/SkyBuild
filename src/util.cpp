#include "util.h"

namespace SkyBuild::Util {
void SetUI3DCamera(RE::UI3DSceneManager *a_sceneManager,
                   const RE::NiPoint3 &a_position,
                   const RE::NiMatrix3 &a_rotation,
                   std::optional<float> a_fov) {
  if (!a_sceneManager) {
    return;
  }

  using SetCameraPosition_t =
      void(RE::UI3DSceneManager *, const RE::NiPoint3 &);
  using SetCameraRotation_t =
      void(RE::UI3DSceneManager *, const RE::NiMatrix3 &);
  using SetCameraFOV_t = void(RE::UI3DSceneManager *, float);
  static REL::Relocation<SetCameraPosition_t> setPosition{
      RELOCATION_ID(51867, 52739)};
  static REL::Relocation<SetCameraRotation_t> setRotation{
      RELOCATION_ID(51869, 52741)};
  static REL::Relocation<SetCameraFOV_t> setFOV{RELOCATION_ID(51870, 52742)};

  setPosition(a_sceneManager, a_position);
  setRotation(a_sceneManager, a_rotation);
  if (a_fov) {
    setFOV(a_sceneManager, *a_fov);
  }
}

bool PlaySoundAt(const RE::TESObjectREFR *refr, RE::FormID a_soundFormID,
                 float volume) {
  auto *descriptor =
      RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(a_soundFormID);

  if (!descriptor) {
    spdlog::error("Sound descriptor {:08X} was not found.", a_soundFormID);

    return false;
  }

  if (!refr || !descriptor)
    return false;

  RE::BSSoundHandle sound;
  if (const auto manager = RE::BSAudioManager::GetSingleton()) {

    if (!manager->GetSoundHandle(sound, descriptor)) {
      return false;
    }

    if (!sound.SetPosition(refr->GetPosition())) {
      return false;
    }

    sound.SetObjectToFollow(refr->Get3D());
    sound.SetVolume(volume);
    return sound.Play();
  }

  return false;
}

void EnableWithFade(RE::TESObjectREFR *a_reference, bool a_fadeIn) {
  if (!a_reference) {
    return;
  }

  using func_t = void(RE::BSScript::Internal::VirtualMachine *, RE::VMStackID,
                      RE::TESObjectREFR *, bool);

  static REL::Relocation<func_t> func{RELOCATION_ID(55629, 56158)};

  auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

  if (!vm) {
    spdlog::error("Could not enable reference: Papyrus VM unavailable.");

    return;
  }

  constexpr RE::VMStackID stackID = 0;

  func(vm, stackID, a_reference, a_fadeIn);
}
} // namespace SkyBuild::Util
