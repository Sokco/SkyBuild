#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace SkyBuild::Terrain::Grass {
struct GrassBounds {
  float left, bottom, right, top;
  bool Overlaps(const GrassBounds &b) const {
    return left <= b.right && right >= b.left && bottom <= b.top &&
           top >= b.bottom;
  }
  void Include(const GrassBounds &b) {
    left = std::min(left, b.left);
    bottom = std::min(bottom, b.bottom);
    right = std::max(right, b.right);
    top = std::max(top, b.top);
  }
};
// A short, bounded journal lets infrequently drawn buffers catch up safely.
// Falling behind the journal or switching worlds deliberately forces refresh.
struct SpatialChanges {
  struct Change {
    std::uint64_t revision;
    std::optional<GrassBounds> area;
    bool full;
  };
  std::deque<Change> journal;
  void Add(std::uint64_t revision, std::optional<GrassBounds> area, bool full) {
    journal.push_back({revision, area, full});
    if (journal.size() > 32)
      journal.pop_front();
  }
  bool AffectsSince(std::uint64_t revision, const GrassBounds &area) const {
    if (!revision || journal.empty() || revision < journal.front().revision - 1)
      return true;
    for (const auto &change : journal)
      if (change.revision > revision &&
          (change.full || (change.area && change.area->Overlaps(area))))
        return true;
    return false;
  }
};
} // namespace SkyBuild::Terrain::Grass
