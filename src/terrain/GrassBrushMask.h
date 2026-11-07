#pragma once
#include "terrain/TerrainVolume.h"
#include <set>
namespace SkyBuild::Terrain::Grass {
// Quantized footprint: stationary pulses do not publish grass revisions. Tiles
// include the brush's bounded normal/surface support and a blade-width margin.
struct BrushMask {
  static constexpr float TileSize = 64, HiddenHeight = -30000;
  static float HiddenLocalHeight(float originZ) {
    return std::clamp(HiddenHeight - originZ, -65000.F, 65000.F);
  }
  std::set<std::pair<int, int>> tiles;
  bool Hidden(float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 16777216 ||
        std::abs(y) > 16777216)
      return false;
    return tiles.contains(
        {int(std::floor(x / TileSize)), int(std::floor(y / TileSize))});
  }
  bool Add(const Brush &brush) {
    const float radius = brush.radius + Step + 32;
    bool changed{};
    const int x0 = int(std::floor((brush.center.x - radius) / TileSize));
    const int x1 = int(std::floor((brush.center.x + radius) / TileSize));
    const int y0 = int(std::floor((brush.center.y - radius) / TileSize));
    const int y1 = int(std::floor((brush.center.y + radius) / TileSize));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float dx =
            brush.center.x -
            std::clamp(brush.center.x, x * TileSize, (x + 1) * TileSize);
        const float dy =
            brush.center.y -
            std::clamp(brush.center.y, y * TileSize, (y + 1) * TileSize);
        if (dx * dx + dy * dy <= radius * radius)
          changed |= tiles.emplace(x, y).second;
      }
    return changed;
  }
  bool Overlaps(float left, float bottom, float right, float top) const {
    for (const auto &[x, y] : tiles)
      if ((x + 1) * TileSize >= left && x * TileSize <= right &&
          (y + 1) * TileSize >= bottom && y * TileSize <= top)
        return true;
    return false;
  }
};
} // namespace SkyBuild::Terrain::Grass
