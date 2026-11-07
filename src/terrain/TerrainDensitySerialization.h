#pragma once
#include "terrain/TerrainDensity.h"
#include <cstring>
#include <span>
#include <type_traits>

namespace SkyBuild::Terrain {
inline constexpr std::size_t MaxDensitySaveBytes = 256 * 1024 * 1024;
struct DensityWriter {
  std::vector<std::byte> bytes;
  template <class T> void Put(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    Raw(&value, sizeof(value));
  }
  void Raw(const void *data, std::size_t size) {
    if (size > MaxDensitySaveBytes - bytes.size())
      throw std::runtime_error("Terrain density save limit exceeded");
    const auto old = bytes.size();
    bytes.resize(old + size);
    std::memcpy(bytes.data() + old, data, size);
  }
};
struct DensityReader {
  std::span<const std::byte> bytes;
  template <class T> bool Get(T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    return Raw(&value, sizeof(value));
  }
  bool Raw(void *data, std::size_t size) {
    if (size > bytes.size())
      return false;
    std::memcpy(data, bytes.data(), size);
    bytes = bytes.subspan(size);
    return true;
  }
};
inline std::vector<std::byte> EncodeDensity(const Volume &source) {
  if (!source.density || !source.Valid())
    throw std::runtime_error("Missing terrain density checkpoint");
  DensityWriter out;
  out.Put(std::uint32_t(2));
  out.Put(source.origin);
  out.Put(source.bottom);
  out.Put(source.top);
  out.Put(source.heights);
  out.Put(*OriginalDensityHalo(source));
  const auto &cache = *source.density;
  out.Put(std::uint32_t(cache.current->values.size()));
  out.Raw(cache.current->values.data(),
          cache.current->values.size() * sizeof(float));
  return std::move(out.bytes);
}
inline bool DecodeDensity(std::span<const std::byte> bytes,
                          Volume &destination) {
  if (bytes.size() > MaxDensitySaveBytes)
    return false;
  DensityReader in{bytes};
  Volume volume;
  std::uint32_t version{}, count{};
  auto halo = std::make_shared<DensityHalo>();
  if (!in.Get(version) || (version != 1 && version != 2) ||
      !in.Get(volume.origin) || !in.Get(volume.bottom) || !in.Get(volume.top) ||
      !in.Get(volume.heights) || !in.Get(*halo) || !volume.Valid() ||
      !std::all_of(halo->begin(), halo->end(),
                   [](float x) { return std::isfinite(x); }))
    return false;
  for (int y = 0; y < Side; ++y)
    for (int x = 0; x < Side; ++x)
      if ((*halo)[(y + 1) * DensitySide + x + 1] !=
          volume.heights[y * Side + x])
        return false;
  volume.densityHeights = halo;
  auto cache = std::make_shared<DensityCache>();
  try {
    cache->original = MakeOriginalGrid(volume);
  } catch (const std::exception &) {
    return false;
  }
  auto current = std::make_shared<DensityGrid>(*cache->original);
  if (!in.Get(count) || count != current->values.size() ||
      !in.Raw(current->values.data(), count * sizeof(float)) ||
      !std::all_of(current->values.begin(), current->values.end(), [](float x) {
        return std::isfinite(x) && std::abs(x) <= MaxStoredDensity;
      }))
    return false;
  cache->current = cache->completed = current;
  count = 0;
  if (version == 1 && (!in.Get(count) || count > MaxEdits))
    return false;
  auto verify = std::make_shared<DensityGrid>(*cache->original);
  std::size_t changes{};
  for (unsigned i = 0; i < count; ++i) {
    auto stroke = std::make_shared<DensityStroke>();
    auto &marker = stroke->marker;
    std::uint8_t add{};
    std::uint32_t nodes{};
    if (!in.Get(marker.center) || !in.Get(marker.radius) || !in.Get(add) ||
        add > 1 || !in.Get(marker.strength) || !in.Get(marker.stroke) ||
        !in.Get(nodes) || nodes > current->values.size() ||
        nodes > in.bytes.size() / 16 || changes + nodes > 8 * 1024 * 1024 ||
        marker.stroke >= (std::uint64_t(1) << 62) - 1 ||
        !Finite(marker.center) || !std::isfinite(marker.radius) ||
        marker.radius < 16.F || marker.radius > 512 ||
        !std::isfinite(marker.strength) || marker.strength < 0 ||
        marker.center.z < volume.bottom || marker.center.z > volume.top)
      return false;
    marker.add = add != 0;
    changes += nodes;
    stroke->changes.reserve(nodes);
    for (unsigned n = 0; n < nodes; ++n) {
      DensityChange change;
      if (!in.Get(change.node.xy) || !in.Get(change.node.z) ||
          !in.Get(change.before) || !in.Get(change.after) ||
          change.node.xy >= DensitySide * DensitySide ||
          change.node.z < current->low || change.node.z > current->high ||
          !std::isfinite(change.before) || !std::isfinite(change.after) ||
          std::abs(change.before) > MaxStoredDensity ||
          std::abs(change.after) > MaxStoredDensity ||
          change.before == change.after ||
          (!stroke->changes.empty() &&
           !(stroke->changes.back().node < change.node)) ||
          verify->At(change.node) != change.before)
        return false;
      verify->values[verify->Index(change.node)] = change.after;
      stroke->changes.push_back(change);
    }
    if (marker.stroke &&
        std::any_of(cache->strokes.begin(), cache->strokes.end(),
                    [&](const auto &prior) {
                      return prior->marker.stroke == marker.stroke;
                    }))
      return false;
    volume.edits.push_back(marker);
    cache->strokes.push_back(std::move(stroke));
  }
  if (!in.bytes.empty() || (version == 1 && verify->values != current->values))
    return false;
  for (int z = current->low; z <= current->high; ++z)
    for (unsigned xy = 0; xy < DensitySide * DensitySide; ++xy) {
      const auto value = current->At({z, xy});
      cache->bound = std::max(cache->bound, std::abs(value));
      if (value != cache->original->At({z, xy}))
        cache->modified[xy] = true;
    }
  // Loading starts a new undo session, including older history-bearing saves.
  cache->baseline = current;
  cache->strokes.clear();
  volume.edits.clear();
  volume.density = cache;
  if (!volume.Valid() || DensityMemory(volume) > MaxDensitySaveBytes)
    return false;
  destination = std::move(volume);
  return true;
}
} // namespace SkyBuild::Terrain
