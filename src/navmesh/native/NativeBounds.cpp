#include "navmesh/native/NativeBounds.h"
#include <cmath>
#include <cstring>
#include <limits>
namespace SkyBuild::Navmesh {
namespace {
struct Uncompressed {
  std::array<float, 6> limits{};
  void *triangles{}, *vertices{};
  std::uint16_t triangleCount{}, vertexCount{};
  std::uint32_t padding{};
};
static_assert(sizeof(Uncompressed) == 0x30);
static_assert(offsetof(Uncompressed, triangles) == 0x18);
static_assert(offsetof(Uncompressed, vertices) == 0x20);
static_assert(offsetof(Uncompressed, triangleCount) == 0x28);
struct OwnedUncompressed {
  Uncompressed value;
  ~OwnedUncompressed() {
    using Destroy = void (*)(Uncompressed *);
    const REL::Relocation<Destroy> destroy{REL::RelocationID(88833, 91282)};
    destroy(&value);
  }
};
} // namespace
std::array<float, 6> BuildNativeBounds(RE::BSNavmeshInfo &info,
                                       const RE::BSNavmesh &mesh) {
  if (mesh.vertices.empty() || info.navMesh != &mesh)
    throw std::runtime_error("Missing native bounds geometry/binding");
  Uncompressed source;
  const auto largest = std::numeric_limits<float>::max();
  source.limits = {largest, largest, largest, -largest, -largest, -largest};
  for (const auto &vertex : mesh.vertices) {
    const float point[]{vertex.location.x, vertex.location.y,
                        vertex.location.z};
    for (unsigned axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(point[axis]))
        throw std::runtime_error("Non-finite bounds vertex");
      source.limits[axis] = std::min(source.limits[axis], point[axis]);
      source.limits[axis + 3] = std::max(source.limits[axis + 3], point[axis]);
    }
  }
  using Setup = void (*)(RE::BSNavmeshInfo *, const Uncompressed *);
  const REL::Relocation<Setup> setup{REL::RelocationID(88780, 91226)};
  setup(&info, &source);
  if (!info.bounds)
    throw std::runtime_error("Native bounds allocation failed");
  return source.limits;
}
std::string ValidateNativeBounds(const RE::BSNavmeshInfo &info,
                                 const RE::BSNavmesh &mesh,
                                 const std::array<float, 6> &expected) {
  if (!info.bounds)
    return "Missing native search bounds";
  OwnedUncompressed decoded;
  using Decode =
      void (*)(const RE::BSCompressedNavmeshBounds *, Uncompressed *);
  const REL::Relocation<Decode> decode{REL::RelocationID(88845, 91295)};
  decode(info.bounds, &decoded.value);
  if (decoded.value.limits != expected || decoded.value.triangles ||
      decoded.value.vertices || decoded.value.triangleCount ||
      decoded.value.vertexCount)
    return "Native conservative bounds payload mismatch";
  using Contains = bool (*)(const Uncompressed *, const RE::NiPoint3 *);
  const REL::Relocation<Contains> contains{REL::RelocationID(88837, 91287)};
  for (const auto &vertex : mesh.vertices)
    if (!contains(&decoded.value, &vertex.location))
      return "Native bounds exclude a mesh vertex";
  RE::NiPoint3 outside{
      std::nextafter(expected[3], std::numeric_limits<float>::infinity()),
      expected[1], expected[2]};
  if (contains(&decoded.value, &outside))
    return "Native bounds accept a point outside the X extent";
  return {};
}
} // namespace SkyBuild::Navmesh
