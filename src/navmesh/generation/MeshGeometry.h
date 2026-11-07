#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
namespace SkyBuild::Navmesh::Geometry {
inline constexpr float WELD_TOLERANCE = .01F;
inline constexpr float MAX_STEP = 32.0F;
inline constexpr float MIN_NORMAL_Z = MIN_WALKABLE_NORMAL_Z;
inline bool Walkable(Point a, Point b, Point c, bool validated) {
  const Point u{b.x - a.x, b.y - a.y, b.z - a.z},
      v{c.x - a.x, c.y - a.y, c.z - a.z};
  const Point n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                u.x * v.y - u.y * v.x};
  const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  return length >= .0001F &&
         (validated ? n.z > 0 : n.z / length >= MIN_NORMAL_Z);
}
using Polygon = std::vector<Point>;
inline bool HasArea(const Polygon &polygon) {
  if (polygon.size() < 3)
    return false;
  double area = 0;
  const auto origin = polygon.front();
  for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
    area += (double(polygon[i].x) - origin.x) *
                (double(polygon[i + 1].y) - origin.y) -
            (double(polygon[i].y) - origin.y) *
                (double(polygon[i + 1].x) - origin.x);
  return std::abs(area) > .0001;
}
inline Point Mix(Point a, Point b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
inline Polygon Clip(const Polygon &polygon, int axis, float boundary,
                    bool positive) {
  Polygon output;
  if (polygon.empty())
    return output;
  const auto coordinate = [axis](Point p) { return axis == 0 ? p.x : p.y; };
  Point previous = polygon.back();
  bool prior = positive ? coordinate(previous) >= boundary
                        : coordinate(previous) <= boundary;
  for (const auto current : polygon) {
    const bool inside = positive ? coordinate(current) >= boundary
                                 : coordinate(current) <= boundary;
    if (inside != prior) {
      const float delta = coordinate(current) - coordinate(previous);
      if (std::abs(delta) > .000001F)
        output.push_back(
            Mix(previous, current, (boundary - coordinate(previous)) / delta));
    }
    if (inside)
      output.push_back(current);
    previous = current;
    prior = inside;
  }
  return output;
}
// Remove only the sampled rectangle; interpolation retains the original surface
// height outside it. The input baseline and its topology are never edited.
inline std::vector<Polygon>
Subtract(Polygon polygon, const BuildInput::Bounds &rectangle,
         const std::optional<BuildInput::Bounds> &bounds) {
  float left = rectangle.left, bottom = rectangle.bottom;
  float right = rectangle.right, top = rectangle.top;
  if (bounds) {
    left = std::max(left, bounds->left);
    bottom = std::max(bottom, bounds->bottom);
    right = std::min(right, bounds->right);
    top = std::min(top, bounds->top);
    if (left >= right || bottom >= top)
      return {std::move(polygon)};
  }
  std::vector<Polygon> outside;
  const std::array<std::tuple<int, float, bool>, 4> boundaries{
      {{0, left, true}, {0, right, false}, {1, bottom, true}, {1, top, false}}};
  // A supporting line may cross a face even when the rectangle does not.
  // Preserve such faces verbatim instead of leaving artificial fragments
  // behind after a later clipping plane proves there was nothing to remove.
  auto overlap = polygon;
  for (const auto &[axis, coordinate, positive] : boundaries) {
    overlap = Clip(overlap, axis, coordinate, positive);
    if (!HasArea(overlap))
      return {std::move(polygon)};
  }
  for (const auto &[axis, coordinate, positive] : boundaries) {
    auto piece = Clip(polygon, axis, coordinate, !positive);
    if (HasArea(piece))
      outside.push_back(std::move(piece));
    polygon = Clip(polygon, axis, coordinate, positive);
    if (!HasArea(polygon))
      break;
  }
  return outside;
}
struct Writer {
  Mesh mesh;
  std::map<std::tuple<long long, long long, long long>,
           std::vector<std::uint32_t>>
      vertices;
  bool invalid{};
  bool limitsExceeded{};
  bool validatedWalkability{};
  std::set<std::uint32_t> boundaryConstraints;
  std::uint32_t Add(Point point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z) || std::abs(point.x) > 1.0e7F ||
        std::abs(point.y) > 1.0e7F || std::abs(point.z) > 1.0e7F) {
      invalid = true;
      return 0;
    }
    const auto x = std::llround(double(point.x) / WELD_TOLERANCE),
               y = std::llround(double(point.y) / WELD_TOLERANCE),
               z = std::llround(double(point.z) / WELD_TOLERANCE);
    const auto key = std::tuple{x, y, z};
    // Most triangle corners reuse an exactly identical vertex. Keep that
    // common path cheap; only new/nearby points need the neighboring search.
    if (const auto bucket = vertices.find(key); bucket != vertices.end())
      for (const auto index : bucket->second) {
        const auto p = mesh.vertices[index];
        if (p.x == point.x && p.y == point.y && p.z == point.z)
          return index;
      }
    std::optional<std::uint32_t> nearest;
    double distance = double(WELD_TOLERANCE) * WELD_TOLERANCE;
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          const auto bucket = vertices.find({x + dx, y + dy, z + dz});
          if (bucket == vertices.end())
            continue;
          for (const auto index : bucket->second) {
            const auto p = mesh.vertices[index];
            const double a = double(p.x) - point.x, b = double(p.y) - point.y,
                         c = double(p.z) - point.z;
            const auto squared = a * a + b * b + c * c;
            if (squared <= distance &&
                (!nearest || squared < distance || index < *nearest)) {
              nearest = index;
              distance = squared;
            }
          }
        }
    if (nearest)
      return *nearest;
    if (mesh.vertices.size() >= 65535) {
      invalid = true;
      limitsExceeded = true;
      return 0;
    }
    const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
    vertices[key].push_back(index);
    mesh.vertices.push_back(point);
    return index;
  }
  void TriangleAt(Point a, Point b, Point c, std::uint16_t flags,
                  bool checkSlope, TriangleSource source = {}) {
    const Point u{b.x - a.x, b.y - a.y, b.z - a.z},
        v{c.x - a.x, c.y - a.y, c.z - a.z};
    const Point n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z,
                  u.x * v.y - u.y * v.x};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < .0001F)
      return;
    if (checkSlope && !Walkable(a, b, c, validatedWalkability))
      return;
    const std::array ids{Add(a), Add(b), Add(c)};
    if (invalid || ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
      return;
    if (mesh.triangles.size() >= 65535) {
      invalid = true;
      limitsExceeded = true;
      return;
    }
    mesh.triangles.push_back(
        {ids, {-1, -1, -1}, static_cast<std::uint16_t>(flags & ~7U), source});
  }
};
} // namespace SkyBuild::Navmesh::Geometry
