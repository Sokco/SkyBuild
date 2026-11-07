#include "terrain/LandscapeGeometry.h"
#include <cstring>
namespace SkyBuild::Terrain {
const std::uint16_t *
LandscapeIndices(RE::BSTriShape *geometry,
                 const RE::BSGraphics::TriShape *renderer) {
  if (renderer->rawIndexData)
    return renderer->rawIndexData;
  const auto &counts = geometry->GetTrishapeRuntimeData();
  const REL::Relocation<std::uintptr_t *> sharedBuffer{REL::ID(400927)};
  const REL::Relocation<const std::uint16_t **> sharedIndices{REL::ID(400928)};
  const auto wrapper = *sharedBuffer;
  if (counts.vertexCount != 289 || counts.triangleCount != 512 || !wrapper ||
      *reinterpret_cast<void **>(wrapper) !=
          reinterpret_cast<void *>(renderer->indexBuffer) ||
      !*sharedIndices)
    throw std::runtime_error(
        "Original landscape shared index identity mismatch");
  return *sharedIndices;
}
LandscapeGeometry CaptureLandscapeGeometry(RE::BSTriShape *geometry) {
  if (!geometry)
    throw std::runtime_error("Missing landscape geometry");
  const auto renderer = static_cast<RE::BSGraphics::TriShape *>(
      geometry->GetGeometryRuntimeData().rendererData);
  const auto &counts = geometry->GetTrishapeRuntimeData();
  if (!renderer || !renderer->rawVertexData || !counts.vertexCount ||
      counts.vertexCount > 4096 || !counts.triangleCount ||
      counts.triangleCount > 60000)
    throw std::runtime_error("Landscape CPU geometry unavailable");
  std::uint64_t bits{};
  std::memcpy(&bits, &renderer->vertexDesc, sizeof(bits));
  const unsigned stride = unsigned(bits & 15) * 4;
  if (!renderer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ||
      stride < 16)
    throw std::runtime_error("Unsupported landscape position descriptor");
  const auto &transform = geometry->world;
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      if (std::abs(transform.rotate.entry[y][x] - (x == y ? 1.F : 0.F)) > 1e-5F)
        throw std::runtime_error("Unsupported rotated landscape geometry");
  if (std::abs(transform.scale - 1) > 1e-5F)
    throw std::runtime_error("Unsupported scaled landscape geometry");
  const Vec origin{transform.translate.x, transform.translate.y,
                   transform.translate.z};
  LandscapeGeometry result;
  result.vertices.resize(counts.vertexCount);
  for (unsigned i = 0; i < counts.vertexCount; ++i) {
    std::memcpy(&result.vertices[i], renderer->rawVertexData + i * stride,
                sizeof(Vec));
    result.vertices[i] = result.vertices[i] + origin;
    if (!Finite(result.vertices[i]))
      throw std::runtime_error("Invalid landscape position");
  }
  result.triangles.resize(counts.triangleCount);
  std::memcpy(result.triangles.data(), LandscapeIndices(geometry, renderer),
              result.triangles.size() * sizeof(result.triangles.front()));
  for (const auto &t : result.triangles)
    for (auto i : t)
      if (i >= result.vertices.size())
        throw std::runtime_error("Invalid landscape triangle index");
  return result;
}
} // namespace SkyBuild::Terrain
