#pragma once
namespace SkyBuild::Rendering { class OverlayBackend; }
#include "terrain/TerrainVolume.h"
#include "terrain/brush-overlay/BrushOverlayTraceState.h"
#include <span>
namespace SkyBuild::Terrain::BrushOverlay {

// Game thread owns capture; the render thread receives immutable CPU snapshots.
void Begin(const Brush &brush);
void ExpectChunk(std::uint64_t key);
void AddOriginal(RE::NiAVObject *root);
void Publish();
void Clear();
void Render(Rendering::OverlayBackend &backend);
} // namespace SkyBuild::Terrain::BrushOverlay
