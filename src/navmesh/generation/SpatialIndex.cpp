#include "navmesh/generation/SpatialIndex.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace SkyBuild::Navmesh {
namespace {
bool Finite(Point p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
         std::abs(p.x) <= 1e7F && std::abs(p.y) <= 1e7F &&
         std::abs(p.z) <= 1e7F;
}
std::uint32_t Cell(float value, float minimum, float length,
                   std::uint32_t size) {
  return static_cast<std::uint32_t>(
      std::clamp(std::floor((value - minimum) / length), 0.0F,
                 static_cast<float>(size - 1)));
}
std::uint32_t LowerCell(float value, float minimum, float length,
                        std::uint32_t size) {
  // Expand after normalization: a world-coordinate ULP can disappear when
  // subtracting the minimum, notably at a zero-coordinate bucket boundary.
  const float coordinate =
      std::nextafter((value - minimum) / length, -INFINITY);
  return static_cast<std::uint32_t>(
      std::clamp(std::floor(coordinate), 0.0F, static_cast<float>(size - 1)));
}
} // namespace
const std::vector<std::uint16_t> *SpatialIndex::At(Point p) const {
  if (!Finite(p) || size == 0 || buckets.size() != size * size ||
      columnLength <= 0 || rowLength <= 0 || p.x < minimum.x ||
      p.x > maximum.x || p.y < minimum.y || p.y > maximum.y)
    return nullptr;
  return &buckets[Cell(p.y, minimum.y, rowLength, size) * size +
                  Cell(p.x, minimum.x, columnLength, size)];
}
IndexResult BuildSpatialIndex(const Mesh &mesh, std::uint32_t size) {
  IndexResult result;
  if (size == 0 || size > 64 || mesh.vertices.size() > 65535 ||
      mesh.triangles.size() > 65535) {
    result.error = "Invalid navmesh grid size or 16-bit geometry limits";
    return result;
  }
  auto &grid = result.index;
  grid.size = size;
  grid.minimum = {std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max()};
  grid.maximum = {-grid.minimum.x, -grid.minimum.y, -grid.minimum.z};
  for (const auto &vertex : mesh.vertices) {
    if (!Finite(vertex)) {
      result.error = "Invalid navmesh grid vertex";
      return result;
    }
    grid.minimum = {std::min(grid.minimum.x, vertex.x),
                    std::min(grid.minimum.y, vertex.y),
                    std::min(grid.minimum.z, vertex.z)};
    grid.maximum = {std::max(grid.maximum.x, vertex.x),
                    std::max(grid.maximum.y, vertex.y),
                    std::max(grid.maximum.z, vertex.z)};
  }
  if (mesh.vertices.empty())
    grid.minimum = grid.maximum = {};
  // Nonzero XY extents prevent division by zero in native lookup even for an
  // empty mesh or a degenerate axis. These are lookup bounds, not cell bounds.
  if (grid.maximum.x - grid.minimum.x < 1)
    grid.maximum.x = grid.minimum.x + 1;
  if (grid.maximum.y - grid.minimum.y < 1)
    grid.maximum.y = grid.minimum.y + 1;
  grid.columnLength = (grid.maximum.x - grid.minimum.x) / size;
  grid.rowLength = (grid.maximum.y - grid.minimum.y) / size;
  grid.buckets.resize(size * size);
  std::size_t entries = 0;
  for (std::uint32_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto &triangle = mesh.triangles[i];
    if (std::ranges::any_of(triangle.vertices, [&](auto id) {
          return id >= mesh.vertices.size();
        })) {
      result.error = "Invalid navmesh grid triangle index";
      return result;
    }
    if (triangle.flags & (8U | 32U))
      continue;
    const auto a = mesh.vertices[triangle.vertices[0]],
               b = mesh.vertices[triangle.vertices[1]],
               c = mesh.vertices[triangle.vertices[2]];
    // Include both sides of an exact bucket boundary. Conservative triangle
    // AABBs may return extra candidates; native triangle tests reject them.
    const auto x0 = LowerCell(std::min({a.x, b.x, c.x}), grid.minimum.x,
                              grid.columnLength, size);
    const auto x1 = Cell(std::max({a.x, b.x, c.x}), grid.minimum.x,
                         grid.columnLength, size);
    const auto y0 = LowerCell(std::min({a.y, b.y, c.y}), grid.minimum.y,
                              grid.rowLength, size);
    const auto y1 =
        Cell(std::max({a.y, b.y, c.y}), grid.minimum.y, grid.rowLength, size);
    for (auto y = y0; y <= y1; ++y)
      for (auto x = x0; x <= x1; ++x) {
        if (++entries > 4'000'000) {
          result.error = "Navmesh spatial lookup budget exceeded";
          return result;
        }
        grid.buckets[y * size + x].push_back(static_cast<std::uint16_t>(i));
      }
  }
  return result;
}
} // namespace SkyBuild::Navmesh
