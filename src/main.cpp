#include "naming/ReferenceNames.h"
#include "activators/Connections.h"
#include "containers/Containers.h"
#include "navmesh/diagnostics/DebugVisualization.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "building/PlacementPreview.h"
#include "building/BuildingManager.h"
#include "building/BuildingMode.h"
#include "building/PlaceableRegistry.h"
#include "hooks/GameUpdateHook.h"
#include "hooks/InputHook.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/NativeUI.h"
#include "ui/settings/Settings.h"
#include "ui/thumbnails/ThumbnailGenerator.h"
#include "log.h"
#include "Plugin.h"
#include "terrain/TerrainTool.h"
#include "map-markers/MapMarkers.h"
#include "map-markers/MarkerIcons.h"

namespace {
void MessageHandler(SKSE::MessagingInterface::Message *a_message) {
  if (!a_message) {
    return;
  }

  switch (a_message->type) {
  case SKSE::MessagingInterface::kPostLoad:
    SkyBuild::MapMarkers::Icons::Listen();
    break;
  case SKSE::MessagingInterface::kDataLoaded:
    if (!SkyBuild::PlaceableRegistry::Load()) {
      spdlog::error("No valid SkyBuild placeable definitions were loaded.");
    }
    SkyBuild::UI::Native::RegisterMenu();
    SkyBuild::Settings::Initialize();
    SkyBuild::Navmesh::Initialize();
    SkyBuild::Navmesh::DebugVisualization::Initialize();
    // Install exactly once.
    SkyBuild::Hooks::GameUpdateHook::Install();
    SkyBuild::Hooks::InputHook::Install();
    SkyBuild::UI::Initialize();
    SkyBuild::UI::SetVisible(false);
    SkyBuild::BuildingMode::Initialize();
    SkyBuild::Terrain::Initialize();
    SkyBuild::MapMarkers::Initialize();
    SkyBuild::Naming::Initialize();
    SkyBuild::Activators::Initialize();
    SkyBuild::Containers::Initialize();
    break;

  case SKSE::MessagingInterface::kPostLoadGame:
  case SKSE::MessagingInterface::kNewGame:
    SkyBuild::UI::Native::ResetInput();
    SkyBuild::BuildingManager::RefreshPackSettings();
    SkyBuild::MapMarkers::Restore();
    SkyBuild::BuildingMode::SetSceneLoaded(true);
    SkyBuild::Terrain::SetSceneLoaded(true);
    SkyBuild::Navmesh::SetSceneLoaded(true);
    SkyBuild::Navmesh::DebugVisualization::SetSceneLoaded(true);
    break;

  case SKSE::MessagingInterface::kPreLoadGame:
    SkyBuild::Containers::Reset();
    SkyBuild::Activators::Reset();
    SkyBuild::Naming::Reset();
    SkyBuild::UI::Native::ResetInput();
    SkyBuild::MapMarkers::Close();
    SkyBuild::BuildingMode::SetSceneLoaded(false);
    SkyBuild::Terrain::SetSceneLoaded(false);
    SkyBuild::Navmesh::SetSceneLoaded(false);
    SkyBuild::Navmesh::DebugVisualization::SetSceneLoaded(false);
    break;
  case SKSE::MessagingInterface::kSaveGame:
    break;

  default:
    break;
  }
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface *a_skse) {
  SKSE::Init(a_skse);

  Logging::Setup();

  spdlog::info("{} v{} loaded successfully.", Plugin::Name, Plugin::Version);
  spdlog::info("Skyrim runtime {}.", REL::Module::get().version().string());
  const auto runtime = REL::Module::get().version();
  const bool supportedSE = REL::Module::IsSE() &&
                           runtime == REL::Version{1, 5, 97, 0};
  const bool supportedAE = REL::Module::IsAE() &&
                           runtime >= REL::Version{1, 6, 1170, 0};
  if (!supportedSE && !supportedAE) {
    spdlog::critical("SkyBuild requires Skyrim 1.5.97 or AE 1.6.1170 or newer.");
    return false;
  }

  if (!SkyBuild::BuildingManager::InitializeSerialization()) {
    spdlog::critical("Failed to initialize BuildingManager serialization.");

    return false;
  }

  // CommonLib initializes this shared pool only once. Reserve the UI, input
  // and terrain recovery call stubs up front; later calls cannot grow it.
  SKSE::AllocTrampoline((2 + 1) * 14);
  const auto papyrus = SKSE::GetPapyrusInterface();
  if (!papyrus || !papyrus->Register(SkyBuild::Containers::RegisterPapyrus)) {
    spdlog::critical("Failed to register SkyBuild container functions.");
    return false;
  }
  if (!SkyBuild::UI::Native::Install())
    return false;
  SkyBuild::Navmesh::NativeAuthoredBasis::Initialize();
  const auto messaging = SKSE::GetMessagingInterface();

  if (!messaging || !messaging->RegisterListener(MessageHandler)) {
    spdlog::critical("Failed to register SKSE messaging listener.");

    return false;
  }

  return true;
}
