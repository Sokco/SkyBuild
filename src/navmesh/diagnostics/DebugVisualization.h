#pragma once
namespace SkyBuild::Navmesh::DebugVisualization {
void Initialize();
void SetSceneLoaded(bool loaded);
void Update();
void RefreshInstalledGeometry();
void __stdcall RenderNavigationSettings();
} // namespace SkyBuild::Navmesh::DebugVisualization
