#pragma once
#include "terrain/TerrainDensitySerialization.h"
#include <map>
#include <set>
namespace SkyBuild::Terrain::Grass {
struct ClearMask {
  static constexpr float TileSize = 32;
  using Tile = std::pair<int, int>;
  std::set<Tile> tiles;
  bool Hidden(float x, float y) const {
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 16777216 &&
           std::abs(y) <= 16777216 &&
           tiles.contains(
               {int(std::floor(x / TileSize)), int(std::floor(y / TileSize))});
  }
  bool Overlaps(float left, float bottom, float right, float top) const {
    auto it = tiles.lower_bound({int(std::floor(left / TileSize)) - 1,
                                 std::numeric_limits<int>::min()});
    for (; it != tiles.end() && it->first * TileSize <= right; ++it)
      if ((it->second + 1) * TileSize >= bottom && it->second * TileSize <= top)
        return true;
    return false;
  }
};
struct PersistentGrass {
  static constexpr std::size_t MaxTiles = 1 << 20;
  struct Change {
    ClearMask::Tile tile;
    bool before{}, after{};
  };
  struct Stroke {
    std::uint32_t world{};
    std::uint64_t id{};
    std::map<ClearMask::Tile, Change> changes;
  };
  std::map<std::uint32_t, ClearMask> worlds;
  std::vector<Stroke> undo, redo;
  std::size_t Count() const {
    std::size_t n{};
    for (const auto &[id, mask] : worlds)
      n += mask.tiles.size();
    return n;
  }
  bool Paint(std::uint32_t world, const Brush &brush, bool restore) {
    if (!world || !brush.stroke || !Finite(brush.center) || brush.radius <= 0 ||
        brush.radius > 512 || !std::isfinite(brush.radius) ||
        std::abs(brush.center.x) > 16777216 ||
        std::abs(brush.center.y) > 16777216)
      throw std::runtime_error("Invalid grass brush");
    std::vector<Change> changes;
    const auto found = worlds.find(world);
    const int x0 =
        int(std::floor((brush.center.x - brush.radius) / ClearMask::TileSize));
    const int x1 =
        int(std::floor((brush.center.x + brush.radius) / ClearMask::TileSize));
    const int y0 =
        int(std::floor((brush.center.y - brush.radius) / ClearMask::TileSize));
    const int y1 =
        int(std::floor((brush.center.y + brush.radius) / ClearMask::TileSize));
    for (int x = x0; x <= x1; ++x)
      for (int y = y0; y <= y1; ++y) {
        const float dx =
            brush.center.x - std::clamp(brush.center.x, x * ClearMask::TileSize,
                                        (x + 1) * ClearMask::TileSize);
        const float dy =
            brush.center.y - std::clamp(brush.center.y, y * ClearMask::TileSize,
                                        (y + 1) * ClearMask::TileSize);
        if (dx * dx + dy * dy > brush.radius * brush.radius)
          continue;
        const ClearMask::Tile tile{x, y};
        const bool before = found != worlds.end() &&
                            found->second.tiles.contains(tile),
                   after = !restore;
        if (before != after)
          changes.push_back({tile, before, after});
      }
    if (changes.empty())
      return false;
    if (!restore && Count() + changes.size() > MaxTiles)
      throw std::runtime_error("Grass clearing save limit reached");
    if (undo.empty() || undo.back().id != brush.stroke ||
        undo.back().world != world) {
      if (undo.size() >= MaxEdits)
        throw std::runtime_error("Grass undo history limit reached");
      if (found == worlds.end() && worlds.size() >= 4096)
        throw std::runtime_error("Grass world limit reached");
      undo.push_back({world, brush.stroke, {}});
    }
    auto &stroke = undo.back();
    auto &mask = worlds[world];
    for (const auto &change : changes) {
      if (change.after)
        mask.tiles.insert(change.tile);
      else
        mask.tiles.erase(change.tile);
      auto [it, inserted] = stroke.changes.try_emplace(change.tile, change);
      if (!inserted)
        it->second.after = change.after;
      if (it->second.before == it->second.after)
        stroke.changes.erase(it);
    }
    if (stroke.changes.empty())
      undo.pop_back();
    if (mask.tiles.empty())
      worlds.erase(world);
    redo.clear();
    return true;
  }
  std::uint64_t LastUndo(std::uint32_t world) const {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it)
      if (it->world == world)
        return it->id;
    return 0;
  }
  std::uint64_t LastRedo(std::uint32_t world) const {
    for (auto it = redo.rbegin(); it != redo.rend(); ++it)
      if (it->world == world)
        return it->id;
    return 0;
  }
  bool Replay(std::uint32_t world, bool forward) {
    auto &from = forward ? redo : undo;
    auto &to = forward ? undo : redo;
    auto it = std::find_if(from.rbegin(), from.rend(),
                           [&](const auto &s) { return s.world == world; });
    if (it == from.rend())
      return false;
    auto stroke = *it;
    auto &mask = worlds[world];
    for (const auto &[tile, c] : stroke.changes) {
      if (forward ? c.after : c.before)
        mask.tiles.insert(tile);
      else
        mask.tiles.erase(tile);
    }
    if (mask.tiles.empty())
      worlds.erase(world);
    from.erase(std::next(it).base());
    to.push_back(std::move(stroke));
    return true;
  }
};
inline std::vector<std::byte> EncodeGrass(const PersistentGrass &state) {
  if (state.Count() > PersistentGrass::MaxTiles || state.worlds.size() > 4096)
    throw std::runtime_error("Grass clearing save limit reached");
  DensityWriter out;
  out.Put(std::uint32_t(state.worlds.size()));
  for (const auto &[world, mask] : state.worlds) {
    out.Put(world);
    out.Put(std::uint32_t(mask.tiles.size()));
    for (const auto &[x, y] : mask.tiles) {
      out.Put(std::int32_t(x));
      out.Put(std::int32_t(y));
    }
  }
  return std::move(out.bytes);
}
inline bool DecodeGrass(std::span<const std::byte> bytes,
                        PersistentGrass &destination) {
  if (bytes.size() > 16 * 1024 * 1024)
    return false;
  DensityReader in{bytes};
  std::uint32_t worlds{}, world{}, count{};
  PersistentGrass loaded;
  std::size_t total{};
  if (!in.Get(worlds) || worlds > 4096)
    return false;
  for (unsigned i = 0; i < worlds; ++i) {
    if (!in.Get(world) || !world || loaded.worlds.contains(world) ||
        !in.Get(count) || !count || count > PersistentGrass::MaxTiles - total)
      return false;
    total += count;
    auto &mask = loaded.worlds[world];
    for (unsigned n = 0; n < count; ++n) {
      std::int32_t x{}, y{};
      if (!in.Get(x) || !in.Get(y) || std::abs(std::int64_t(x)) > 524305 ||
          std::abs(std::int64_t(y)) > 524305 ||
          !mask.tiles.emplace(x, y).second)
        return false;
    }
  }
  if (!in.bytes.empty())
    return false;
  destination = std::move(loaded);
  return true;
}
} // namespace SkyBuild::Terrain::Grass
