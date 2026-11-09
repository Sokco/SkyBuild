#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SkyBuild::Models {
struct Point {
  double x{}, y{}, z{};
  double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
  Point operator+(Point b) const { return {x + b.x, y + b.y, z + b.z}; }
  Point operator-(Point b) const { return {x - b.x, y - b.y, z - b.z}; }
  Point operator*(double s) const { return {x * s, y * s, z * s}; }
};
inline double Dot(Point a, Point b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Point Cross(Point a, Point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
struct Triangle {
  std::array<Point, 3> points;
  std::uint32_t texture{}; // Index into Mesh::textures.
};
struct Mesh {
  std::vector<std::string> textures;
  std::vector<Triangle> triangles;
};
struct Bounds {
  std::array<float, 3> min, max;
};
struct TextureSwap {
  unsigned index;
  std::string shape, diffuse;
};
} // namespace SkyBuild::Models
