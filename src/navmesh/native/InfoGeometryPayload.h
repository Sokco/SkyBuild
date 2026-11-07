#pragma once
#include <utility>
namespace SkyBuild::Navmesh {
// Derived geometry ownership only. Identity, parent ownership and the bound
// mesh stay with their info. Both array pointers move with their counts.
// 30362's empty-geometry bit and 91226's compressed-bounds bit follow geometry;
// all other flags remain properties of the receiving native identity.
inline constexpr unsigned INFO_GEOMETRY_FLAGS = 0x28;
template <class Info, class Visit>
void VisitInfoGeometryPayload(Info &a, Info &b, Visit &&visit) {
  visit(a.approxLocation, b.approxLocation);
  visit(a.connectedDoorsArray, b.connectedDoorsArray);
  visit(a.adjacentMeshesArray, b.adjacentMeshesArray);
  visit(a.connectedDoorCount, b.connectedDoorCount);
  visit(a.adjacentMeshCount, b.adjacentMeshCount);
  visit(a.preferredPercent, b.preferredPercent);
  visit(a.bounds, b.bounds);
  visit(a.preferredStartIndex, b.preferredStartIndex);
}
template <class Info>
void ExchangeInfoGeometryPayload(Info &a, Info &b) noexcept {
  VisitInfoGeometryPayload(a, b, [](auto &left, auto &right) {
    using std::swap;
    swap(left, right);
  });
  const auto differing = (a.uiFlags ^ b.uiFlags) & INFO_GEOMETRY_FLAGS;
  a.uiFlags ^= differing;
  b.uiFlags ^= differing;
}
} // namespace SkyBuild::Navmesh
