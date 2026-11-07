#pragma once
#include "terrain/LandscapeSampling.h"
#include "terrain/brush-overlay/BrushOverlayTraceState.h"

namespace SkyBuild::Terrain {
class TerrainSurface;
struct TerrainTextures {
  RE::NiPointer<RE::BSLightingShaderProperty> property;
  LandscapeSurface surface;
  Vec renderOrigin;
};
TerrainTextures CaptureTerrainTextures(RE::BSTriShape *geometry);
void InitializeTerrainWeldingPolicy();
class NativeChunk {
public:
  static std::shared_ptr<NativeChunk> Create(const Mesh &mesh, Vec origin,
                                             const TerrainTextures &textures);
  static std::shared_ptr<NativeChunk>
  CreateCollision(const Mesh &mesh, Vec origin, RE::bhkRigidBody *sourceBody);
  ~NativeChunk();
  void StampOverlay(std::uint32_t cell, unsigned chunk,
                    std::shared_ptr<const TerrainSurface> surface);
  RE::NiNode *Root() const;
  RE::bhkRigidBody *Body() const;
  void Attach(RE::NiNode *parent, RE::bhkWorld *world, Vec origin);
  void Detach();

private:
  NativeChunk();
  struct Data;
  std::unique_ptr<Data> data;
};
} // namespace SkyBuild::Terrain
