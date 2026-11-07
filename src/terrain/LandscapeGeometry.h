#pragma once
#include "terrain/TerrainVolume.h"
namespace SkyBuild::Terrain {
struct LandscapeGeometry {
  std::vector<Vec> vertices;
  std::vector<std::array<std::uint16_t, 3>> triangles;
};
// Borrow the exact native index list, validating the shared LAND index buffer
// identity when Skyrim omits per-shape CPU indices. Use only during capture.
const std::uint16_t *LandscapeIndices(RE::BSTriShape *geometry,
                                      const RE::BSGraphics::TriShape *renderer);
// Positions/triangles only. No material, UV, texture weights, bins or logging.
LandscapeGeometry CaptureLandscapeGeometry(RE::BSTriShape *geometry);
} // namespace SkyBuild::Terrain
