#pragma once
#include "navmesh/native/WriterObservation.h"
namespace SkyBuild::Navmesh::NativeCutAudit {
struct Report {
  WriterObservation observation;
  bool available{};
};
void Initialize();
[[nodiscard]] Report Snapshot() noexcept;
} // namespace SkyBuild::Navmesh::NativeCutAudit
