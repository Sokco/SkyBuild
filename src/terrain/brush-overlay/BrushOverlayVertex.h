#pragma once
#include "terrain/TerrainVolume.h"
namespace SkyBuild::Terrain::BrushOverlay {
// Projection and brush masking share the unmodified surface position.
using OverlayVertex = Vec;
inline OverlayVertex SurfaceVertex(Vec surface) { return surface; }
} // namespace SkyBuild::Terrain::BrushOverlay
