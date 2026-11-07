#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>
namespace SkyBuild::Terrain::BrushOverlay::Trace {
struct Stamp {
  std::uint64_t key{}, revision{}; // cell/chunk key, immutable mesh identity
};
template <class T> struct DrawnPayload {
  Stamp stamp;
  std::shared_ptr<const T> surface;
};
// Per-render-frame observations, not a history or a "latest" mesh cache.
template <class T> class FrameDraws {
  std::map<std::pair<std::uint64_t, std::uint64_t>, DrawnPayload<T>> surfaces;

public:
  void Record(Stamp stamp, std::shared_ptr<const T> surface) {
    if (stamp.key && stamp.revision && surface)
      surfaces.try_emplace({stamp.key, stamp.revision},
                           DrawnPayload<T>{stamp, std::move(surface)});
  }
  std::vector<DrawnPayload<T>>
  Select(std::span<const std::uint64_t> keys) const {
    std::vector<DrawnPayload<T>> result;
    for (auto key : keys) {
      for (auto it = surfaces.lower_bound({key, 0});
           it != surfaces.end() && it->first.first == key; ++it)
        result.push_back(it->second);
    }
    return result;
  }
  void Clear() { surfaces.clear(); }
};
} // namespace SkyBuild::Terrain::BrushOverlay::Trace
