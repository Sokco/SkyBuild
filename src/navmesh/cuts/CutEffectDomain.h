#pragma once
#include <cstdint>
#include <optional>
namespace SkyBuild::Navmesh {
enum class CutEffectDomain { Invalid, BeforeOnly, AfterOnly, Both };
// Effects describe an operation transition, not necessarily surviving geometry.
// A missing bound means the mesh was outside the retained operation cohort;
// zero is a known empty mesh. No earlier generation's maximum is accepted.
inline CutEffectDomain ClassifyCutEffect(std::uint32_t triangle,
                                         std::uint32_t edge,
                                         std::optional<std::uint32_t> before,
                                         std::optional<std::uint32_t> after) {
  if (edge >= 3 || !before || !after)
    return CutEffectDomain::Invalid;
  const bool inBefore = triangle < *before, inAfter = triangle < *after;
  if (inBefore && inAfter)
    return CutEffectDomain::Both;
  if (inBefore)
    return CutEffectDomain::BeforeOnly;
  if (inAfter)
    return CutEffectDomain::AfterOnly;
  return CutEffectDomain::Invalid;
}
} // namespace SkyBuild::Navmesh
