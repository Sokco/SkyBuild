#include "building/BuildingMode.h"
#include "terrain/TerrainTool.h"

#include "building/ModifyMode.h"
#include "building/PickerMode.h"
#include "building/PlacementPreview.h"
#include "external/BetterThirdPersonSelection.h"
#include "ui/imgui/UI.h"
#include "ui/search/SearchMenu.h"
#include "ui/thumbnails/ThumbnailGenerator.h"

namespace SkyBuild::BuildingMode {
namespace {
constexpr std::string_view PLUGIN_NAME = "SkyBuild.esp";
constexpr RE::FormID HAMMER_LOCAL_FORM_ID = 0x005900;

RE::TESObjectWEAP *buildingHammer = nullptr;
bool sceneLoaded = false;
bool hammerGranted = false;
bool checkHammerInventory = false;

void ProvideHammer(RE::PlayerCharacter *player) {
  if (!checkHammerInventory || hammerGranted || !buildingHammer ||
      !sceneLoaded || !player || !player->Is3DLoaded() ||
      UI::IsGameMenuBlocking(true))
    return;
  checkHammerInventory = false;
  const auto count = [&] {
    const auto inventory = player->GetInventoryCounts(
        [](RE::TESBoundObject &object) { return &object == buildingHammer; });
    const auto found = inventory.find(buildingHammer);
    return found != inventory.end() ? found->second : 0;
  };
  if (count() > 0)
    return;
  player->AddObjectToContainer(buildingHammer, nullptr, 1, nullptr);
  hammerGranted = count() > 0;
  if (hammerGranted)
    spdlog::info("Added the Construction Hammer to the player's inventory.");
  else
    spdlog::warn(
        "Construction Hammer could not be added to the player's inventory.");
}
bool active = false;
bool hammerReady = false;
// This choice survives unequipping the hammer. Opening Settings or rebuilding
// navigation never changes it.
bool modeEnabled = true;
InteractionMode interactionMode = InteractionMode::kIdle;
bool carouselLoaded = false;
bool interactionBlocked = false;
void SetInteractionBlocked(bool blocked) {
  if (interactionBlocked == blocked)
    return;
  // Block active-tool inputs in InputHook, not through Skyrim's global
  // control flags. Global activation state can outlive this tool lifecycle
  // and interfere with normal Talk/Use/Sit after switching tools or loading.
  if (blocked) {
    if (const auto btps = BTPS::GetAPI())
      btps->HideSelectionWidget("SkyBuild");
  } else {
    if (const auto btps = BTPS::GetAPI())
      btps->ShowSelectionWidget("SkyBuild");
  }
  interactionBlocked = blocked;
}
// Own only the visibility change, never Skyrim's global menu visibility.
struct HiddenMovie {
  RE::GPtr<RE::GFxMovieView> movie;
  bool wasVisible{};

  void Restore() {
    if (!movie)
      return;
    movie->SetVisible(wasVisible);
    movie.reset();
  }

  void Refresh(const RE::GPtr<RE::GFxMovieView> &current, bool hide) {
    if (!hide || movie != current)
      Restore();
    if (!hide || !current)
      return;
    if (!movie) {
      movie = current;
      wasVisible = current->GetVisible();
    }
    current->SetVisible(false);
  }
};
HiddenMovie hiddenHUD;
HiddenMovie hiddenSelection;

void RestoreGameHUD() {
  hiddenHUD.Restore();
  hiddenSelection.Restore();
}

void RefreshGameHUD() {
  const auto ui = RE::UI::GetSingleton();
  const auto menu = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
  const auto movie = menu ? menu->uiMovie : RE::GPtr<RE::GFxMovieView>{};
  const bool hide = active && interactionMode != InteractionMode::kIdle;
  hiddenHUD.Refresh(movie, hide);
  // Debug builds cannot pass checked STL strings to BTPS's release API.
  // Fall back to its native movie only when that API cannot be used.
  const auto selection = ui ? ui->GetMenu("BTPS Menu") : nullptr;
  hiddenSelection.Refresh(selection ? selection->uiMovie
                                    : RE::GPtr<RE::GFxMovieView>{},
                          hide && !BTPS::GetAPI());
}

bool IsHammerEquipped(const RE::PlayerCharacter *a_player) {
  if (!a_player || !buildingHammer) {
    return false;
  }

  return a_player->GetEquippedObject(false) == buildingHammer ||
         a_player->GetEquippedObject(true) == buildingHammer;
}

void Deactivate() {
  SetInteractionBlocked(false);
  RestoreGameHUD();
  if (!Terrain::Ready())
    UI::CancelToolWheel();
  if (!active) {
    interactionMode = InteractionMode::kIdle;
    return;
  }
  UI::SearchMenu::Close();
  active = false;
  interactionMode = InteractionMode::kIdle;
  carouselLoaded = false;
  UI::ThumbnailGenerator::SetRenderingEnabled(false);
  ModifyMode::Stop();
  PickerMode::Stop();
  PlacementPreview::Stop();
  UI::SetVisible(hammerReady);
  spdlog::info("SkyBuild building closed.");
}
} // namespace

void ResetHammerGrant() {
  hammerGranted = false;
  checkHammerInventory = false;
}
void SaveHammerGrant(SKSE::SerializationInterface *serialization) {
  const std::uint8_t granted = hammerGranted ? 1 : 0;
  if (!serialization->OpenRecord(HammerSaveRecord, 1) ||
      !serialization->WriteRecordData(&granted, sizeof(granted)))
    spdlog::error("Failed to save Construction Hammer grant state.");
}
void LoadHammerGrant(SKSE::SerializationInterface *serialization,
                     std::uint32_t version, std::uint32_t length) {
  std::uint8_t granted{};
  if (version != 1 || length != sizeof(granted) ||
      serialization->ReadRecordData(&granted, sizeof(granted)) !=
          sizeof(granted) ||
      granted > 1) {
    spdlog::warn("Rejected invalid Construction Hammer grant record.");
    return;
  }
  hammerGranted = granted != 0;
}
void Initialize() {
  spdlog::info("BTPS interaction widget: {}",
               BTPS::GetAPI() ? "public V0 API" : "native menu fallback");
  buildingHammer =
      RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectWEAP>(
          HAMMER_LOCAL_FORM_ID, PLUGIN_NAME);
  if (buildingHammer) {
    spdlog::info("Resolved building hammer {:08X} from {}.",
                 buildingHammer->GetFormID(), PLUGIN_NAME);
  } else {
    spdlog::error(
        "Could not resolve building hammer local FormID {:06X} from {}.",
        HAMMER_LOCAL_FORM_ID, PLUGIN_NAME);
  }
}

void SetSceneLoaded(bool a_loaded) {
  sceneLoaded = a_loaded;
  checkHammerInventory = a_loaded && !hammerGranted;
  if (!sceneLoaded) {
    Deactivate();
    hammerReady = false;
    UI::SetVisible(false);
  }
}

void Update(RE::PlayerCharacter *a_player) {
  ProvideHammer(a_player);
  const bool wasReady = hammerReady;
  hammerReady = sceneLoaded && a_player && IsHammerEquipped(a_player) &&
                a_player->Is3DLoaded();
  if (!hammerReady || !modeEnabled)
    Deactivate();
  else if (!wasReady)
    SelectTool(InteractionMode::kBuild);
  UI::SetVisible(hammerReady);
  UI::RefreshPresentation();
  RefreshGameHUD();
  if (active && interactionMode == InteractionMode::kModify)
    ModifyMode::Update();
  if (active && interactionMode == InteractionMode::kPicker)
    PickerMode::Update();
}

void SetEnabled(bool enabled) {
  modeEnabled = enabled;
  if (!enabled)
    Deactivate();
  const auto player = RE::PlayerCharacter::GetSingleton();
  hammerReady =
      sceneLoaded && player && IsHammerEquipped(player) && player->Is3DLoaded();
}
bool IsHammerReady() { return hammerReady; }
bool IsActive() { return active; }

bool IsBuildMode() {
  return active && interactionMode == InteractionMode::kBuild;
}

InteractionMode GetTool() { return interactionMode; }
void SelectTool(InteractionMode tool) {
  if (!hammerReady)
    return;
  if (tool == InteractionMode::kIdle) {
    Deactivate();
    return;
  }
  UI::SearchMenu::Close();
  ModifyMode::Stop();
  PickerMode::Stop();
  if (tool == InteractionMode::kBuild && !PlacementPreview::Start())
    return;
  if (tool != InteractionMode::kBuild)
    PlacementPreview::Stop();
  if (tool == InteractionMode::kModify)
    ModifyMode::Start();
  if (tool == InteractionMode::kPicker)
    PickerMode::Start();
  active = true;
  interactionMode = tool;
  SetInteractionBlocked(true);
  RefreshGameHUD();
  UI::ThumbnailGenerator::SetRenderingEnabled(tool == InteractionMode::kBuild);
  if (tool == InteractionMode::kBuild && !carouselLoaded) {
    UI::LoadPlaceableCarousel();
    carouselLoaded = true;
  }
  UI::RefreshPresentation();
}
} // namespace SkyBuild::BuildingMode
