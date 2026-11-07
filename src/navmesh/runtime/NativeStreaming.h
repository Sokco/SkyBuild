#pragma once
namespace SkyBuild::Navmesh::NativeStreaming {
// Restore the active connected graph at the first detach boundary, before
// native cell events/state changes, using an owned native navigation pause.
[[nodiscard]] bool Initialize();
} // namespace SkyBuild::Navmesh::NativeStreaming
