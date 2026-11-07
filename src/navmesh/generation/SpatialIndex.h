#pragma once
#include "navmesh/generation/MeshBuilder.h"

namespace SkyBuild::Navmesh {
struct SpatialIndex {
  std::uint32_t size{};
  Point minimum{}, maximum{};
  float columnLength{}, rowLength{};
  std::vector<std::vector<std::uint16_t>> buckets;
  [[nodiscard]] const std::vector<std::uint16_t> *At(Point point) const;
};
struct IndexResult {
  SpatialIndex index;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
[[nodiscard]] IndexResult BuildSpatialIndex(const Mesh &mesh,
                                            std::uint32_t size = 16);
} // namespace SkyBuild::Navmesh
