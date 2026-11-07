#pragma once
#include "navmesh/generation/recast/SceneGeometry.h"
namespace SkyBuild::Navmesh {
// Game-thread snapshot under the physics world's read lock. Unsupported
// locomotion collision rejects capture rather than substituting render bounds.
[[nodiscard]] bool
CaptureRecastCollision(RE::TESObjectREFR *reference, RecastSceneGeometry &scene,
                       std::string &error,
                       const RE::hkpCollidable *selectedCollider = nullptr);
} // namespace SkyBuild::Navmesh
