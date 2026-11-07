#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace SkyBuild::UI::ThumbnailPixels {
using Pixel = std::array<std::uint8_t, 4>;

inline Pixel DecodeBGRA(const std::uint8_t *a_source, bool a_rgba,
                        bool a_tenBit) {
  if (a_tenBit) {
    std::uint32_t packed{};
    std::memcpy(&packed, a_source, sizeof(packed));
    const auto channel = [packed](unsigned a_shift) {
      return static_cast<std::uint8_t>(
          (((packed >> a_shift) & 1023U) * 255U + 511U) / 1023U);
    };
    return {channel(20), channel(10), channel(0), 255};
  }
  return a_rgba ? Pixel{a_source[2], a_source[1], a_source[0], a_source[3]}
                : Pixel{a_source[0], a_source[1], a_source[2], a_source[3]};
}

// Cblack = alpha * Cobject; Cwhite - Cblack = 255 * (1 - alpha).
// Coverage comes from the two backgrounds, preserving opaque black materials.
inline Pixel RecoverMatte(const Pixel &a_black, const Pixel &a_white) {
  int transmission = 0;
  for (unsigned channel = 0; channel < 3; ++channel) {
    transmission += static_cast<int>(a_white[channel]) - a_black[channel];
  }
  const int coverage = std::clamp(255 - (transmission + 1) / 3, 0, 255);
  if (coverage <= 2) {
    return {};
  }
  Pixel result{};
  for (unsigned channel = 0; channel < 3; ++channel) {
    result[channel] = static_cast<std::uint8_t>(std::min(
        255,
        (static_cast<int>(a_black[channel]) * 255 + coverage / 2) / coverage));
  }
  result[3] = static_cast<std::uint8_t>(coverage);
  return result;
}
} // namespace SkyBuild::UI::ThumbnailPixels
