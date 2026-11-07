#include "activators/Connections.h"
#include "containers/Containers.h"
#include "map-markers/MapMarkers.h"
#include "navmesh/diagnostics/DebugVisualization.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "navmesh/runtime/GenerationGameplayPause.h"
#include "ui/search/SearchMenu.h"
#include "ui/fine-adjustments/FineAdjustments.h"
#include "hooks/GameUpdateHook.h"
#include "building/BuildingMode.h"
#include "terrain/TerrainTool.h"
#include "building/PlacementPreview.h"
#include "ui/thumbnails/ThumbnailGenerator.h"
#include "ui/imgui/UI.h"
#include "ui/pack-editor/PackEditor.h"

namespace SkyBuild::Hooks {

void GameUpdateHook::Install() {
  static bool installed = false;

  if (installed) {
    return;
  }

  installed = true;

  REL::Relocation<std::uintptr_t> playerVTable{RE::VTABLE_PlayerCharacter[0]};

  Original = playerVTable.write_vfunc(0x7D, Thunk);

  spdlog::info("Installed GameUpdateHook.");
}

void GameUpdateHook::Thunk(RE::PlayerCharacter *a_player, float a_deltaTime) {
  // Let Skyrim update the player and camera-related state first.
  Original(a_player, a_deltaTime);
  SkyBuild::BuildingMode::Update(a_player);
  SkyBuild::Terrain::Update(a_player);
  SkyBuild::PlacementPreview::Update();
  SkyBuild::UI::ThumbnailGenerator::Update();
  SkyBuild::UI::PackEditor::Update();
  SkyBuild::UI::SearchMenu::Update();
  SkyBuild::MapMarkers::Update();
  SkyBuild::Activators::Update();
  SkyBuild::Containers::Update();
  SkyBuild::UI::FineAdjustments::Update();
  if (!SkyBuild::Navmesh::GenerationGameplayPause::Ready())
    SkyBuild::Navmesh::Update();
  SkyBuild::Navmesh::DebugVisualization::Update();
}
} // namespace SkyBuild::Hooks
