#pragma once
namespace SkyBuild::Navmesh::GenerationGameplayPause {
void Initialize();
[[nodiscard]] bool Begin();
void End();
[[nodiscard]] bool Owned() noexcept;
[[nodiscard]] bool Ready();
[[nodiscard]] bool OnlyGameplayPause();
} // namespace SkyBuild::Navmesh::GenerationGameplayPause
