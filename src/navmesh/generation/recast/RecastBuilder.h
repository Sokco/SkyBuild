#pragma once
#include "navmesh/generation/MeshBuilder.h"
namespace SkyBuild::Navmesh {
inline constexpr float RECAST_CELL_SIZE = 8.0F;
inline constexpr int RECAST_RADIUS_CELLS = 2;
inline constexpr int RECAST_BORDER_CELLS = RECAST_RADIUS_CELLS + 1;
inline constexpr float RECAST_CONTOUR_ERROR_CELLS = 1.0F;
// Border/erosion plus one contour-error cell: the simplified raster boundary
// must remain outside the actual replacement footprint.
inline constexpr float RECAST_CONTEXT_HALO =
    (RECAST_BORDER_CELLS + RECAST_CONTOUR_ERROR_CELLS) * RECAST_CELL_SIZE;
[[nodiscard]] BuildResult
GenerateRecast(const BuildInput &input,
               const std::function<bool()> &cancelled = {});
// Rasterize owned collision polygons and original walkable context. Skyrim
// retains responsibility for pathfinding and all runtime navigation.
[[nodiscard]] BuildResult
BuildRecast(const BuildInput &input,
            const std::function<bool()> &cancelled = {});
[[nodiscard]] BuildResult
RecastSampledSurface(const Mesh &mesh,
                     const std::function<bool()> &cancelled = {});
} // namespace SkyBuild::Navmesh
