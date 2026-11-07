#pragma once
#include "terrain/TerrainDensity.h"
#include <map>
namespace SkyBuild::Terrain {
struct DensitySnapshot {
  std::vector<Volume> volumes;
  std::map<std::tuple<float, float, float>, std::optional<float>> samples;
  std::optional<float> CanonicalSample(Vec world) {
    const auto value = Sample(world);
    if (!value)
      return {};
    return CanonicalDensityValue(*value, [&](int x, int y, int z) {
      return Sample(world + Vec{x * Step, y * Step, z * Step});
    });
  }
  std::optional<float> Sample(Vec world) {
    const auto key = std::tuple{world.x, world.y, world.z};
    if (const auto found = samples.find(key); found != samples.end())
      return found->second;
    std::optional<float> result;
    for (const auto &volume : volumes) {
      if (!volume.density || world.x < volume.origin.x ||
          world.y < volume.origin.y || world.x >= volume.origin.x + CellSize ||
          world.y >= volume.origin.y + CellSize)
        continue;
      const auto &grid = *volume.density->current;
      const int z = int(std::lround(world.z / Step));
      if (z < grid.low || z > grid.high)
        break;
      const unsigned x =
          unsigned(std::lround((world.x - volume.origin.x) / Step)) + 1;
      const unsigned y =
          unsigned(std::lround((world.y - volume.origin.y) / Step)) + 1;
      result = grid.At({z, y * DensitySide + x});
      break;
    }
    samples.emplace(key, result);
    return result;
  }
};
} // namespace SkyBuild::Terrain
