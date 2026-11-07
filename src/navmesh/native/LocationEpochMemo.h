#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace SkyBuild::Navmesh {
// Value memo, not object ownership. A reused address can hit only if its
// complete semantic state was observed after native resolution in the same
// geometry epoch. Fixed storage avoids allocation/failure and locks in pathing
// callbacks.
template <class State, std::size_t Capacity = 4096> class LocationEpochMemo {
  static_assert(Capacity > 0 && std::is_trivially_copyable_v<State>);

public:
  bool NeedsResolution(std::uint64_t epoch, std::uintptr_t location,
                       const State &state) const noexcept {
    if (!epoch)
      return false;
    const auto &entry = entries[Index(location)];
    return entry.epoch != epoch || entry.location != location ||
           entry.state != state;
  }
  void Resolved(std::uint64_t epoch, std::uintptr_t location,
                const State &state) noexcept {
    if (epoch)
      entries[Index(location)] = {location, epoch, state};
  }

private:
  static std::size_t Index(std::uintptr_t location) noexcept {
    return ((location >> 4) ^ (location >> 16)) % Capacity;
  }
  struct Entry {
    std::uintptr_t location{};
    std::uint64_t epoch{};
    State state{};
  };
  std::array<Entry, Capacity> entries{};
};
} // namespace SkyBuild::Navmesh
