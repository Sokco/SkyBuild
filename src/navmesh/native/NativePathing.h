#pragma once
#include <string>

namespace SkyBuild::Navmesh::NativePathing {
// Route invalidation, called on the SKSE task thread with an owned, ready
// native search pause. It never writes mesh geometry or registry records.
[[nodiscard]] bool RefreshPaths(void *) noexcept;
// Deferred streaming refresh covers currently attached high-process actors,
// not borrowed cell pointers from the departed publication.
[[nodiscard]] bool RefreshLoadedPaths(void *) noexcept;
[[nodiscard]] std::string Status();
} // namespace SkyBuild::Navmesh::NativePathing
