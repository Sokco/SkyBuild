#pragma once
#include "terrain/TerrainVolume.h"
#include <bit>

namespace SkyBuild::Terrain {
constexpr std::uint64_t VertexDescriptor = (std::uint64_t(0x41B) << 44) |
                                           (6ULL << 20) | (5ULL << 16) |
                                           (4ULL << 8) | 7ULL;
inline std::uint16_t Half(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  const auto sign = (bits >> 16) & 0x8000;
  const int exponent = int((bits >> 23) & 255) - 112;
  if (exponent <= 0)
    return static_cast<std::uint16_t>(sign);
  if (exponent >= 31)
    return static_cast<std::uint16_t>(sign | 0x7BFF);
  return static_cast<std::uint16_t>(sign | (std::uint32_t(exponent) << 10) |
                                    ((bits >> 13) & 1023));
}
inline std::uint8_t PackedNormal(float value) {
  return static_cast<std::uint8_t>(
      std::clamp(std::lround((value + 1) * 127.5F), 0L, 255L));
}
struct PackedVertex {
  Vec position;
  float bitangentX;
  std::array<std::uint16_t, 2> uv;
  std::array<std::uint8_t, 4> normal, tangent;
};
static_assert(sizeof(PackedVertex) == 28);
struct Buffers {
  std::vector<PackedVertex> vertices;
  std::vector<std::array<std::uint16_t, 3>> triangles;
  Vec center;
  float radius{};
};
inline Buffers PackBuffers(const Mesh &mesh, Vec origin) {
  if (!Finite(origin) || mesh.vertices.empty() || mesh.triangles.empty() ||
      mesh.vertices.size() > 60000 || mesh.triangles.size() > 60000)
    throw std::runtime_error("Invalid terrain buffer counts");
  Buffers result;
  auto lo = mesh.vertices.front().position - origin, hi = lo;
  for (const auto &vertex : mesh.vertices) {
    if (!Finite(vertex.position) || !Finite(vertex.normal))
      throw std::runtime_error("Invalid terrain vertex");
    const auto p = vertex.position - origin;
    const auto n = Unit(vertex.normal);
    const auto t =
        Unit(Cross(std::abs(n.z) < .99F ? Vec{0, 0, 1} : Vec{0, 1, 0}, n));
    const auto b = Cross(n, t);
    result.vertices.push_back({p,
                               b.x,
                               {Half(p.x / 512), Half(p.y / 512)},
                               {PackedNormal(n.x), PackedNormal(n.y),
                                PackedNormal(n.z), PackedNormal(b.y)},
                               {PackedNormal(t.x), PackedNormal(t.y),
                                PackedNormal(t.z), PackedNormal(b.z)}});
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  for (const auto &triangle : mesh.triangles) {
    for (auto index : triangle)
      if (index >= mesh.vertices.size())
        throw std::runtime_error("Invalid terrain index");
    result.triangles.push_back(triangle);
  }
  result.center = (lo + hi) * .5F;
  result.radius = Length(hi - lo) * .5F;
  return result;
}

using ChunkMask = std::array<bool, 16>;
inline ChunkMask DirtyChunks(const Volume *before, const Volume &after) {
  ChunkMask dirty{};
  if (!before || before->heights != after.heights ||
      before->bottom != after.bottom || before->top != after.top ||
      Length(before->origin - after.origin) != 0) {
    dirty.fill(true);
    return dirty;
  }
  if (before->density && after.density) {
    if (before->density->current == after.density->current)
      return dirty;
    const auto mark = [&](DensityNode node) {
      const int nx = int(node.xy % DensitySide) - 1,
                ny = int(node.xy / DensitySide) - 1;
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
          if (nx >= x * 16 - 1 && nx <= x * 16 + 17 && ny >= y * 16 - 1 &&
              ny <= y * 16 + 17)
            dirty[y * 4 + x] = true;
    };
    if (after.density->from == before->density->current) {
      for (const auto &node : after.density->changed)
        mark(node);
    } else {
      for (int z = after.density->current->low;
           z <= after.density->current->high; ++z)
        for (unsigned xy = 0; xy < DensitySide * DensitySide; ++xy)
          if (before->density->current->At({z, xy}) !=
              after.density->current->At({z, xy}))
            mark({z, xy});
    }
    return dirty;
  }
  std::size_t common{};
  while (common < before->edits.size() && common < after.edits.size()) {
    const auto &a = before->edits[common], &b = after.edits[common];
    if (Length(a.center - b.center) != 0 || a.radius != b.radius ||
        a.add != b.add || a.strength != b.strength || a.stroke != b.stroke)
      break;
    ++common;
  }
  const auto include = [&](const Brush &brush) {
    // Truncated field and finite-difference normals have bounded support.
    const float radius = brush.radius + 2 * Step;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        const float left = after.origin.x + x * 1024,
                    bottom = after.origin.y + y * 1024;
        if (brush.center.x + radius >= left &&
            brush.center.x - radius <= left + 1024 &&
            brush.center.y + radius >= bottom &&
            brush.center.y - radius <= bottom + 1024)
          dirty[y * 4 + x] = true;
      }
  };
  for (auto i = common; i < before->edits.size(); ++i)
    include(before->edits[i]);
  for (auto i = common; i < after.edits.size(); ++i)
    include(after.edits[i]);
  return dirty;
}
} // namespace SkyBuild::Terrain
