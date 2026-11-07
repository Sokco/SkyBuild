#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <set>

namespace SkyBuild::Navmesh {
// Attachment notifications can precede cell readiness. Coalesce bursts and
// allow a short retry window, without polling historical caches while idle.
class AttachmentRestoreSchedule {
public:
  using Clock = std::chrono::steady_clock;
  void Wake(std::uint32_t cell) {
    if (cell)
      pending[cell] = 4;
  }
  bool Due(Clock::time_point now) const {
    return !pending.empty() && now >= next;
  }
  std::set<std::uint32_t> Cells() const {
    std::set<std::uint32_t> cells;
    for (const auto &[cell, attempts] : pending)
      cells.insert(cell);
    return cells;
  }
  void Scanned(Clock::time_point now) {
    for (auto it = pending.begin(); it != pending.end();) {
      if (--it->second == 0)
        it = pending.erase(it);
      else
        ++it;
    }
    next = now + std::chrono::milliseconds(250);
  }

private:
  std::map<std::uint32_t, unsigned> pending;
  Clock::time_point next{};
};
} // namespace SkyBuild::Navmesh
