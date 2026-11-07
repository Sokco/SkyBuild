#pragma once
#include <array>
#include <optional>
#include <vector>
namespace SkyBuild::Physics {
struct PlacementContact {
  RE::NiPoint3 normal;
  float depth;
};
struct PlacementShape {
  RE::NiPointer<RE::bhkRigidBody> owner;
  RE::NiTransform local;
};
using PlacementShapes = std::vector<PlacementShape>;
PlacementShapes CapturePlacementShapes(RE::NiAVObject *model);
bool CanQueryShapes(const PlacementShapes &shapes, float scale);
std::optional<std::array<RE::NiPoint3, 2>>
PlacementShapeBounds(const PlacementShapes &shapes);
std::vector<PlacementContact> ShapeOverlaps(RE::bhkWorld *world,
                                            const PlacementShapes &shapes,
                                            const RE::NiPoint3 &position,
                                            const RE::NiMatrix3 &rotation,
                                            RE::TESObjectREFR *ignore);
// Cast the complete collider assembly from a clear pose. Returns the earliest
// hit fraction (1 for no hit), or no value if its starting pose is obstructed.
std::optional<float> ShapeCast(RE::bhkWorld *world,
                               const PlacementShapes &shapes,
                               const RE::NiPoint3 &from, const RE::NiPoint3 &to,
                               const RE::NiMatrix3 &rotation,
                               RE::TESObjectREFR *ignore);
// Temporary native box query. Nothing is inserted into or retained by the
// world.
std::vector<PlacementContact> BoxOverlaps(RE::bhkWorld *world,
                                          const RE::NiPoint3 &center,
                                          const RE::NiPoint3 &halfExtents,
                                          const RE::NiMatrix3 &rotation,
                                          RE::TESObjectREFR *ignore);
} // namespace SkyBuild::Physics
