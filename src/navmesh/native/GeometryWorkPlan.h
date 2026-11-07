#pragma once
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
namespace SkyBuild::Navmesh {
// Geometry-derived work is version-specific. Registration retries are NOT
// supplied here: they express persistent intent and must not be discarded.
template <class Record, class Identity>
std::vector<Record>
PreserveOutsideGeometryWork(std::span<const Record> records,
                            const std::set<Identity> &changed) {
  std::vector<Record> kept;
  kept.reserve(records.size());
  for (const auto &record : records) {
    if (!record.info)
      throw std::runtime_error("Geometry work has a null navigation identity");
    if (!changed.contains(record.info))
      kept.push_back(record); // Outside identities remain opaque.
  }
  return kept;
}
} // namespace SkyBuild::Navmesh
