#pragma once
namespace SkyBuild::Navmesh {
// Conservative native search bounds. Optional reduced surface arrays stay
// empty; the engine's point/distance queries then use their AABB fallback.
[[nodiscard]] std::array<float, 6> BuildNativeBounds(RE::BSNavmeshInfo &info,
                                                     const RE::BSNavmesh &mesh);
[[nodiscard]] std::string
ValidateNativeBounds(const RE::BSNavmeshInfo &info, const RE::BSNavmesh &mesh,
                     const std::array<float, 6> &expected);
} // namespace SkyBuild::Navmesh
