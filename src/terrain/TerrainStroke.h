#pragma once
#include "terrain/TerrainTiming.h"
#include "terrain/TerrainVolume.h"
#include <chrono>
namespace SkyBuild::Terrain {
struct RemovedBrush {
  std::size_t index;
  Brush brush;
  std::shared_ptr<const DensityStroke> density;
};
inline std::vector<RemovedBrush> RemoveStroke(Volume &volume,
                                              const Brush &selected) {
  std::vector<RemovedBrush> removed;
  if (volume.density) {
    const auto &strokes = volume.density->strokes;
    for (std::size_t i = strokes.size(); i > 0; --i) {
      const auto &marker = strokes[i - 1]->marker;
      if (selected.stroke ? marker.stroke == selected.stroke
                          : SameBrush(marker, selected)) {
        auto delta = RemoveDensityStroke(volume, i - 1);
        removed.push_back({i - 1, marker, std::move(delta)});
        break;
      }
    }
    return removed;
  }
  if (!selected.stroke) {
    const auto last = std::find_if(
        volume.edits.rbegin(), volume.edits.rend(),
        [&](const Brush &brush) { return SameBrush(brush, selected); });
    if (last != volume.edits.rend()) {
      const auto index = std::size_t(
          std::distance(volume.edits.begin(), std::prev(last.base())));
      removed.push_back({index, *last});
      volume.edits.erase(std::prev(last.base()));
    }
    return removed;
  }
  for (std::size_t i = 0; i < volume.edits.size(); ++i)
    if (volume.edits[i].stroke == selected.stroke)
      removed.push_back({i, volume.edits[i]});
  std::erase_if(volume.edits, [&](const Brush &brush) {
    return brush.stroke == selected.stroke;
  });
  return removed;
}
inline void RestoreStroke(Volume &volume,
                          const std::vector<RemovedBrush> &removed) {
  for (const auto &entry : removed) {
    if (entry.density) {
      InsertDensityStroke(volume, entry.index, entry.density);
      continue;
    }
    if (entry.index > volume.edits.size() || volume.edits.size() >= MaxEdits)
      throw std::runtime_error("Terrain redo no longer matches its history");
    volume.edits.insert(volume.edits.begin() + entry.index, entry.brush);
  }
}
struct LevelStrokeTarget {
  std::uint64_t stroke{};
  float height{};
  float Sample(std::uint64_t currentStroke, float hitHeight) {
    if (stroke != currentStroke) {
      stroke = currentStroke;
      height = hitHeight;
    }
    return height;
  }
};
struct BrushStrengthClock {
  using Clock = std::chrono::steady_clock;
  std::uint64_t stroke{};
  Clock::time_point last{};
  float Amount(Clock::time_point now, std::uint64_t currentStroke,
               Clock::time_point pressed,
               float rate = BrushStrengthPerSecond) const {
    const auto from = currentStroke == stroke ? last : pressed;
    const auto elapsed = std::clamp(
        now - from, Clock::duration::zero(),
        std::chrono::duration_cast<Clock::duration>(MaxStrengthElapsed));
    return rate * std::chrono::duration<float>(elapsed).count();
  }
  void Applied(Clock::time_point now, std::uint64_t currentStroke) {
    last = now;
    stroke = currentStroke;
  }
};
struct BrushRepeat {
  using Clock = std::chrono::steady_clock;
  std::uint64_t stroke{};
  Clock::time_point next{};
  bool Due(Clock::time_point now, std::uint64_t heldStroke, bool blocked,
           std::chrono::milliseconds interval = std::chrono::milliseconds(20)) {
    if (stroke != heldStroke) {
      stroke = heldStroke;
      next = now;
    }
    if (!heldStroke || blocked || now < next)
      return false;
    // Keep fractional-frame timing without replaying missed intervals after a
    // rebuild or stall. The caller attempts at most one edit per game update.
    next = now >= next + interval ? now + interval : next + interval;
    return true;
  }
  void
  Started(Clock::time_point now, std::uint64_t heldStroke,
          std::chrono::milliseconds interval = std::chrono::milliseconds(20)) {
    stroke = heldStroke;
    next = now + interval;
  }
};
} // namespace SkyBuild::Terrain
