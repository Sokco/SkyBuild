#pragma once
#include <array>
#include <cstddef>

namespace SkyBuild::Terrain {
// One bounded collision stage per cell. Brush input waits for the complete
// visual/collision cycle; there is no FIFO of brush pulses.
struct CollisionQueue {
  std::array<bool, 16> dirty{}, changed{};
  unsigned cursor{};
  bool Pending() const {
    for (bool value : dirty)
      if (value)
        return true;
    return false;
  }
  void Queue(unsigned index) { dirty[index] = changed[index] = true; }
  int Next() const {
    for (unsigned i = 0; i < dirty.size(); ++i) {
      const auto index = (cursor + i) % dirty.size();
      if (dirty[index])
        return static_cast<int>(index);
    }
    return -1;
  }
  void Complete(unsigned index) {
    dirty[index] = false;
    cursor = (index + 1) % dirty.size();
  }
  void Published() { changed.fill(false); }
};
} // namespace SkyBuild::Terrain
