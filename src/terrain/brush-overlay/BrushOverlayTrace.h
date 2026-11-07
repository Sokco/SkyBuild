#pragma once
#include "terrain/brush-overlay/BrushOverlayTraceState.h"
#include "terrain/brush-overlay/SceneFrameSnapshot.h"
#include <span>
struct ID3D11Buffer;
struct ID3D11DeviceContext;
namespace SkyBuild::Terrain {
class TerrainSurface;
}
namespace SkyBuild::Terrain::BrushOverlay::Trace {
void Initialize(ID3D11DeviceContext *context);
void Tag(ID3D11Buffer *buffer, Stamp stamp,
         std::shared_ptr<const TerrainSurface> surface);
SceneFrameBinding SceneFrame();
void Capture(bool enabled);
void CaptureScene(bool enabled); // Shared scene matrices for object previews.
std::vector<DrawnPayload<TerrainSurface>>
Select(std::span<const std::uint64_t> keys);
void FrameBoundary();
} // namespace SkyBuild::Terrain::BrushOverlay::Trace
