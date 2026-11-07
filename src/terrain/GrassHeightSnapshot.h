#pragma once
#include "terrain/GrassBrushMask.h"
#include "terrain/GrassSpatialChanges.h"
#include "terrain/PersistentGrass.h"
#include "terrain/TerrainSurface.h"
#include <memory>

namespace SkyBuild::Terrain::Grass {
using Surfaces = std::array<std::shared_ptr<const TerrainSurface>, 16>;
struct HeightSnapshot {
  struct Cell {
    Volume baseline;
    Surfaces surfaces;
  };
  BrushMask hidden;
  ClearMask cleared;
  bool Concealed(float x, float y) const {
    return hidden.Hidden(x, y) || cleared.Hidden(x, y);
  }
  std::uint64_t revision{};
  std::uint32_t world{};
  SpatialChanges changes;
  SpatialChanges surfaceChanges;
  std::vector<Cell> cells;
  std::optional<GrassBounds> affectedBounds;
  void PrepareChanges(const HeightSnapshot &previous) {
    changes = previous.changes;
    surfaceChanges = previous.surfaceChanges;
    std::optional<GrassBounds> changed;
    const auto add = [&](float x, float y, float size) {
      const GrassBounds b{x, y, x + size, y + size};
      if (changed)
        changed->Include(b);
      else
        changed = b;
    };
    const auto mask = [&](const auto &a, const auto &b, float size) {
      auto i = a.begin(), j = b.begin();
      while (i != a.end() || j != b.end()) {
        if (j == b.end() || (i != a.end() && *i < *j)) {
          add(i->first * size, i->second * size, size);
          ++i;
        } else if (i == a.end() || *j < *i) {
          add(j->first * size, j->second * size, size);
          ++j;
        } else {
          ++i;
          ++j;
        }
      }
    };
    const auto surfaces = [&](const auto &a, const auto &b) {
      for (const auto &cell : a) {
        const auto p = cell.baseline.origin;
        const auto other = std::find_if(b.begin(), b.end(), [&](const auto &v) {
          return v.baseline.origin.x == p.x && v.baseline.origin.y == p.y;
        });
        if (other == b.end()) {
          add(p.x, p.y, CellSize);
          continue;
        }
        for (unsigned k = 0; k < 16; ++k)
          if (cell.surfaces[k] != other->surfaces[k])
            add(p.x + (k % 4) * 1024, p.y + (k / 4) * 1024, 1024);
      }
    };
    surfaces(cells, previous.cells);
    surfaces(previous.cells, cells);
    surfaceChanges.Add(revision, changed, world != previous.world);
    mask(hidden.tiles, previous.hidden.tiles, BrushMask::TileSize);
    mask(cleared.tiles, previous.cleared.tiles, ClearMask::TileSize);
    changes.Add(revision, changed, world != previous.world);
  }
  // Built once when publishing, rather than walking masks on every GPU bind.
  void PrepareBounds() {
    affectedBounds.reset();
    const auto add = [&](float x, float y, float size) {
      const GrassBounds bounds{x, y, x + size, y + size};
      if (affectedBounds)
        affectedBounds->Include(bounds);
      else
        affectedBounds = bounds;
    };
    for (const auto &cell : cells)
      add(cell.baseline.origin.x, cell.baseline.origin.y, CellSize);
    for (const auto &[x, y] : hidden.tiles)
      add(x * BrushMask::TileSize, y * BrushMask::TileSize,
          BrushMask::TileSize);
    for (const auto &[x, y] : cleared.tiles)
      add(x * ClearMask::TileSize, y * ClearMask::TileSize,
          ClearMask::TileSize);
  }
  bool MayAffect(float x, float y) const {
    return affectedBounds && x >= affectedBounds->left &&
           x <= affectedBounds->right && y >= affectedBounds->bottom &&
           y <= affectedBounds->top;
  }
  std::optional<float> Highest(float x, float y) const {
    if (Concealed(x, y))
      return BrushMask::HiddenHeight;
    for (const auto &cell : cells) {
      const auto origin = cell.baseline.origin;
      if (x < origin.x || y < origin.y || x >= origin.x + CellSize ||
          y >= origin.y + CellSize)
        continue;
      if (!DensityAffected(cell.baseline, x, y))
        return {};
      std::optional<float> height;
      for (const auto &surface : cell.surfaces)
        if (surface)
          if (auto hit = surface->Highest(x, y);
              hit && (!height || hit->height > *height))
            height = hit->height;
      return height;
    }
    return {};
  }
};
} // namespace SkyBuild::Terrain::Grass
