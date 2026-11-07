#pragma once
#include <set>
namespace SkyBuild::Navmesh::NativeLocationCache {
// Hooks delegate normally unless a publication marks an info obsolete.
// Retired forms and infos remain pinned. Pointer guards do not establish
// a safe runtime FormID-reuse boundary.
[[nodiscard]] bool Initialize();
[[nodiscard]] bool Available();
[[nodiscard]] bool HasObsoleteInfos() noexcept;
using Obsolete = std::set<const RE::BSNavmeshInfo *>;
[[nodiscard]] std::shared_ptr<const Obsolete>
Prepare(const Obsolete &infos, const Obsolete &revived = {});
void Publish(std::shared_ptr<const Obsolete> infos) noexcept;
// Published generated infos stay pinned. Classify tokens without dereferencing
// obsolete engine pointers. Caller holds the publication write lock.
void RememberGenerated(const Obsolete &infos);
[[nodiscard]] Obsolete ObsoleteGenerated();
// Native-pause diagnostics restore this exact cohort, including a null one.
[[nodiscard]] std::shared_ptr<const Obsolete> CaptureObsolete() noexcept;
// Future stable-identity backend only: call inside the verified publication
// barrier after changing geometry. No object pointer/lifetime is used as a
// generation stamp. The legacy backend leaves this tracking disabled.
void PublishGeometryChange() noexcept;
void DisableGeometryTracking() noexcept;
} // namespace SkyBuild::Navmesh::NativeLocationCache
