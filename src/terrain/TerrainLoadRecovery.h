#pragma once
#include "terrain/TerrainDensity.h"
#include "terrain/TerrainHeightQueries.h"
#include <optional>
namespace SkyBuild::Terrain {
struct RecoveryCell {
  std::uint32_t cell{}, world{};
  Volume volume;
};
using RecoverySnapshot = std::vector<std::shared_ptr<const RecoveryCell>>;
inline std::shared_ptr<const RecoverySnapshot>
WithoutRecoveryCell(const std::shared_ptr<const RecoverySnapshot> &previous,
                    std::uint32_t id) {
  if (!previous || std::ranges::none_of(*previous, [id](const auto &entry) {
        return entry->cell == id;
      }))
    return previous;
  auto next = std::make_shared<RecoverySnapshot>(*previous);
  std::erase_if(*next, [id](const auto &entry) { return entry->cell == id; });
  return next->empty() ? nullptr : next;
}
inline std::shared_ptr<const RecoverySnapshot>
WithRecoveryCell(const std::shared_ptr<const RecoverySnapshot> &previous,
                 std::uint32_t cell, std::uint32_t world,
                 const Volume &volume) {
  if (!world || !HasTerrainChanges(volume))
    return WithoutRecoveryCell(previous, cell);
  if (previous && volume.density)
    for (const auto &entry : *previous)
      if (entry->cell == cell && entry->world == world &&
          entry->volume.density &&
          entry->volume.density->current == volume.density->current)
        return previous;
  auto entry =
      std::make_shared<RecoveryCell>(RecoveryCell{cell, world, volume});
  if (volume.density) {
    // Share the immutable current samples, without retaining undo history or
    // superseded grids through the arrival-query snapshot.
    auto density = std::make_shared<DensityCache>();
    density->current = volume.density->current;
    density->modified = volume.density->modified;
    density->bound = volume.density->bound;
    entry->volume.density = std::move(density);
    entry->volume.edits.clear();
  }
  auto next = previous ? std::make_shared<RecoverySnapshot>(*previous)
                       : std::make_shared<RecoverySnapshot>();
  std::erase_if(*next, [cell](const auto &old) { return old->cell == cell; });
  next->push_back(std::move(entry));
  return next;
}
// Recovery must never raise native LAND height, or affect another cell/world.
inline std::optional<float>
LoadRecoveryHeight(const Volume &volume, Vec position, float nativeHeight) {
  if (!Finite(position) || !std::isfinite(nativeHeight) ||
      position.x < volume.origin.x || position.x >= volume.origin.x + 4096.F ||
      position.y < volume.origin.y || position.y >= volume.origin.y + 4096.F ||
      !HasTerrainChanges(volume) ||
      !DensityAffected(volume, position.x, position.y))
    return {};
  try {
    const float floor = LowestSurface(volume, position.x, position.y) - .5F;
    if (std::isfinite(floor) && floor < nativeHeight - .01F)
      return floor;
  } catch (const std::exception &) {
  }
  return {};
}
} // namespace SkyBuild::Terrain
