#pragma once

namespace SkyBuild::Terrain::Grass::RendererCapture {
void Initialize(RE::BSNonReentrantSpinLock *groupLock);
bool UsesInPlace();
void Clear();
} // namespace SkyBuild::Terrain::Grass::RendererCapture
