#pragma once
#include <chrono>
namespace SkyBuild::Terrain {
// Time-based editing rate, independent of render/update frequency.
inline constexpr float BrushStrengthPerSecond = 100.F;
inline constexpr float MinBrushStrengthPerSecond = 1.F;
inline constexpr float MaxBrushStrengthPerSecond = 200.F;
inline constexpr auto MaxStrengthElapsed = std::chrono::milliseconds(250);
} // namespace SkyBuild::Terrain
