#pragma once
namespace SkyBuild::Navmesh {
// 93329/93311 emit borrowed BSNavmesh pointers during direct cut operations.
// 30612 subsequently converts them to BSNavmeshInfo pointers before queueing
// its task. At that later boundary use NativeDerivedWork::Edge instead; equal
// record sizes do not make the pointer domains interchangeable.
struct NativeCutEdge {
  RE::BSNavmesh *mesh{};
  std::uint16_t triangle{};
  std::array<std::byte, 6> padding{};
  std::uint32_t edge{};
  std::array<std::byte, 4> tail{};
};
static_assert(sizeof(NativeCutEdge) == 24);
static_assert(offsetof(NativeCutEdge, triangle) == 8);
static_assert(offsetof(NativeCutEdge, edge) == 16);
struct NativeCutEffects {
  RE::BSTArray<NativeCutEdge> first, second;
  [[nodiscard]] bool Empty() const { return first.empty() && second.empty(); }
};
static_assert(sizeof(NativeCutEffects) == 48);
static_assert(offsetof(NativeCutEffects, second) == 24);
} // namespace SkyBuild::Navmesh
