#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace SkyBuild::Terrain::Grass {
// Bottled/Community Shaders Common/GrassInstance.hlsli CullGroup layout.
struct PackedGrassGroup {
  std::array<float, 3> origin;
  float fadeStart;
  std::uint32_t sourceStart, count;
  bool Valid(std::size_t available) const {
    return count > 0 && count <= 64 && sourceStart <= available &&
           count <= available - sourceStart;
  }
};
static_assert(sizeof(PackedGrassGroup) == 24);
static_assert(offsetof(PackedGrassGroup, sourceStart) == 16);
} // namespace SkyBuild::Terrain::Grass
