#pragma once
#include <atomic>

namespace SkyBuild::Navmesh::Diagnostics {
inline std::atomic_bool enabled{};
[[nodiscard]] inline bool Enabled() noexcept {
  return enabled.load(std::memory_order_relaxed);
}
// Controls optional diagnostics and log verbosity; native safety tracking is
// independent.
void SetEnabled(bool value);
} // namespace SkyBuild::Navmesh::Diagnostics
