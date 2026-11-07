#pragma once
#include "RE/C/CollisionLayers.h"

namespace SkyBuild::Physics {
// Helper volumes do not represent a surface the player can select. This
// filter is deliberately separate from placement and terrain physics rays.
inline bool IgnoreSelectionLayer(RE::COL_LAYER layer) {
  switch (layer) {
  case RE::COL_LAYER::kTrigger:
  case RE::COL_LAYER::kNonCollidable:
  case RE::COL_LAYER::kPortal:
  case RE::COL_LAYER::kAcousticSpace:
  case RE::COL_LAYER::kActorZone:
  case RE::COL_LAYER::kProjectileZone:
  case RE::COL_LAYER::kInvisibleWall:
  case RE::COL_LAYER::kStairHelper:
  case RE::COL_LAYER::kAvoidBox:
  case RE::COL_LAYER::kCameraSphere:
  case RE::COL_LAYER::kDoorDetection:
    return true;
  default:
    return false;
  }
}
} // namespace SkyBuild::Physics
