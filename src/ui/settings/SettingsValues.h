#pragma once
#include "terrain/TerrainTiming.h"
#include "building/InputChord.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace SkyBuild::Settings {
inline constexpr std::uint32_t MenuToggleKey = 48; // B: main wheel.
struct Bindings {
  InputChord menuKey{MenuToggleKey};
  InputChord secondaryBinding{257};
  std::array<InputChord, 2> fineCompletionBindings{256, 257};
  InputChord counterclockwiseBinding{42, 19};     // Left Shift + R.
  std::array<InputChord, 2> modifyBindings{46, 18};  // Copy, properties.
  std::array<InputChord, 2> historyBindings{44, 45}; // Undo / redo.
  std::array<InputChord, 2> brushBindings{
      264, 265}; // Increase / decrease radius.
  std::array<InputChord, 11> bindings{265, 264, 16,  18, 46, 47,
                                         19,  256, 258, 20, 33};
  std::array<InputChord, 14> fineBindings{203, 205, 200, 208, 201, 209, 36,
                                             38,  23,  37,  22,  24,  12,  13};
};
inline Bindings GamepadDefaults() {
  Bindings b;
  b.menuKey = 271;
  b.secondaryBinding = 277;
  b.fineCompletionBindings = {270, 271};
  b.counterclockwiseBinding = 0;
  b.modifyBindings = {267, 266};
  b.historyBindings = {268, 269};
  b.brushBindings = {275, 274};
  b.bindings = {274, 275, 280, 281, 266, 267, 279, 276, 278, 272, 273};
  b.fineBindings = {268, 269, 266, 267, 280, 274, 278, 277, 279, 276, 0, 0, 275, 281};
  return b;
}
struct Values : Bindings {
  Bindings gamepad{GamepadDefaults()};
  bool sandboxMode{false};
  bool showBuildingMaterialNames{true};
  float buildingCostMultiplier{1.F};
  bool quickSelectMainMenu{false}, quickSelectToolMenu{true};
  float terrainStrengthPerSecond{Terrain::BrushStrengthPerSecond};
  float uiOpacity{1.0F};
  float hudBackgroundOpacity{0.8F};
  float previewOpacity{0.5F};
  float modifyOpacity{0.65F};
  float iconOpacity{0.5F};
  float iconBrightness{1.0F};
  std::array<float, 3> previewTint{0.0F, 1.0F, 0.0F};
  std::array<float, 3> moveTint{0.1F, 0.45F, 1.0F};
};
inline constexpr int HistoryBindingOffset = 25;
inline constexpr int MenuBindingAction = 27;
inline constexpr int BrushBindingOffset = 28;
inline constexpr int ModifyBindingOffset = 30;
inline constexpr int SecondaryBindingAction = 32;
inline constexpr int CounterclockwiseBindingAction = 33;
inline constexpr int FineCompletionBindingOffset = 34;
inline constexpr int BindingCount = 36;
template <class T> inline auto &BindingAt(T &bindings, int action) {
  if (action >= FineCompletionBindingOffset) return bindings.fineCompletionBindings.at(action - FineCompletionBindingOffset);
  if (action == CounterclockwiseBindingAction) return bindings.counterclockwiseBinding;
  if (action == SecondaryBindingAction) return bindings.secondaryBinding;
  if (action >= ModifyBindingOffset) return bindings.modifyBindings.at(action - ModifyBindingOffset);
  if (action >= BrushBindingOffset) return bindings.brushBindings.at(action - BrushBindingOffset);
  if (action == MenuBindingAction) return bindings.menuKey;
  if (action >= HistoryBindingOffset) return bindings.historyBindings.at(action - HistoryBindingOffset);
  if (action >= 11) return bindings.fineBindings.at(action - 11);
  return bindings.bindings.at(action);
}
enum BindingScope : unsigned {
  ScopeBuild = 1, ScopeModify = 2, ScopeMove = 4, ScopeFine = 8, ScopeTerrain = 16, ScopePicker = 32, ScopeIdle = 64, ScopeWheel = 128
};
inline unsigned BindingScopes(int action) {
  constexpr unsigned all = ScopeBuild | ScopeModify | ScopeMove | ScopeFine | ScopeTerrain | ScopePicker | ScopeIdle | ScopeWheel;
  if (action >= FineCompletionBindingOffset) return ScopeFine;
  if (action == MenuBindingAction || action == 8) return all & ~ScopeFine;
  if (action == 7 || action == SecondaryBindingAction) return all & ~(ScopeIdle | ScopeFine);
  if (action == HistoryBindingOffset || action == HistoryBindingOffset + 1)
    return ScopeBuild | ScopeModify | ScopeMove | ScopeTerrain;
  if (action == BrushBindingOffset || action == BrushBindingOffset + 1) return ScopeTerrain;
  if (action == ModifyBindingOffset || action == ModifyBindingOffset + 1) return ScopeModify;
  if (action >= 11 && action < HistoryBindingOffset) return ScopeFine;
  if (action == 6 || action == 9 || action == 10 || action == CounterclockwiseBindingAction)
    return ScopeBuild | ScopeMove;
  return ScopeBuild;
}
template <class KnownKey>
bool ValidateBindings(const Bindings &values, KnownKey known) {
  for (int i = 0; i < BindingCount; ++i) {
    const auto code = BindingAt(values, i);
    if (!code) continue;
    if (code.keys.size() > 2) return false;
    for (std::size_t k = 0; k < code.keys.size(); ++k) {
      const auto key = code.keys[k];
      if (!known(key) || key == 0 || key == 1) return false;
      if (std::find(code.keys.begin(), code.keys.begin() + k, key) != code.keys.begin() + k)
        return false;
      if ((key == 264 || key == 265) &&
          (k + 1 != code.keys.size() || (i != 0 && i != 1 &&
           i != BrushBindingOffset && i != BrushBindingOffset + 1))) return false;
    }
    for (int j = 0; j < i; ++j)
      if (code == BindingAt(values, j) && (BindingScopes(i) & BindingScopes(j)))
        return false;
  }
  return true;
}
// A new assignment only displaces controls that can run in the same scope.
inline void AssignBinding(Bindings &values, int action, const InputChord &code) {
  if (code)
    for (int i = 0; i < BindingCount; ++i)
      if (i != action && BindingAt(values, i) == code &&
          (BindingScopes(action) & BindingScopes(i)))
        BindingAt(values, i) = 0;
  BindingAt(values, action) = code;
}
template <class KnownKey>
bool ValidateAndNormalize(Values &values, KnownKey known) {
  if (!ValidateBindings(values, known))
    return false;
  if (!std::isfinite(values.buildingCostMultiplier))
    return false;
  values.buildingCostMultiplier =
      std::clamp(values.buildingCostMultiplier, .1F, 10.F);
  if (!std::isfinite(values.terrainStrengthPerSecond))
    return false;
  values.terrainStrengthPerSecond = std::clamp(
      values.terrainStrengthPerSecond, Terrain::MinBrushStrengthPerSecond,
      Terrain::MaxBrushStrengthPerSecond);
  if (!std::isfinite(values.uiOpacity) ||
      !std::isfinite(values.hudBackgroundOpacity) ||
      !std::isfinite(values.previewOpacity) ||
      !std::isfinite(values.modifyOpacity) ||
      !std::isfinite(values.iconOpacity) ||
      !std::isfinite(values.iconBrightness))
    return false;
  values.uiOpacity = std::clamp(values.uiOpacity, 0.0F, 1.0F);
  values.hudBackgroundOpacity =
      std::clamp(values.hudBackgroundOpacity, 0.0F, 1.0F);
  values.previewOpacity = std::clamp(values.previewOpacity, 0.0F, 1.0F);
  values.modifyOpacity = std::clamp(values.modifyOpacity, 0.F, 1.F);
  values.iconOpacity = std::clamp(values.iconOpacity, 0.0F, 1.0F);
  values.iconBrightness = std::clamp(values.iconBrightness, 0.0F, 2.0F);
  for (auto *tint : {&values.previewTint, &values.moveTint}) {
    for (auto &channel : *tint) {
      if (!std::isfinite(channel))
        return false;
      channel = std::clamp(channel, 0.0F, 1.0F);
    }
  }
  return true;
}
} // namespace SkyBuild::Settings
