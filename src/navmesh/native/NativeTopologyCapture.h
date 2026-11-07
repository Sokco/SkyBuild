#pragma once
#include "navmesh/generation/AuthoredTopology.h"
namespace SkyBuild::Navmesh {
// Caller owns a ready native search pause and publication lock. Copies native
// metadata into pointer-free data; does not mutate or retain engine records.
[[nodiscard]] AuthoredMesh CaptureNativeTopology(const RE::BSNavmesh &mesh,
                                                 std::uint32_t formID);
} // namespace SkyBuild::Navmesh
