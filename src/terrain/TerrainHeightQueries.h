#pragma once
#include "terrain/TerrainVolume.h"

namespace SkyBuild::Terrain {
using LandHeights = std::array<float, 4 * 17 * 17>;

// LAND queries expose a single height, whereas collision is volumetric. Use
// the lowest solid-to-air crossing for below-ground recovery; Havok continues
// to supply the actual contact surface, including roofs and overhangs.
inline float LowestSurface(const Volume &volume, float x, float y) {
  float previous = volume.Density({x, y, volume.bottom});
  for (float z = volume.bottom + Step; z <= volume.top; z += Step) {
    const float next = volume.Density({x, y, z});
    if (previous <= 0 && next > 0)
      return z - Step + Step * (-previous / (next - previous));
    previous = next;
  }
  throw std::runtime_error("No terrain recovery surface in volume");
}

inline LandHeights RecoveryHeights(const Volume &volume,
                                   const LandHeights &original, float base) {
  if (!volume.Valid() || !std::isfinite(base))
    throw std::runtime_error("Invalid landscape recovery-height inputs");
  auto result = original;
  if (!HasTerrainChanges(volume))
    return result;
  std::array<float, Side * Side> lowered;
  lowered.fill(std::numeric_limits<float>::infinity());
  for (int y = 0; y < Side; ++y)
    for (int x = 0; x < Side; ++x) {
      const float wx = volume.origin.x + x * Step,
                  wy = volume.origin.y + y * Step;
      // Outside every subtractive brush's support the original surface remains
      // the lowest crossing, so no field scan is necessary.
      const bool affected =
          volume.density
              ? DensityAffected(volume, wx, wy)
              : std::any_of(volume.edits.begin(), volume.edits.end(),
                            [&](const Brush &b) {
                              return !b.add &&
                                     std::abs(wx - b.center.x) <= b.radius &&
                                     std::abs(wy - b.center.y) <= b.radius;
                            });
      if (!affected)
        continue;
      const float floor = LowestSurface(volume, wx, wy);
      if (floor < volume.Height(wx, wy) - .01F)
        lowered[y * Side + x] = floor;
    }
  for (int q = 0; q < 4; ++q)
    for (int y = 0; y < 17; ++y)
      for (int x = 0; x < 17; ++x) {
        const int gx = (q % 2) * 32 + x * 2, gy = (q / 2) * 32 + y * 2;
        float low = std::numeric_limits<float>::infinity();
        // LAND's grid spacing is 128; marching cubes uses 64. A conservative
        // minimum over adjacent LAND tiles prevents the coarser height query
        // from placing a recovery plane above a narrow excavated floor.
        for (int dy = -2; dy <= 2; ++dy)
          for (int dx = -2; dx <= 2; ++dx)
            if (gx + dx >= 0 && gx + dx < Side && gy + dy >= 0 &&
                gy + dy < Side)
              low = std::min(low, lowered[(gy + dy) * Side + gx + dx]);
        const auto index = q * 289 + y * 17 + x;
        if (std::isfinite(low))
          result[index] = std::min(original[index], low - base - .5F);
      }
  return result;
}
} // namespace SkyBuild::Terrain
