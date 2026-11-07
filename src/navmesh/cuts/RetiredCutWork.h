#pragma once
#include <cstddef>
namespace SkyBuild::Navmesh {
// Borrowed records only. Preserve native order, allocation and all unrelated
// work; callers supply identities proven permanently retired, never originals
// which may be restored. No info pointer is dereferenced.
template <class Array, class Predicate>
std::size_t RemoveRetiredCutWork(Array &records, Predicate retired) {
  const auto before = records.size();
  decltype(records.size()) kept{};
  for (decltype(records.size()) i{}; i < before; ++i)
    if (!retired(records[i])) {
      if (kept != i)
        records[kept] = records[i];
      ++kept;
    }
  if (kept != before)
    records.resize(kept);
  return before - kept;
}
} // namespace SkyBuild::Navmesh
