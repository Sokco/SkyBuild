#pragma once
#include <cstdint>
#include <optional>
namespace SkyBuild::Navmesh {
// Legacy decoded record values. CutMaterialization independently models
// mesh-local effects; publication integration awaits full binding validation.
// 93310 writes 1 before construction completes. 93311 writes 3 before the
// enclosing 91152 removal/compaction/reapplication transaction completes.
// Interpreting them requires a separately established coherent capture.
// See RuntimeRedesign.md; DecodeCutLifecycle alone provides no such proof.
enum class CutLifecycle : std::uint32_t { Applied = 1, Undone = 3 };
[[nodiscard]] inline std::optional<CutLifecycle>
DecodeCutLifecycle(std::uint32_t value) {
  switch (value) {
  case 1:
    return CutLifecycle::Applied;
  case 3:
    return CutLifecycle::Undone;
  default:
    return std::nullopt;
  }
}
} // namespace SkyBuild::Navmesh
