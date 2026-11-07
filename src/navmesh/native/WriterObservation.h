#pragma once
#include <atomic>
#include <cstdint>
namespace SkyBuild::Navmesh {
struct WriterObservation {
  std::uint64_t epoch{}, calls{};
  std::uint32_t active{};
  bool operator==(const WriterObservation &) const = default;
};
// Passive evidence only. An unchanged observation is not an exclusion lock.
class WriterObserver {
public:
  void Enter() noexcept {
    epoch.fetch_add(1);
    active.fetch_add(1);
    calls.fetch_add(1);
  }
  void Leave() noexcept {
    active.fetch_sub(1);
    epoch.fetch_add(1);
  }
  [[nodiscard]] WriterObservation Snapshot() const noexcept {
    return {epoch.load(), calls.load(), active.load()};
  }
  [[nodiscard]] static bool Changed(WriterObservation before,
                                    WriterObservation after) noexcept {
    return before.active || after.active || before.epoch != after.epoch ||
           before.calls != after.calls;
  }

private:
  std::atomic_uint64_t epoch{}, calls{};
  std::atomic_uint32_t active{};
};
} // namespace SkyBuild::Navmesh
