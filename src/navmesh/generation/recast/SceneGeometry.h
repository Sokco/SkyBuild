#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include "terrain/TerrainNavigation.h"
namespace SkyBuild::Navmesh {
struct GeometryTransform {
  std::array<Point, 3> columns{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  Point translation{};
  [[nodiscard]] Point Apply(Point p) const;
};
struct CompressedChunk {
  Point offset;
  std::vector<std::uint16_t> vertices, indices, strips;
  std::uint16_t reference{0xFFFF}, transform{0xFFFF};
};
struct CompressedGeometry {
  // Skyrim instance scale applies after the compressed chunk/QS transform.
  Point scale{1, 1, 1};
  float error{};
  std::vector<GeometryTransform> transforms;
  std::vector<CompressedChunk> chunks;
  std::vector<Point> bigVertices;
  struct BigTriangle {
    std::array<std::uint16_t, 3> indices;
    std::uint16_t transform{0xFFFF};
  };
  std::vector<BigTriangle> bigTriangles;
};
struct CollisionGeometry {
  enum class Kind { Compressed, Box, Convex, Sphere, Capsule } kind{};
  GeometryTransform world;
  CompressedGeometry compressed;
  std::vector<Point> vertices;
  std::uint32_t sourceFormID{};
  Point halfExtents{}, endA{}, endB{};
  float radius{};
};
// Value-only snapshots. No Havok objects, references or scene nodes reach
// workers.
struct RecastSceneGeometry {
  std::size_t capturedBytes{};
  std::vector<Terrain::NavigationCell> terrain;
  std::vector<CollisionGeometry> collisions;
  std::vector<std::shared_ptr<const Baseline>> ground;
};
[[nodiscard]] BuildResult
DecodeSceneGeometry(const RecastSceneGeometry &scene, const BuildInput &input,
                    const std::function<bool()> &cancelled = {});
} // namespace SkyBuild::Navmesh
