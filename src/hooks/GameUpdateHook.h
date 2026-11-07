#pragma once

namespace SkyBuild::Hooks {
class GameUpdateHook {
public:
  static void Install();

private:
  static void Thunk(RE::PlayerCharacter *a_player, float a_deltaTime);

  static inline REL::Relocation<decltype(Thunk)> Original;
};
} // namespace SkyBuild::Hooks