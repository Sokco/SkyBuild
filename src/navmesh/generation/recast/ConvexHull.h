#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace SkyBuild::Navmesh {
// Incremental 3D convex hull: initialize a tetrahedron, remove faces visible
// from each exterior vertex, and connect that vertex to the visible horizon.
// Havok's optional plane cache is not required to describe the vertex hull.
template <class Point>
std::vector<std::array<std::size_t, 3>>
ConvexCollisionHull(const std::vector<Point> &input) {
  struct V {
    double x, y, z;
    V operator-(V b) const { return {x - b.x, y - b.y, z - b.z}; }
  };
  const auto dot = [](V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
  const auto cross = [](V a, V b) {
    return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x};
  };
  if (input.size() < 4 || input.size() > 4096)
    throw std::runtime_error("Invalid convex collision vertex count");
  std::vector<V> points;
  for (const auto &p : input) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      throw std::runtime_error("Non-finite convex collision vertex");
    points.push_back({double(p.x) - input[0].x, double(p.y) - input[0].y,
                      double(p.z) - input[0].z});
  }
  std::size_t b = 0, c = 0, d = 0;
  double longest = 0;
  for (std::size_t i = 1; i < points.size(); ++i)
    if (const auto length = dot(points[i], points[i]); length > longest) {
      longest = length;
      b = i;
    }
  const double span = std::sqrt(longest), epsilon = std::max(1e-9, span * 1e-7);
  if (span <= epsilon)
    throw std::runtime_error(
        "Degenerate convex collision: coincident vertices");
  double widest = 0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto normal = cross(points[b], points[i]);
    const double distance = dot(normal, normal) / longest;
    if (distance > widest) {
      widest = distance;
      c = i;
    }
  }
  if (widest <= epsilon * epsilon)
    throw std::runtime_error("Degenerate convex collision: collinear vertices");
  const auto initialNormal = cross(points[b], points[c]);
  const double normalLength = std::sqrt(dot(initialNormal, initialNormal));
  double deepest = 0;
  for (std::size_t i = 0; i < points.size(); ++i)
    if (const double distance =
            std::abs(dot(initialNormal, points[i])) / normalLength;
        distance > deepest) {
      deepest = distance;
      d = i;
    }
  if (deepest <= epsilon)
    throw std::runtime_error("Degenerate convex collision: coplanar vertices");
  const V inside{(points[b].x + points[c].x + points[d].x) / 4,
                 (points[b].y + points[c].y + points[d].y) / 4,
                 (points[b].z + points[c].z + points[d].z) / 4};
  struct Face {
    std::array<std::size_t, 3> indices;
    V normal;
  };
  const auto face = [&](std::size_t x, std::size_t y, std::size_t z) {
    auto normal = cross(points[y] - points[x], points[z] - points[x]);
    const double length = std::sqrt(dot(normal, normal));
    if (length <= epsilon * epsilon)
      throw std::runtime_error("Degenerate convex collision hull face");
    if (dot(normal, inside - points[x]) > 0) {
      std::swap(y, z);
      normal = {-normal.x, -normal.y, -normal.z};
    }
    return Face{{x, y, z},
                {normal.x / length, normal.y / length, normal.z / length}};
  };
  std::vector<Face> faces{face(0, b, c), face(0, d, b), face(0, c, d),
                          face(b, d, c)};
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (i == 0 || i == b || i == c || i == d)
      continue;
    bool duplicate = false;
    for (std::size_t j = 0; j < i; ++j)
      if (points[i].x == points[j].x && points[i].y == points[j].y &&
          points[i].z == points[j].z) {
        duplicate = true;
        break;
      }
    if (duplicate)
      continue;
    struct Edge {
      std::size_t a, b;
      unsigned count = 0;
    };
    std::map<std::pair<std::size_t, std::size_t>, Edge> edges;
    std::vector<Face> retained;
    bool outside = false;
    for (const auto &f : faces) {
      // Visibility is a topological predicate, not a distance acceptance
      // test. Keeping a slightly visible adjacent face can disconnect the
      // horizon on near-coplanar inputs and exclude previously added points.
      if (dot(f.normal, points[i] - points[f.indices[0]]) <= 0) {
        retained.push_back(f);
        continue;
      }
      outside = true;
      for (unsigned j = 0; j < 3; ++j) {
        const auto x = f.indices[j], y = f.indices[(j + 1) % 3];
        auto &e = edges[{std::min(x, y), std::max(x, y)}];
        e.a = x;
        e.b = y;
        ++e.count;
        if (e.count > 2)
          throw std::runtime_error("Non-manifold convex collision horizon");
      }
    }
    if (!outside)
      continue;
    for (const auto &[key, e] : edges)
      if (e.count == 1)
        retained.push_back(face(e.a, e.b, i));
    faces = std::move(retained);
  }
  std::vector<std::array<std::size_t, 3>> result;
  std::map<std::pair<std::size_t, std::size_t>, unsigned> edges;
  for (const auto &f : faces) {
    for (const auto p : points)
      if (dot(f.normal, p - points[f.indices[0]]) > epsilon * 4)
        throw std::runtime_error(
            "Convex collision hull failed containment validation");
    result.push_back(f.indices);
    for (unsigned j = 0; j < 3; ++j) {
      const auto x = f.indices[j], y = f.indices[(j + 1) % 3];
      ++edges[{std::min(x, y), std::max(x, y)}];
    }
  }
  for (const auto &[edge, count] : edges)
    if (count != 2)
      throw std::runtime_error("Convex collision hull is not closed");
  return result;
}
} // namespace SkyBuild::Navmesh
