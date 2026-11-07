#pragma once
#include "terrain/TerrainVolume.h"
#include <optional>

namespace SkyBuild::Terrain {
struct SurfacePoint {
  float height;
  Vec normal;
};
// Immutable CPU copy of the same triangles supplied to rendering and Havok.
class TerrainSurface {
public:
  TerrainSurface(Mesh source, Vec chunkOrigin)
      : mesh(std::move(source)), origin(chunkOrigin) {
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
      const auto &t = mesh.triangles[i];
      const auto a = mesh.vertices[t[0]].position,
                 b = mesh.vertices[t[1]].position,
                 c = mesh.vertices[t[2]].position;
      if (Cross(b - a, c - a).z <= 0)
        continue;
      const int x0 = Bin(std::min({a.x, b.x, c.x}) - origin.x),
                x1 = Bin(std::max({a.x, b.x, c.x}) - origin.x);
      const int y0 = Bin(std::min({a.y, b.y, c.y}) - origin.y),
                y1 = Bin(std::max({a.y, b.y, c.y}) - origin.y);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
          bins[y * 17 + x].push_back(static_cast<std::uint16_t>(i));
    }
  }
  std::optional<SurfacePoint> Highest(float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y) || x < origin.x ||
        y < origin.y || x > origin.x + 1024 || y > origin.y + 1024)
      return {};
    std::optional<SurfacePoint> result;
    for (auto index : bins[Bin(y - origin.y) * 17 + Bin(x - origin.x)]) {
      const auto &t = mesh.triangles[index];
      const auto &a = mesh.vertices[t[0]], &b = mesh.vertices[t[1]],
                 &c = mesh.vertices[t[2]];
      const auto u = b.position - a.position, v = c.position - a.position;
      const double px = double(x) - a.position.x, py = double(y) - a.position.y;
      const double det = double(u.x) * v.y - double(u.y) * v.x;
      if (det <= 0)
        continue;
      const double s = (px * v.y - py * v.x) / det,
                   tv = (double(u.x) * py - double(u.y) * px) / det;
      if (s < -1e-7 || tv < -1e-7 || s + tv > 1 + 1e-7)
        continue;
      const float height =
          static_cast<float>(a.position.z + s * u.z + tv * v.z);
      if (!result || height > result->height)
        result = SurfacePoint{height,
                              Unit(a.normal * static_cast<float>(1 - s - tv) +
                                   b.normal * static_cast<float>(s) +
                                   c.normal * static_cast<float>(tv))};
    }
    return result;
  }

  const Mesh &Geometry() const { return mesh; }

private:
  static int Bin(float value) {
    return std::clamp(int(std::floor(value / Step)), 0, 16);
  }
  Mesh mesh;
  Vec origin;
  std::array<std::vector<std::uint16_t>, 17 * 17> bins;
};
} // namespace SkyBuild::Terrain
