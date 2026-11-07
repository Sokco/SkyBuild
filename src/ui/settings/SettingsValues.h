#pragma once
#include "terrain/TerrainTiming.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace SkyBuild::Settings {
inline constexpr std::uint32_t MenuToggleKey = 48; // B: main wheel.
struct Values {
  bool sandboxMode{false};
  float buildingCostMultiplier{1.F};
  std::uint32_t menuKey{MenuToggleKey};
  std::uint32_t secondaryBinding{257};
  std::uint32_t counterclockwiseBinding{};             // Zero means unbound.
  std::array<std::uint32_t, 2> modifyBindings{46, 18}; // Copy, properties.
  bool quickSelectMainMenu{false}, quickSelectToolMenu{true};
  float terrainStrengthPerSecond{Terrain::BrushStrengthPerSecond};
  std::array<std::uint32_t, 2> historyBindings{44, 45}; // Undo / redo.
  std::array<std::uint32_t, 2> brushBindings{
      264, 265}; // Increase / decrease radius.
  std::array<std::uint32_t, 11> bindings{265, 264, 16,  18, 46, 47,
                                         19,  256, 258, 20, 33};
  std::array<std::uint32_t, 14> fineBindings{203, 205, 200, 208, 201, 209, 36,
                                             38,  23,  37,  22,  24,  12,  13};
  float uiOpacity{1.0F};
  float hudBackgroundOpacity{0.8F};
  float previewOpacity{0.5F};
  float modifyOpacity{0.65F};
  float iconOpacity{0.5F};
  float iconBrightness{1.0F};
  std::array<float, 3> previewTint{0.0F, 1.0F, 0.0F};
  std::array<float, 3> moveTint{0.1F, 0.45F, 1.0F};
};
template <class KnownKey>
bool ValidateAndNormalize(Values &values, KnownKey known) {
  if (!known(values.menuKey) || values.menuKey == 1 || values.menuKey == 264 ||
      values.menuKey == 265)
    return false;
  if (!known(values.secondaryBinding) || values.secondaryBinding == 1 ||
      values.secondaryBinding == 264 || values.secondaryBinding == 265 ||
      values.secondaryBinding == values.menuKey)
    return false;
  for (std::size_t i = 0; i < values.bindings.size(); ++i) {
    if (values.bindings[i] == values.menuKey ||
        values.bindings[i] == values.secondaryBinding ||
        values.bindings[i] == 1 || !known(values.bindings[i]))
      return false;
    if (i > 1 && (values.bindings[i] == 264 || values.bindings[i] == 265))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (values.bindings[i] == values.bindings[j])
        return false;
  }
  for (std::size_t i = 0; i < values.modifyBindings.size(); ++i) {
    const auto key = values.modifyBindings[i];
    if (!known(key) || key == 1 || key == 264 || key == 265 ||
        key == values.menuKey || key == values.secondaryBinding ||
        key == values.bindings[7] || key == values.bindings[8] ||
        key == values.bindings[6] || key == values.bindings[9] ||
        key == values.bindings[10] ||
        std::ranges::find(values.historyBindings, key) !=
            values.historyBindings.end())
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (key == values.modifyBindings[j])
        return false;
  }
  for (std::size_t i = 0; i < values.fineBindings.size(); ++i) {
    if (values.fineBindings[i] == values.menuKey ||
        values.fineBindings[i] == 1 ||
        values.fineBindings[i] == values.bindings[7] ||
        values.fineBindings[i] == values.secondaryBinding ||
        !known(values.fineBindings[i]))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (values.fineBindings[i] == values.fineBindings[j])
        return false;
  }
  for (std::size_t i = 0; i < values.historyBindings.size(); ++i) {
    const auto key = values.historyBindings[i];
    if (key == values.menuKey || !known(key) || key == values.bindings[7] ||
        key == values.bindings[8] || key == values.secondaryBinding ||
        key == 1 || key == 264 || key == 265 ||
        std::find(values.bindings.begin(), values.bindings.end(), key) !=
            values.bindings.end())
      return false; // Secondary terrain action.
    for (std::size_t j = 0; j < i; ++j)
      if (key == values.historyBindings[j])
        return false;
  }
  for (std::size_t i = 0; i < values.brushBindings.size(); ++i) {
    const auto key = values.brushBindings[i];
    if (!known(key) || key == values.menuKey || key == 1 ||
        key == values.secondaryBinding || key == values.bindings[7] ||
        key == values.bindings[8] ||
        std::find(values.historyBindings.begin(), values.historyBindings.end(),
                  key) != values.historyBindings.end())
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (key == values.brushBindings[j])
        return false;
  }
  if (const auto key = values.counterclockwiseBinding; key != 0) {
    if (!known(key) || key == 1 || key == 264 || key == 265 ||
        key == values.menuKey || key == values.secondaryBinding ||
        std::ranges::find(values.bindings, key) != values.bindings.end() ||
        std::ranges::find(values.historyBindings, key) !=
            values.historyBindings.end() ||
        std::ranges::find(values.modifyBindings, key) !=
            values.modifyBindings.end())
      return false;
  }
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
