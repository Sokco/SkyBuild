#pragma once
#include <set>
namespace SkyBuild::Navmesh {
// Read-only snapshot of derived geometry requests. Caller retains a ready
// search pause and publication lock. Records borrow infos, not native owners.
class NativeDerivedWork {
public:
  NativeDerivedWork();
  // Admission only: inspect queue headers under native locks without owning
  // a navigation pause, so Skyrim can finish its pending cut transactions.
  [[nodiscard]] static bool QueuesIdle() noexcept;
  NativeDerivedWork(const NativeDerivedWork &) = delete;
  NativeDerivedWork &operator=(const NativeDerivedWork &) = delete;
  [[nodiscard]] bool Unchanged() const;
  // Final retirement only, under the same pause/write lock as capture.
  std::size_t Retire(const std::set<const RE::BSNavmeshInfo *> &infos);
  [[nodiscard]] std::set<RE::BSNavmeshInfo *> BorrowedInfos() const;
  // Value layouts verified in the 1170 consumer. Public for owned evidence and
  // adapter tests; native collection addresses remain private.
  struct Triangle {
    RE::BSNavmeshInfo *info{};
    std::uint16_t triangle{};
    std::array<std::byte, 6> padding{};
  };
  struct Edge {
    RE::BSNavmeshInfo *info{};
    std::uint16_t triangle{};
    std::array<std::byte, 6> padding{};
    std::uint32_t edge{};
    std::array<std::byte, 4> tail{};
  };
  static_assert(sizeof(Triangle) == 16 && sizeof(Edge) == 24);

private:
  RE::BSTArray<Triangle> *triangles{};
  RE::BSTArray<Edge> *edges{};
  std::vector<Triangle> beforeTriangles;
  std::vector<Edge> beforeEdges;
  const void *triangleAllocation{}, *edgeAllocation{};
  std::uint32_t triangleCapacity{}, edgeCapacity{};
};
} // namespace SkyBuild::Navmesh
