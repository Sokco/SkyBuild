#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace SkyBuild::Terrain {
struct Vec {
  float x{}, y{}, z{};
  Vec operator+(Vec b) const { return {x + b.x, y + b.y, z + b.z}; }
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  Vec operator*(float s) const { return {x * s, y * s, z * s}; }
};
inline float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec Cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec a) { return std::sqrt(Dot(a, a)); }
inline Vec Unit(Vec a) {
  const auto l = Length(a);
  return l > 1e-6F ? a * (1 / l) : Vec{0, 0, 1};
}
inline bool Finite(Vec p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
constexpr int Side = 65;
constexpr float Step = 64, CellSize = 4096;
constexpr std::size_t MaxEdits = 4096;
struct Brush {
  Vec center;
  float radius = 160;
  bool add{};
  float strength{}; // World displacement for Sculpt/Level; density relaxation
                    // for Smooth. Zero is full CSG.
  std::uint64_t stroke{};
};
// Include the sampling/normal halo so cells share the same field at borders.
inline bool BrushTouchesCell(const Brush &brush, Vec origin,
                             float padding = 2 * Step) {
  const auto x = std::clamp(brush.center.x, origin.x, origin.x + CellSize);
  const auto y = std::clamp(brush.center.y, origin.y, origin.y + CellSize);
  const auto dx = x - brush.center.x, dy = y - brush.center.y,
             r = brush.radius + padding;
  return dx * dx + dy * dy <= r * r;
}
inline std::vector<std::pair<int, int>> BrushCells(const Brush &brush,
                                                   float padding = 2 * Step) {
  std::vector<std::pair<int, int>> result;
  const auto r = brush.radius + padding;
  if (!Finite(brush.center) || !std::isfinite(r) || brush.radius <= 0 ||
      padding < 0 || std::abs(brush.center.x) > 16777216 ||
      std::abs(brush.center.y) > 16777216 || r > 4 * CellSize)
    throw std::runtime_error("Terrain brush support exceeds cell limits");
  const int x0 = int(std::floor((brush.center.x - r) / CellSize));
  const int x1 = int(std::floor((brush.center.x + r) / CellSize));
  const int y0 = int(std::floor((brush.center.y - r) / CellSize));
  const int y1 = int(std::floor((brush.center.y + r) / CellSize));
  if (std::uint64_t(x1 - x0 + 1) * std::uint64_t(y1 - y0 + 1) > 64)
    throw std::runtime_error("Terrain brush support exceeds cell limits");
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      if (BrushTouchesCell(brush, {x * CellSize, y * CellSize, 0}, padding))
        result.emplace_back(x, y);
  return result;
}
inline bool SameBrush(const Brush &a, const Brush &b) {
  return a.center.x == b.center.x && a.center.y == b.center.y &&
         a.center.z == b.center.z && a.radius == b.radius && a.add == b.add &&
         a.strength == b.strength && a.stroke == b.stroke;
}

struct Vertex {
  Vec position, normal;
};
struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<std::array<std::uint16_t, 3>> triangles;
};

// A sampled landscape is solid below its surface. Sphere CSG makes the field
// volumetric: subsequent edits can create cavities, arches and overhangs.
struct DensityCache;
constexpr int DensitySide = Side + 2;
using DensityHalo = std::array<float, DensitySide * DensitySide>;
float CachedDensity(const DensityCache &, Vec local);
float DensityBottom(const DensityCache &);
float DensityTop(const DensityCache &);
struct Volume {
  Vec origin;
  std::array<float, Side * Side> heights{};
  std::vector<Brush> edits;
  float bottom{}, top{};
  std::shared_ptr<const DensityHalo> densityHeights;
  std::shared_ptr<const DensityCache> density;

  bool Valid() const {
    if (!Finite(origin) || std::abs(origin.x) > 16777216 ||
        std::abs(origin.y) > 16777216 || origin.z != 0 ||
        !std::isfinite(bottom) || !std::isfinite(top) || bottom < -1048576 ||
        top > 1048576 || top <= bottom || top - bottom > 16384 ||
        std::fmod(top - bottom, Step) != 0 || edits.size() > MaxEdits)
      return false;
    for (auto h : heights)
      if (!std::isfinite(h) || h < bottom + Step || h > top - Step)
        return false;
    for (const auto &e : edits)
      if (!Finite(e.center) || !std::isfinite(e.radius) || e.radius < 16.F ||
          e.radius > 512 || !std::isfinite(e.strength) || e.strength < 0 ||
          (!density && !BrushTouchesCell(e, origin)) || e.center.z < bottom ||
          e.center.z > top)
        return false;
    return true;
  }
  void SetBounds() {
    const auto [lo, hi] = std::minmax_element(heights.begin(), heights.end());
    bottom = std::floor((*lo - 512) / Step) * Step;
    top = std::ceil((*hi + 512) / Step) * Step;
    for (const auto &edit : edits) {
      bottom = std::min(
          bottom,
          std::floor((edit.center.z - edit.radius - 2 * Step) / Step) * Step);
      top = std::max(
          top,
          std::ceil((edit.center.z + edit.radius + 2 * Step) / Step) * Step);
    }
    if (density) {
      bottom = std::min(bottom, DensityBottom(*density));
      top = std::max(top, DensityTop(*density));
    }
    if (!Valid())
      throw std::runtime_error("Landscape exceeds terrain volume limits");
  }
  float Height(float x, float y) const {
    x = std::clamp((x - origin.x) / Step, 0.F, float(Side - 1));
    y = std::clamp((y - origin.y) / Step, 0.F, float(Side - 1));
    const int ix = std::min(int(x), Side - 2), iy = std::min(int(y), Side - 2);
    const float tx = x - ix, ty = y - iy;
    return std::lerp(
        std::lerp(heights[iy * Side + ix], heights[iy * Side + ix + 1], tx),
        std::lerp(heights[(iy + 1) * Side + ix],
                  heights[(iy + 1) * Side + ix + 1], tx),
        ty);
  }
  float Density(Vec p) const {
    return density ? CachedDensity(*density, p - origin)
                   : DensityAtHeight(p, Height(p.x, p.y));
  }
  float DensityAtHeight(Vec p, float height) const {
    if (density)
      return CachedDensity(*density, p - origin);
    // Truncate distances so a brush cannot perturb interpolation in distant
    // chunks. The zero surface is retained, with bounded remeshing support.
    float d = std::clamp(p.z - height, -Step, Step);
    for (const auto &e : edits) {
      const auto delta = p - e.center;
      // CSG can only change d when distance < radius +/- d. Use a
      // conservative guard to skip expensive square roots for exact no-ops.
      const auto reach = e.radius + (e.add ? d : -d) + 1;
      if (reach <= 0 || Dot(delta, delta) > reach * reach)
        continue;
      const float sphere = Length(delta) - e.radius;
      const float next = e.add ? std::min(d, sphere) : std::max(d, -sphere);
      d = e.strength > 0 ? std::clamp(next, d - e.strength, d + e.strength)
                         : next;
    }
    return d;
  }
  Vec Normal(Vec p) const {
    constexpr float h = 1;
    return Unit({Density(p + Vec{h, 0, 0}) - Density(p - Vec{h, 0, 0}),
                 Density(p + Vec{0, h, 0}) - Density(p - Vec{0, h, 0}),
                 Density(p + Vec{0, 0, h}) - Density(p - Vec{0, 0, h})});
  }
};

// Exact query-domain pruning. Every CSG step keeps density inside +/- max
// (Step, brush radii). Beyond radius+that bound a sphere cannot change min/max,
// regardless of strength, operation order, or query Z. Include the normal halo.
inline Volume LocalizedVolume(const Volume &source, Vec lo, Vec hi) {
  if (source.density)
    return source;
  float bound = Step;
  for (const auto &brush : source.edits)
    bound = std::max(bound, brush.radius);
  auto local = source;
  std::erase_if(local.edits, [&](const Brush &brush) {
    const Vec nearest{std::clamp(brush.center.x, lo.x, hi.x),
                      std::clamp(brush.center.y, lo.y, hi.y),
                      std::clamp(brush.center.z, lo.z, hi.z)};
    const auto delta = nearest - brush.center;
    const auto reach =
        brush.radius + bound + 1; // Conservative floating-point guard.
    return Dot(delta, delta) > reach * reach;
  });
  return local;
}

// Published MC33 implementation; see TerrainMesher.cpp and
// extern/mc33/UPSTREAM.md.
Mesh Polygonize(const Volume &volume, int startX, int startY, int width = 16);
} // namespace SkyBuild::Terrain

#include "terrain/TerrainDensity.h"
