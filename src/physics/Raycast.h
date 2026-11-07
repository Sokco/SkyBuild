#pragma once

#include <vector>
#include <functional>

namespace SkyBuild::Physics {
struct RaycastResult {
  bool didHit{false};

  float distance{0.0F};
  float fraction{1.0F};

  RE::NiPoint3 position{};
  RE::hkVector4 normal{};

  RE::COL_LAYER layer{RE::COL_LAYER::kUnidentified};

  RE::NiPointer<RE::TESObjectREFR> hitReference{};

  // Borrowed identity for immediate hit classification; never retain
  // across a world update or a cell transition.
  const RE::hkpCollidable *collidable{};
  std::optional<std::array<RE::NiPoint3, 8>> selectionBox;
  bool hasRigidBody{};
};

using RaycastResults = std::vector<RaycastResult>;

// Object selection only: repair compound-body ownership and include
// helper-volume filtering and visual-bounds fallback when no body is hit.
RaycastResults
CastSelectionRay(float distance,
                 const std::function<bool(RE::TESObjectREFR *)> &eligible);

// All SkyBuild physics rays ignore actors (including corpses/ragdolls).
// a_resolveReferences controls returned reference ownership, not actor
// filtering.

[[nodiscard]]
RaycastResults CastRay(const RE::NiPoint3 &a_start,
                       const RE::NiPoint3 &a_direction, float a_maxDistance,
                       RE::COL_LAYER a_queryLayer = RE::COL_LAYER::kProjectile,
                       RE::Actor *a_actor = nullptr,
                       bool a_resolveReferences = true);

[[nodiscard]]
RaycastResults
CastCameraRay(float a_maxDistance,
              RE::COL_LAYER a_queryLayer = RE::COL_LAYER::kProjectile,
              bool a_resolveReferences = true);
} // namespace SkyBuild::Physics
