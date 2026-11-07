#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace SkyBuild::Navmesh {
// cos(55 degrees), shared by geometry and generated-triangle checks.
inline constexpr float MIN_WALKABLE_NORMAL_Z = .573576436F;
struct Point {
  float x{}, y{}, z{};
  bool operator==(const Point &) const = default;
};
struct TriangleSource {
  std::uint32_t formID{};
  std::uint32_t triangle{0xFFFFFFFFU};
  bool operator==(const TriangleSource &) const = default;
  [[nodiscard]] bool Authored() const { return triangle != 0xFFFFFFFFU; }
};
struct Triangle {
  std::array<std::uint32_t, 3> vertices{};
  std::array<std::int32_t, 3> neighbors{-1, -1, -1};
  std::uint16_t flags{};
  // Retained through clipping and seam stitching for per-form portal remapping.
  // Sampled replacement surfaces have no authored source.
  TriangleSource source;
  bool operator==(const Triangle &) const = default;
};
struct Mesh {
  std::vector<Point> vertices;
  std::vector<Triangle> triangles;
  bool operator==(const Mesh &) const = default;
};
struct Baseline {
  std::uint32_t formID{};
  Mesh mesh;
};
struct RecastSceneGeometry;
struct BuildInput {
  struct Bounds {
    float left{}, bottom{}, right{}, top{};
  };
  std::uint64_t revision{};
  std::shared_ptr<const RecastSceneGeometry> sceneGeometry;
  // Exterior cell ownership clips sampled geometry, never authored originals.
  std::optional<Bounds> sampledBounds;
  // Nonoverlapping replacement regions planned from object and terrain bounds.
  std::vector<Bounds> footprints;
  std::vector<std::shared_ptr<const Baseline>> originals;
};
struct FootprintPlan {
  std::vector<BuildInput::Bounds> regions;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
[[nodiscard]] FootprintPlan
PlanFootprints(const std::vector<BuildInput::Bounds> &objects,
               const std::optional<BuildInput::Bounds> &cell, float spacing,
               std::size_t maxQuads = 65536);
[[nodiscard]] std::optional<Point>
BaselineTriangleSurface(const Mesh &mesh, const Triangle &face, float x,
                        float y, float low, float high);
[[nodiscard]] std::optional<Point>
BaselineSurface(const std::vector<std::shared_ptr<const Baseline>> &originals,
                float x, float y, float low, float high);
struct BuildResult {
  std::uint64_t revision{};
  std::shared_ptr<const Mesh> sourceGeometry;
  // Recast already validates source slopes and voxel step connectivity.
  // Preserve its tessellation without applying ramp limits to stair flights.
  bool validatedWalkability{};
  Mesh mesh;
  std::string error;
  bool cancelled{};
  std::size_t pendingNativeForms{};
  [[nodiscard]] bool Valid() const { return !cancelled && error.empty(); }
};
// Recast produces replacements inside footprint bounds.
// Baseline clipping, source provenance and seam repair stay in this pipeline.
using ReplacementGenerator = std::function<BuildResult(
    const BuildInput &, const std::function<bool()> &)>;
[[nodiscard]] BuildResult
Build(const BuildInput &input, const std::function<bool()> &cancelled = {},
      const ReplacementGenerator &generator = {},
      const std::function<void(unsigned)> &progress = {});
// Detached replay/repair of sampled-to-authored boundary seams. Retains source
// provenance and applies the same slope/adjacency validation as generation.
[[nodiscard]] BuildResult
RepairSampledSeams(const Mesh &mesh,
                   const std::function<bool()> &cancelled = {},
                   const std::vector<Point> &constraints = {});
// Only open generated edges receive cuts; existing vertex identities and
// interior surfaces remain unchanged during cached cell seam refinement.
[[nodiscard]] BuildResult
RefineGeneratedBoundaries(const Mesh &mesh,
                          const std::vector<Point> &constraints);
} // namespace SkyBuild::Navmesh
