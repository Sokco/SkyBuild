#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
namespace SkyBuild::UI {
inline void ApplyImageBrightness(std::span<std::uint8_t> rgba,
                                 float brightness) {
  brightness =
      std::isfinite(brightness) ? std::clamp(brightness, 0.F, 2.F) : 1.F;
  if (brightness == 1.F)
    return;
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
    for (std::size_t channel = 0; channel < 3; ++channel)
      rgba[i + channel] = static_cast<std::uint8_t>(
          std::clamp(std::lround(rgba[i + channel] * brightness), 0L, 255L));
}
} // namespace SkyBuild::UI
