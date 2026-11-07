#pragma once
#include <cstdint>

namespace SkyBuild::Navmesh::NativeSearchAudit {
struct Report {
  std::uint32_t active{};
  bool available{};
};
// Passive instrumentation of verified request-processor dispatch slots.
// Does not block searches or establish a geometry publication barrier.
void Initialize();
[[nodiscard]] Report Snapshot();
[[nodiscard]] bool InSearchOnCurrentThread();
} // namespace SkyBuild::Navmesh::NativeSearchAudit
