#include "navmesh/generation/recast/RecastBuilder.h"
#include "navmesh/generation/XYBoundsIndex.h"
#include "navmesh/generation/recast/SceneGeometry.h"
#include <Recast.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>
namespace SkyBuild::Navmesh {
namespace {
constexpr float CELL_SIZE = RECAST_CELL_SIZE, CELL_HEIGHT = 1;
constexpr int MAX_CELLS = 1024 * 1024;
class Context final : public rcContext {
public:
  std::string error;

protected:
  void doLog(rcLogCategory category, const char *message, int length) override {
    if (category == RC_LOG_ERROR)
      error.assign(message, length);
  }
};
struct Cancelled {};
} // namespace
BuildResult RecastSampledSurface(const Mesh &mesh,
                                 const std::function<bool()> &cancelled) {
  BuildResult result;
  result.validatedWalkability = true;
  const auto stop = [&] {
    if (cancelled && cancelled())
      throw Cancelled{};
  };
  Context context;
  const auto require = [&](bool ok, const char *stage) {
    stop();
    if (!ok)
      throw std::runtime_error(
          std::string("Recast ") + stage + " failed" +
          (context.error.empty() ? "" : ": " + context.error));
  };
  try {
    stop();
    if (mesh.triangles.empty())
      return result;
    if (mesh.vertices.size() > 1000000 || mesh.triangles.size() > 262144)
      throw std::runtime_error("Recast input exceeds native geometry limits");
    // Recast's float triangulation works in tile-local coordinates. Large
    // absolute world positions lose precision in detail circumcircle tests.
    // A raster-aligned origin preserves alignment between neighboring cells.
    const Point origin{
        std::floor(mesh.vertices.front().x / CELL_SIZE) * CELL_SIZE,
        std::floor(mesh.vertices.front().y / CELL_SIZE) * CELL_SIZE,
        std::floor(mesh.vertices.front().z)};
    std::vector<float> vertices;
    vertices.reserve(mesh.vertices.size() * 3);
    for (const auto p : mesh.vertices) {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        throw std::runtime_error("Non-finite Recast input vertex");
      // Right-handed Z-up Skyrim -> right-handed Y-up Recast.
      vertices.insert(vertices.end(),
                      {p.x - origin.x, p.z - origin.z, origin.y - p.y});
    }
    std::vector<int> triangles;
    std::vector<unsigned char> areas;
    std::vector<XYBounds> bounds;
    for (const auto &t : mesh.triangles) {
      stop();
      if (t.source.Authored())
        throw std::runtime_error("Recast may not remesh authored provenance");
      for (const auto v : t.vertices) {
        if (v >= mesh.vertices.size())
          throw std::runtime_error("Invalid Recast input triangle index");
        triangles.push_back(static_cast<int>(v));
      }
      areas.push_back(t.flags & 64U ? 2 : 1);
      const auto a = mesh.vertices[t.vertices[0]],
                 b = mesh.vertices[t.vertices[1]],
                 c = mesh.vertices[t.vertices[2]];
      bounds.push_back({std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}),
                        std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y})});
    }
    std::vector<unsigned char> walkable(mesh.triangles.size(), RC_NULL_AREA);
    rcMarkWalkableTriangles(
        &context, 55, vertices.data(), static_cast<int>(mesh.vertices.size()),
        triangles.data(), static_cast<int>(mesh.triangles.size()),
        walkable.data());
    for (std::size_t i = 0; i < areas.size(); ++i)
      if (walkable[i] == RC_NULL_AREA)
        areas[i] = RC_NULL_AREA;
    const XYBoundsIndex index(std::move(bounds));
    std::vector<std::uint32_t> candidates;
    const auto surface = [&](float x, float y,
                             float z) -> std::optional<Point> {
      if (!index.Query({x - .001F, y - .001F, x + .001F, y + .001F}, candidates,
                       [&] { return cancelled && cancelled(); }))
        throw Cancelled{};
      std::optional<Point> best;
      float distance =
          32; // same vertical seam tolerance as the existing generator
      for (const auto id : candidates) {
        if (areas[id] == RC_NULL_AREA)
          continue;
        const auto p = BaselineTriangleSurface(mesh, mesh.triangles[id], x, y,
                                               z - distance, z + distance);
        if (p && (!best || std::abs(p->z - z) < std::abs(best->z - z)))
          best = p;
      }
      return best;
    };
    rcConfig config{};
    config.cs = CELL_SIZE;
    config.ch = CELL_HEIGHT;
    config.walkableSlopeAngle = 55;
    config.walkableHeight = 128;
    config.walkableClimb = 32;
    config.walkableRadius = RECAST_RADIUS_CELLS;
    // Keep contour vertices required by shape/area changes, without imposing
    // a maximum edge length. Shared Build() adds connection-driven splits.
    config.maxEdgeLen = 0;
    config.maxSimplificationError = RECAST_CONTOUR_ERROR_CELLS;
    config.borderSize = RECAST_BORDER_CELLS;
    config.minRegionArea = 0; // retain small disconnected placed surfaces
    config.mergeRegionArea = 0;
    config.maxVertsPerPoly = 6;
    config.detailSampleDist = 6 * config.cs;
    config.detailSampleMaxError = 1;
    rcCalcBounds(vertices.data(), static_cast<int>(mesh.vertices.size()),
                 config.bmin, config.bmax);
    // World-aligned raster origin makes independently generated cells agree.
    config.bmin[0] = std::floor(config.bmin[0] / CELL_SIZE) * CELL_SIZE;
    config.bmin[2] = std::floor(config.bmin[2] / CELL_SIZE) * CELL_SIZE;
    config.bmax[0] = std::ceil(config.bmax[0] / CELL_SIZE) * CELL_SIZE;
    config.bmax[2] = std::ceil(config.bmax[2] / CELL_SIZE) * CELL_SIZE;
    config.bmin[1] = std::floor(config.bmin[1]) - 2;
    config.bmax[1] = std::ceil(config.bmax[1]) + 4;
    if (config.bmax[1] - config.bmin[1] >= RC_SPAN_MAX_HEIGHT)
      throw std::runtime_error(
          "Recast sampled height range exceeds voxel limits");
    rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width,
                   &config.height);
    if (config.width <= 0 || config.height <= 0 ||
        static_cast<std::int64_t>(config.width) * config.height > MAX_CELLS)
      throw std::runtime_error("Recast raster exceeds cell budget");
    const std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)> field(
        rcAllocHeightfield(), rcFreeHeightField);
    require(bool(field), "heightfield allocation");
    require(rcCreateHeightfield(&context, *field, config.width, config.height,
                                config.bmin, config.bmax, config.cs, config.ch),
            "heightfield");
    require(rcRasterizeTriangles(&context, vertices.data(),
                                 static_cast<int>(mesh.vertices.size()),
                                 triangles.data(), areas.data(),
                                 static_cast<int>(areas.size()), *field,
                                 config.walkableClimb),
            "rasterization");
    rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *field);
    rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb,
                       *field);
    rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *field);
    stop();
    const std::unique_ptr<rcCompactHeightfield,
                          decltype(&rcFreeCompactHeightfield)>
        compact(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);
    require(bool(compact), "compact allocation");
    require(rcBuildCompactHeightfield(&context, config.walkableHeight,
                                      config.walkableClimb, *field, *compact),
            "compact field");
    // Rasterization touches cells on both sides of an exact triangle edge.
    // Only retain spans whose center belongs to the supplied walkable surface;
    // holes and disjoint footprints must not grow into unsampled space.
    for (int z = 0; z < compact->height; ++z) {
      stop();
      for (int x = 0; x < compact->width; ++x) {
        const auto &cell = compact->cells[x + z * compact->width];
        for (unsigned i = cell.index; i < cell.index + cell.count; ++i)
          if (!surface(origin.x + compact->bmin[0] + (x + .5F) * CELL_SIZE,
                       origin.y - (compact->bmin[2] + (z + .5F) * CELL_SIZE),
                       origin.z + compact->bmin[1] +
                           compact->spans[i].y * CELL_HEIGHT))
            compact->areas[i] = RC_NULL_AREA;
      }
    }
    require(rcErodeWalkableArea(&context, config.walkableRadius, *compact),
            "agent clearance erosion");
    require(rcBuildRegionsMonotone(&context, *compact, config.borderSize, 0, 0),
            "regions");
    const std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)> contours(
        rcAllocContourSet(), rcFreeContourSet);
    require(bool(contours), "contour allocation");
    require(rcBuildContours(&context, *compact, config.maxSimplificationError,
                            config.maxEdgeLen, *contours,
                            RC_CONTOUR_TESS_WALL_EDGES |
                                RC_CONTOUR_TESS_AREA_EDGES),
            "contours");
    if (!contours->nconts)
      return result;
    const std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)> polygons(
        rcAllocPolyMesh(), rcFreePolyMesh);
    require(bool(polygons), "polygon allocation");
    require(
        rcBuildPolyMesh(&context, *contours, config.maxVertsPerPoly, *polygons),
        "polygons");
    const std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>
        detail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);
    require(bool(detail), "detail allocation");
    require(rcBuildPolyMeshDetail(&context, *polygons, *compact,
                                  config.detailSampleDist,
                                  config.detailSampleMaxError, *detail),
            "detail mesh");
    std::map<std::tuple<long long, long long, long long>, std::uint32_t> unique;
    const auto add = [&](const float *v) {
      const Point world{origin.x + v[0], origin.y - v[2], origin.z + v[1]};
      // Preserve Recast's voxel/detail surface. Per-vertex projection back to
      // separate treads/source faces creates discontinuities and folded edges.
      const auto p = world;
      const auto key =
          std::tuple{std::llround(p.x * 100), std::llround(p.y * 100),
                     std::llround(p.z * 100)};
      const auto [found, inserted] = unique.emplace(
          key, static_cast<std::uint32_t>(result.mesh.vertices.size()));
      if (inserted)
        result.mesh.vertices.push_back(p);
      return found->second;
    };
    for (int i = 0; i < detail->nmeshes; ++i) {
      stop();
      const auto *part = &detail->meshes[i * 4];
      for (unsigned j = 0; j < part[3]; ++j) {
        const auto *t = &detail->tris[(part[2] + j) * 4];
        const std::array ids{add(&detail->verts[(part[0] + t[0]) * 3]),
                             add(&detail->verts[(part[0] + t[1]) * 3]),
                             add(&detail->verts[(part[0] + t[2]) * 3])};
        // Recast detail triangles use the original right-handed winding.
        result.mesh.triangles.push_back(
            {ids,
             {-1, -1, -1},
             static_cast<std::uint16_t>(polygons->areas[i] == 2 ? 64 : 0),
             {}});
      }
    }
    if (result.mesh.vertices.size() > 65535 ||
        result.mesh.triangles.size() > 65535)
      throw std::runtime_error(
          "Recast output exceeds Skyrim 16-bit geometry limits");
  } catch (const Cancelled &) {
    result.cancelled = true;
    result.mesh = {};
  } catch (const std::exception &error) {
    result.error = error.what();
    result.mesh = {};
  }
  return result;
}
BuildResult GenerateRecast(const BuildInput &input,
                           const std::function<bool()> &cancelled) {
  if (cancelled && cancelled()) {
    BuildResult stopped;
    stopped.revision = input.revision;
    stopped.cancelled = true;
    return stopped;
  }
  if (input.footprints.empty()) {
    BuildResult empty;
    empty.revision = input.revision;
    empty.validatedWalkability = true;
    return empty;
  }
  if (!input.sceneGeometry) {
    BuildResult missing;
    missing.revision = input.revision;
    missing.error = "Recast requires a captured collision geometry scene";
    return missing;
  }
  auto source = DecodeSceneGeometry(*input.sceneGeometry, input, cancelled);
  if (!source.Valid())
    return source;
  auto result = RecastSampledSurface(source.mesh, cancelled);
  if (result.Valid())
    result.sourceGeometry = std::make_shared<Mesh>(std::move(source.mesh));
  result.revision = input.revision;
  return result;
}
BuildResult BuildRecast(const BuildInput &input,
                        const std::function<bool()> &cancelled) {
  return Build(input, cancelled, GenerateRecast);
}
} // namespace SkyBuild::Navmesh
