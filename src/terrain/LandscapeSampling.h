#pragma once
#include "terrain/LandscapeProjection.h"
#include "terrain/TerrainBuffers.h"
#include <span>

namespace SkyBuild::Terrain {
struct LandscapeSample {
  Vec position;
  std::array<float, 2> uv{};
  std::array<float, 4> color{};
  std::array<float, 8> blend{};
  Vec tangent{1, 0, 0}, bitangent{0, 1, 0};
};
// Snapshot the original triangles, rather than assuming a LAND diagonal or
// reconstructing its sequential texture-layer blending from record data.
struct LandscapeSurface {
  std::vector<LandscapeSample> vertices;
  std::vector<std::array<std::uint16_t, 3>> triangles;
  Vec low{}, high{};
  std::array<std::vector<std::size_t>, 256> bins;
  int Bin(float value, float first, float last) const {
    return std::clamp(int((value - first) * 16 / (last - first)), 0, 15);
  }
  bool Contains(Vec p) const {
    return p.x >= low.x - .01F && p.x <= high.x + .01F && p.y >= low.y - .01F &&
           p.y <= high.y + .01F;
  }
  void Prepare() {
    if (vertices.empty() || triangles.empty())
      throw std::runtime_error("Empty landscape texture surface");
    low = high = vertices.front().position;
    for (const auto &v : vertices) {
      if (!Finite(v.position))
        throw std::runtime_error("Invalid landscape texture position");
      low.x = std::min(low.x, v.position.x);
      low.y = std::min(low.y, v.position.y);
      high.x = std::max(high.x, v.position.x);
      high.y = std::max(high.y, v.position.y);
    }
    if (high.x <= low.x || high.y <= low.y)
      throw std::runtime_error("Degenerate landscape texture bounds");
    for (auto &bin : bins)
      bin.clear();
    for (std::size_t i = 0; i < triangles.size(); ++i) {
      auto lo = high, hi = low;
      for (auto index : triangles[i]) {
        if (index >= vertices.size())
          throw std::runtime_error("Invalid landscape texture index");
        const auto p = vertices[index].position;
        lo.x = std::min(lo.x, p.x);
        lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x);
        hi.y = std::max(hi.y, p.y);
      }
      for (int y = Bin(lo.y, low.y, high.y); y <= Bin(hi.y, low.y, high.y); ++y)
        for (int x = Bin(lo.x, low.x, high.x); x <= Bin(hi.x, low.x, high.x);
             ++x)
          bins[y * 16 + x].push_back(i);
    }
  }
  LandscapeSample Sample(Vec p) const {
    if (!Contains(p))
      throw std::runtime_error("Terrain vertex outside source landscape");
    for (auto index :
         bins[Bin(p.y, low.y, high.y) * 16 + Bin(p.x, low.x, high.x)]) {
      const auto &tri = triangles[index];
      const auto &a = vertices[tri[0]], &b = vertices[tri[1]],
                 &c = vertices[tri[2]];
      const auto ab = b.position - a.position, ac = c.position - a.position,
                 ap = p - a.position;
      const float det = ab.x * ac.y - ab.y * ac.x;
      if (std::abs(det) < 1e-6F)
        continue;
      float v = (ap.x * ac.y - ap.y * ac.x) / det;
      float w = (ab.x * ap.y - ab.y * ap.x) / det;
      float u = 1 - v - w;
      if (std::min({u, v, w}) < -1e-5F)
        continue;
      LandscapeSample result;
      result.position = p;
      const auto interpolate = [&](auto &out, const auto &aa, const auto &bb,
                                   const auto &cc) {
        for (std::size_t i = 0; i < out.size(); ++i)
          out[i] = u * aa[i] + v * bb[i] + w * cc[i];
      };
      interpolate(result.uv, a.uv, b.uv, c.uv);
      interpolate(result.color, a.color, b.color, c.color);
      interpolate(result.blend, a.blend, b.blend, c.blend);
      const float du1 = b.uv[0] - a.uv[0], dv1 = b.uv[1] - a.uv[1],
                  du2 = c.uv[0] - a.uv[0], dv2 = c.uv[1] - a.uv[1];
      const float uvDet = du1 * dv2 - du2 * dv1;
      if (std::abs(uvDet) < 1e-8F)
        throw std::runtime_error("Degenerate landscape UV triangle");
      const Vec flatAB{ab.x, ab.y, 0}, flatAC{ac.x, ac.y, 0};
      result.tangent = (flatAB * dv2 - flatAC * dv1) * (1 / uvDet);
      result.bitangent = (flatAC * du1 - flatAB * du2) * (1 / uvDet);
      return result;
    }
    throw std::runtime_error(
        "No original landscape triangle covers terrain vertex");
  }
};
inline float Unhalf(std::uint16_t value) {
  const float sign = value & 0x8000 ? -1.F : 1.F;
  const int exponent = (value >> 10) & 31;
  if (exponent == 31)
    throw std::runtime_error("Non-finite landscape UV");
  return sign * std::ldexp(float((value & 1023) + (exponent ? 1024 : 0)),
                           exponent ? exponent - 25 : -24);
}
struct LandscapeVertex {
  PackedVertex base;
  std::array<std::uint8_t, 4> color;
  std::array<std::uint8_t, 8> blend;
};
static_assert(sizeof(LandscapeVertex) == 40);
constexpr std::uint64_t LandscapeVertexDescriptor =
    (std::uint64_t(0xBB) << 44) | (8ULL << 32) | (7ULL << 24) | (6ULL << 20) |
    (5ULL << 16) | (4ULL << 8) | 10ULL;
inline std::uint8_t Unorm(float v) {
  return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255), 0L, 255L));
}
inline std::vector<LandscapeVertex>
PackLandscape(const Mesh &mesh, const Buffers &packed,
              const LandscapeSurface &source,
              std::span<const LandscapeAxis> axes = {}) {
  if (!axes.empty() && axes.size() != mesh.vertices.size())
    throw std::runtime_error("Invalid landscape projection count");
  std::vector<LandscapeVertex> result;
  result.reserve(mesh.vertices.size());
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    const auto sample = source.Sample(mesh.vertices[i].position);
    LandscapeVertex vertex{packed.vertices[i], {}, {}};
    vertex.base.uv = {Half(sample.uv[0]), Half(sample.uv[1])};
    const auto n = Unit(mesh.vertices[i].normal);
    // UVs are a function of world XY. Lift dP/du onto the surface while
    // keeping its XY components parallel to the original constant-V line.
    // Merely projecting a horizontal U vector onto the normal plane changes
    // its XY direction on diagonal slopes, and therefore changes V as well.
    // n.z*dP/du = (n.z*dx/du,n.z*dy/du,-dot(n.xy,dXY/du)).
    // The scale avoids division near vertical normals; orient it toward +U.
    auto t = Vec{n.z * sample.tangent.x, n.z * sample.tangent.y,
                 -Dot(n, sample.tangent)};
    const bool regularUV = std::abs(n.z) > 1e-5F;
    if (regularUV && n.z < 0)
      t = t * -1;
    // At a vertical surface XY-projected UVs have a singular Jacobian. Keep
    // the finite fallback there; tangent changes cannot repair UV collapse.
    if (!regularUV)
      t = sample.tangent - n * Dot(n, sample.tangent);
    if (Length(t) < 1e-5F)
      t = Cross(sample.bitangent, n);
    t = Unit(t);
    const float handedness =
        (Cross(sample.tangent, sample.bitangent).z < 0 ? -1.F : 1.F) *
        (regularUV && n.z < 0 ? -1.F : 1.F);
    auto b = Cross(n, t) * handedness;
    if (!axes.empty() && axes[i] != LandscapeAxis::Top) {
      // Invert the original XY mapping to retain its texture scale and phase.
      // Replace the facing axis with world Z, giving X faces YZ coordinates
      // and Y faces XZ coordinates. World anchoring avoids chunk-local seams.
      const auto p = mesh.vertices[i].position;
      const auto &u = sample.tangent;
      const auto &v = sample.bitangent;
      const float det = u.x * v.y - u.y * v.x;
      if (std::abs(det) < 1e-8F)
        throw std::runtime_error("Degenerate landscape projection basis");
      Vec gu{v.y / det, -v.x / det, 0};
      Vec gv{-u.y / det, u.x / det, 0};
      auto uv = sample.uv;
      if (axes[i] == LandscapeAxis::X) {
        uv[0] += gu.x * (p.z - p.x);
        uv[1] += gv.x * (p.z - p.x);
        gu = {0, gu.y, gu.x};
        gv = {0, gv.y, gv.x};
      } else {
        uv[0] += gu.y * (p.z - p.y);
        uv[1] += gv.y * (p.z - p.y);
        gu = {gu.x, 0, gu.y};
        gv = {gv.x, 0, gv.y};
      }
      vertex.base.uv = {Half(uv[0]), Half(uv[1])};
      t = Cross(gv, n);
      if (Dot(t, gu) < 0)
        t = t * -1;
      if (Length(t) < 1e-5F)
        t = gu - n * Dot(gu, n);
      t = Unit(t);
      b = Cross(n, t) * (Dot(Cross(gu, gv), n) < 0 ? -1.F : 1.F);
    }
    // Lighting.hlsl reconstructs the U axis from POSITION.w, NORMAL.w,
    // and BINORMAL.w; BINORMAL.xyz supplies the V axis. The native field
    // names are misleading: packing these in the opposite order swaps the
    // camera's U/V parallax components and the sampled normal-map axes.
    vertex.base.bitangentX = t.x;
    vertex.base.normal = {PackedNormal(n.x), PackedNormal(n.y),
                          PackedNormal(n.z), PackedNormal(t.y)};
    vertex.base.tangent = {PackedNormal(b.x), PackedNormal(b.y),
                           PackedNormal(b.z), PackedNormal(t.z)};
    // LAND shaders require COLOR0, but original painted shading should not
    // follow terrain that has been reshaped. White leaves lighting neutral.
    vertex.color.fill(255);
    for (std::size_t j = 0; j < 8; ++j)
      vertex.blend[j] = Unorm(sample.blend[j]);
    result.push_back(vertex);
  }
  return result;
}
} // namespace SkyBuild::Terrain
