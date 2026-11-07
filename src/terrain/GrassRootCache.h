#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SkyBuild::Terrain::Grass {
// Only root position affects terrain height. Renderer changes to blade flags,
// spawn times, etc. must be preserved without repeating a surface query.
struct RootHeightCache {
  std::array<std::byte, 18> key{};
  std::uint16_t height{};
  bool valid{};
  float x{}, y{};
  bool Matches(const void *position, const void *origin) const {
    return valid && !std::memcmp(key.data(), position, 6) &&
           !std::memcmp(key.data() + 6, origin, 12);
  }
  template <class Evaluate>
  std::uint16_t Resolve(const void *position, const void *origin, bool refresh,
                        Evaluate evaluate) {
    if (refresh || !Matches(position, origin)) {
      height = evaluate();
      std::memcpy(key.data(), position, 6);
      std::memcpy(key.data() + 6, origin, 12);
      valid = true;
    }
    return height;
  }
};
} // namespace SkyBuild::Terrain::Grass
