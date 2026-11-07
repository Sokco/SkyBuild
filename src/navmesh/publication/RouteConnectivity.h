#pragma once
namespace SkyBuild::Navmesh {
// Coarse routes survive streaming boundaries whose portal descriptors are
// retained, even while runtime edges are closed until neighbor attachment.
// Original directionality remains authoritative; an absent link is not a route.
constexpr bool CanRetainRouteLink(bool originalForward, bool originalBackward,
                                  bool replacementForward,
                                  bool replacementBackward,
                                  bool deferredBoundary) {
  return (originalForward || originalBackward) &&
         (!originalForward || replacementForward || deferredBoundary) &&
         (!originalBackward || replacementBackward || deferredBoundary);
}
} // namespace SkyBuild::Navmesh
