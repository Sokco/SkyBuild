#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
#include <set>
namespace SkyBuild::Navmesh {
// Same ready pause and publication lock as CaptureNativeTopology. Neither
// establishes all native-writer exclusion. Raw shared status is separated from
// mesh-local materialization using geometry and ownership witnesses. Owns only
// decoded values: no native undo, clone, reference retention or publication.
[[nodiscard]] NativeCutSnapshot CaptureNativeCuts(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &loaded);
// Borrowed-target census permits distinct native instances with the same ID.
// Caller owns every source and holds the publication pause/lock. Returned
// pointers are comparison tokens only; target memory is never dereferenced.
[[nodiscard]] std::set<const RE::BSNavmesh *>
CaptureCutBorrowedTargets(const std::vector<const RE::BSNavmesh *> &sources);
} // namespace SkyBuild::Navmesh
