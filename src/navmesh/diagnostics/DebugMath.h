#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace SkyBuild::Navmesh::DebugMath {
// Native cuts retain their covered source triangles for undo. They are
// overlap-marked, rather than necessarily deleted, and are not active faces.
inline bool VisibleTriangle(std::uint16_t flags) {
  return (flags & (8U | 32U)) == 0;
}
struct ScreenPoint {
  float x{}, y{};
};
inline Point Mix(Point a, Point b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
inline bool ClipNear(Point &a, Point &b, const std::array<float, 4> &depth,
                     float nearPlane) {
  const auto distance = [&](Point p) {
    return p.x * depth[0] + p.y * depth[1] + p.z * depth[2] + depth[3];
  };
  const float first = distance(a), second = distance(b);
  if (!std::isfinite(first) || !std::isfinite(second) ||
      !std::isfinite(nearPlane) || nearPlane <= 0)
    return false;
  if (first < nearPlane && second < nearPlane)
    return false;
  if (first < nearPlane)
    a = Mix(a, b, (nearPlane - first) / (second - first));
  else if (second < nearPlane)
    b = Mix(a, b, (nearPlane - first) / (second - first));
  return true;
}
inline bool ClipViewport(ScreenPoint &a, ScreenPoint &b) {
  if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
      !std::isfinite(b.y))
    return false;
  const float dx = b.x - a.x, dy = b.y - a.y;
  float first = 0, last = 1;
  const auto clip = [&](float p, float q) {
    if (std::abs(p) < .000001F)
      return q >= 0;
    const float t = q / p;
    if (p < 0) {
      if (t > last)
        return false;
      if (t > first)
        first = t;
    } else {
      if (t < first)
        return false;
      if (t < last)
        last = t;
    }
    return true;
  };
  if (!clip(-dx, a.x) || !clip(dx, 1 - a.x) || !clip(-dy, a.y) ||
      !clip(dy, 1 - a.y))
    return false;
  b = {a.x + dx * last, a.y + dy * last};
  a = {a.x + dx * first, a.y + dy * first};
  return true;
}
inline bool InRange(const std::array<Point, 3> &triangle, Point origin,
                    float radius) {
  float distance = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const auto coordinate = [axis](Point p) {
      return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
    };
    float low = coordinate(triangle[0]), high = low;
    for (auto p : triangle) {
      low = std::min(low, coordinate(p));
      high = std::max(high, coordinate(p));
    }
    const float value = coordinate(origin), delta = value < low ? low - value
                                                    : value > high
                                                        ? value - high
                                                        : 0;
    distance += delta * delta;
  }
  return distance <= radius * radius;
}
} // namespace SkyBuild::Navmesh::DebugMath
