#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <map>
#include <span>
#include <set>

namespace SkyBuild::Navmesh {
struct PlacementStamp {
  std::uint32_t reference{}, base{};
  std::array<float, 7> transform{}; // position, rotation, scale
  bool operator==(const PlacementStamp &) const = default;
};
struct CachedCell {
  std::vector<Baseline> originals; // uncut, immutable, authored form IDs
  Mesh modified;                   // owned geometry; no runtime form IDs
  std::vector<PlacementStamp> placements;
};
using CellCache = std::map<std::uint32_t, CachedCell>;
inline constexpr std::size_t MAX_CELL_CACHE_BYTES = 64 * 1024 * 1024;
// Committed navigation survives placement edits. Only the original basis
// determines whether its saved geometry can be restored into this scene.
// A caller may supply checked authored/runtime door-face witnesses. Only the
// native door-state bit on those exact source faces is then an annotation.
[[nodiscard]] bool CacheOriginalsMatch(
    const CachedCell &cache,
    const std::vector<std::shared_ptr<const Baseline>> &originals,
    const std::map<std::uint32_t, std::set<std::uint32_t>> &doorFaces = {});
[[nodiscard]] std::string CacheOriginalsMismatch(
    const CachedCell &cache,
    const std::vector<std::shared_ptr<const Baseline>> &originals,
    const std::map<std::uint32_t, std::set<std::uint32_t>> &doorFaces = {});
[[nodiscard]] std::vector<std::uint8_t> EncodeCellCache(const CellCache &cache);
// Decode into a new map so malformed/unresolved records cannot partially
// overwrite the current save's cache. IDs resolve through SKSE on load.
[[nodiscard]] CellCache DecodeCellCache(
    std::span<const std::uint8_t> bytes,
    const std::function<std::optional<std::uint32_t>(std::uint32_t)> &resolve);
} // namespace SkyBuild::Navmesh
