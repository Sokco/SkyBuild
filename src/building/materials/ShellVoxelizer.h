#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace SkyBuild::MaterialShell {
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
  std::uint32_t texture{};
  std::uint32_t mesh{};
};
struct Mesh {
  std::vector<std::string> textures;
  std::vector<Triangle> triangles;
};
struct TextureCount {
  std::string texture;
  std::uint64_t voxels{};
};
struct Result {
  std::vector<TextureCount> textures;
  std::uint64_t voxels{};
};
// Triangle/AABB SAT: three box axes, the triangle normal, nine edge cross axes.
inline bool Overlaps(const std::array<Point, 3> &triangle, Point center) {
  std::array<Point, 3> p{triangle[0] - center, triangle[1] - center,
                         triangle[2] - center};
  const std::array<Point, 3> edges{p[1] - p[0], p[2] - p[1], p[0] - p[2]};
  const auto separated = [&](Point axis) {
    const double radius =
        .5 * (std::abs(axis.x) + std::abs(axis.y) + std::abs(axis.z));
    const double a = Dot(p[0], axis), b = Dot(p[1], axis), c = Dot(p[2], axis);
    return std::min({a, b, c}) > radius + 1e-9 ||
           std::max({a, b, c}) < -radius - 1e-9;
  };
  for (Point axis : std::array<Point, 3>{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}) {
    if (separated(axis))
      return false;
    for (auto edge : edges)
      if (separated(Cross(edge, axis)))
        return false;
  }
  return !separated(Cross(edges[0], edges[1]));
}
// Closest point on the actual triangle, used only when textures share a voxel.
inline double DistanceSquared(Point p, const std::array<Point, 3> &t) {
  const auto a = t[0], b = t[1], c = t[2], ab = b - a, ac = c - a, ap = p - a;
  const auto d1 = Dot(ab, ap), d2 = Dot(ac, ap);
  Point q;
  if (d1 <= 0 && d2 <= 0)
    q = a;
  else {
    const auto bp = p - b;
    const auto d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
      q = b;
    else {
      const auto vc = d1 * d4 - d3 * d2;
      if (vc <= 0 && d1 >= 0 && d3 <= 0)
        q = a + ab * (d1 / (d1 - d3));
      else {
        const auto cp = p - c;
        const auto d5 = Dot(ab, cp), d6 = Dot(ac, cp);
        if (d6 >= 0 && d5 <= d6)
          q = c;
        else {
          const auto vb = d5 * d2 - d1 * d6;
          if (vb <= 0 && d2 >= 0 && d6 <= 0)
            q = a + ac * (d2 / (d2 - d6));
          else {
            const auto va = d3 * d6 - d5 * d4;
            if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
              q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
            else {
              const auto inv = 1. / (va + vb + vc);
              q = a + ab * (vb * inv) + ac * (vc * inv);
            }
          }
        }
      }
    }
  }
  const auto delta = p - q;
  return Dot(delta, delta);
}
inline Result Voxelize(const Mesh &mesh, const std::atomic_bool &cancel) {
  struct Cell {
    double distance;
    std::uint32_t texture;
  };
  std::unordered_map<std::uint64_t, Cell> cells;
  constexpr int bias = 1 << 20;
  const auto key = [](int x, int y, int z) {
    if (x < -bias || y < -bias || z < -bias || x >= bias || y >= bias ||
        z >= bias)
      throw std::runtime_error(
          "Mesh exceeds the one-unit shell coordinate range.");
    return (std::uint64_t(x + bias) << 42) | (std::uint64_t(y + bias) << 21) |
           std::uint64_t(z + bias);
  };
  std::uint64_t visits{};
  for (const auto &triangle : mesh.triangles) {
    if (cancel.load(std::memory_order_relaxed))
      throw std::runtime_error("Shell analysis cancelled.");
    if (triangle.texture >= mesh.textures.size())
      throw std::runtime_error("Invalid shell texture assignment.");
    const auto &p = triangle.points;
    for (const auto &v : p)
      for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(v[axis]) || std::abs(v[axis]) > bias - 2)
          throw std::runtime_error("Invalid shell vertex.");
    const auto normal = Cross(p[1] - p[0], p[2] - p[0]);
    if (Dot(normal, normal) < 1e-18)
      continue;
    int axis = 0;
    if (std::abs(normal.y) > std::abs(normal[axis]))
      axis = 1;
    if (std::abs(normal.z) > std::abs(normal[axis]))
      axis = 2;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    const double su = -normal[u] / normal[axis], sv = -normal[v] / normal[axis];
    const auto lo = [&](int a) {
      return std::min({p[0][a], p[1][a], p[2][a]});
    };
    const auto hi = [&](int a) {
      return std::max({p[0][a], p[1][a], p[2][a]});
    };
    const int firstU = int(std::ceil(lo(u) - .5)),
              lastU = int(std::floor(hi(u) + .5));
    const int firstV = int(std::ceil(lo(v) - .5)),
              lastV = int(std::floor(hi(v) + .5));
    // Dominant-axis projection bounds the third-axis candidates to a few cells
    // per column, even for diagonal triangles. The SAT rejects excess cells.
    for (int x = firstU; x <= lastU; ++x)
      for (int y = firstV; y <= lastV; ++y) {
        if ((++visits & 1023) == 0 && cancel.load(std::memory_order_relaxed))
          throw std::runtime_error("Shell analysis cancelled.");
        if (visits > 250000000)
          throw std::runtime_error(
              "One-unit shell analysis exceeds the work budget.");
        const double plane =
            p[0][axis] + su * (x - p[0][u]) + sv * (y - p[0][v]);
        const double radius = .5 * (std::abs(su) + std::abs(sv));
        const int first =
            int(std::ceil(std::max(lo(axis) - .5, plane - radius - .5)));
        const int last =
            int(std::floor(std::min(hi(axis) + .5, plane + radius + .5)));
        for (int z = first; z <= last; ++z) {
          std::array<int, 3> coordinate{};
          coordinate[u] = x;
          coordinate[v] = y;
          coordinate[axis] = z;
          const Point center{double(coordinate[0]), double(coordinate[1]),
                             double(coordinate[2])};
          if (!Overlaps(p, center))
            continue;
          const auto id = key(coordinate[0], coordinate[1], coordinate[2]);
          const auto distance = DistanceSquared(center, p);
          const auto [found, inserted] =
              cells.try_emplace(id, Cell{distance, triangle.texture});
          if (!inserted &&
              (distance < found->second.distance - 1e-9 ||
               (std::abs(distance - found->second.distance) <= 1e-9 &&
                mesh.textures[triangle.texture] <
                    mesh.textures[found->second.texture])))
            found->second = {distance, triangle.texture};
          if (cells.size() > 4000000)
            throw std::runtime_error(
                "One-unit shell exceeds four million surface voxels.");
        }
      }
  }
  if (cells.empty())
    throw std::runtime_error(
        "No triangle surfaces were available for shell analysis.");
  std::map<std::string, std::uint64_t> counts;
  for (const auto &[id, cell] : cells)
    ++counts[mesh.textures[cell.texture]];
  Result result;
  result.voxels = cells.size();
  for (const auto &[texture, count] : counts)
    result.textures.push_back({texture, count});
  return result;
}
} // namespace SkyBuild::MaterialShell
