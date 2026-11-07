#pragma once
#include "navmesh/publication/NativeFormOwnership.h"
#include <set>
namespace SkyBuild::Navmesh {
// Synchronous, thread-local query overlay. Owns scratch lists; never changes
// TESObjectCELL::navMeshes. Outside cells are excluded; a missing or aliased
// declared cell remains an explicit cohort coverage failure.
class NativeCellQueryView {
public:
  // Native path searches may encounter pinned, unbound predecessors. Give
  // each search an owned eligible-only snapshot without editing live lists.
  NativeCellQueryView();
  static void Initialize();
  explicit NativeCellQueryView(
      const std::map<RE::TESObjectCELL *, RE::NavMeshArray> &cells);
  ~NativeCellQueryView();
  NativeCellQueryView(const NativeCellQueryView &) = delete;
  NativeCellQueryView &operator=(const NativeCellQueryView &) = delete;
  [[nodiscard]] std::size_t Hits() const { return hits; }
  [[nodiscard]] std::size_t Misses() const { return misses; }
  [[nodiscard]] std::size_t CohortMisses() const { return cohortMisses; }
  [[nodiscard]] std::size_t OutsideQueries() const {
    return misses - cohortMisses;
  }

private:
  static RE::NavMeshArray *Lookup(RE::TESObjectCELL *cell);
  std::map<RE::TESObjectCELL *, RE::NavMeshArray> cells;
  std::set<RE::FormID> cellIDs;
  std::size_t hits{}, misses{}, cohortMisses{};
  bool pathQuery{};
  NativeCellQueryView *previous{};
};
} // namespace SkyBuild::Navmesh
