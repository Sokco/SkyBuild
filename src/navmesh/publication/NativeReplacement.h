#pragma once
#include <set>
#include <string>
namespace SkyBuild::Navmesh {
struct NativeOriginalView;
}
namespace SkyBuild::Navmesh::NativeReplacement {
[[nodiscard]] bool CopyOriginalView(NativeOriginalView &out,
                                    std::string &error);
[[nodiscard]] bool Prepare();
[[nodiscard]] bool Install(void *) noexcept;
[[nodiscard]] bool Restore(void *) noexcept;
[[nodiscard]] bool RestoreForStreaming(void *) noexcept;
[[nodiscard]] bool ReconcileMembership(void *) noexcept;
[[nodiscard]] bool NeedsMembershipReconciliation();
[[nodiscard]] bool Installed();
[[nodiscard]] bool DependsOnCell(RE::FormID cell);
[[nodiscard]] std::set<RE::FormID>
ReadyDeferredConnections(const std::set<RE::FormID> &cells);
// Cohort association, including native successors not yet strongly retained.
[[nodiscard]] bool TracksMesh(const RE::NavMesh *mesh);
[[nodiscard]] bool TracksRetiredMesh(const RE::NavMesh *mesh);
void SceneReset();
void FinishPending();
void SceneLoaded();
[[nodiscard]] std::string Status();
[[nodiscard]] std::string RetirementStatus();
[[nodiscard]] bool ReclaimRetired(void *) noexcept;
} // namespace SkyBuild::Navmesh::NativeReplacement
