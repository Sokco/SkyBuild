#include "ui/pack-editor/PackEditor.h"
#include "ui/settings/Settings.h"
#include "SkyBuildPaths.h"
#include "building/ControlBindings.h"
#include "building/ModifyMode.h"
#include "building/PlacementPreview.h"
#include "navmesh/diagnostics/DebugVisualization.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "terrain/TerrainTool.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/UI.h"
#include "ui/settings/SettingsFields.h"
#include "ui/settings/SettingsValues.h"
#include "ui/thumbnails/ThumbnailGenerator.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace SkyBuild::Settings {
namespace {
std::atomic_bool sandboxMode{false};
std::atomic<float> buildingCostMultiplier{1.F};
std::atomic_bool quickMain{false}, quickTools{true};
struct Key {
  std::uint32_t code;
  const char *label;
  const char *icon;
};
constexpr Key keys[]{
    {1, "Escape", "Escape"},
    {12, "Minus", "Minus"},
    {13, "Plus / Equals", "Equal"},
    {16, "Q", "KeyQ"},
    {17, "W", "KeyW"},
    {18, "E", "KeyE"},
    {19, "R", "KeyR"},
    {20, "T", "KeyT"},
    {21, "Y", "KeyY"},
    {22, "U", "KeyU"},
    {23, "I", "KeyI"},
    {24, "O", "KeyO"},
    {25, "P", "KeyP"},
    {30, "A", "KeyA"},
    {31, "S", "KeyS"},
    {32, "D", "KeyD"},
    {33, "F", "KeyF"},
    {34, "G", "KeyG"},
    {35, "H", "KeyH"},
    {36, "J", "KeyJ"},
    {37, "K", "KeyK"},
    {38, "L", "KeyL"},
    {44, "Z", "KeyZ"},
    {45, "X", "KeyX"},
    {46, "C", "KeyC"},
    {47, "V", "KeyV"},
    {48, "B", "KeyB"},
    {49, "N", "KeyN"},
    {50, "M", "KeyM"},
    {2, "1", "Digit1"},
    {3, "2", "Digit2"},
    {4, "3", "Digit3"},
    {5, "4", "Digit4"},
    {6, "5", "Digit5"},
    {7, "6", "Digit6"},
    {8, "7", "Digit7"},
    {9, "8", "Digit8"},
    {10, "9", "Digit9"},
    {11, "0", "Digit0"},
    {59, "F1", "F1"},
    {60, "F2", "F2"},
    {61, "F3", "F3"},
    {62, "F4", "F4"},
    {63, "F5", "F5"},
    {64, "F6", "F6"},
    {65, "F7", "F7"},
    {66, "F8", "F8"},
    {67, "F9", "F9"},
    {68, "F10", "F10"},
    {87, "F11", "F11"},
    {88, "F12", "F12"},
    {203, "Left", "ArrowLeft"},
    {205, "Right", "ArrowRight"},
    {200, "Up", "ArrowUp"},
    {208, "Down", "ArrowDown"},
    {57, "Space", "Space"},
    {28, "Enter", "Enter"},
    {15, "Tab", "Tab"},
    {256, "Left mouse", "MouseLeft"},
    {257, "Right mouse", "MouseRight"},
    {258, "Middle mouse", "MouseMiddle"},
    {264, "Scroll up", "MouseWheelUp"},
    {265, "Scroll down", "MouseWheelDown"},
    {14, "Backspace", "Backspace"},
    {29, "Left Ctrl", "ControlLeft"},
    {157, "Right Ctrl", "ControlRight"},
    {42, "Left Shift", "ShiftLeft"},
    {54, "Right Shift", "ShiftRight"},
    {56, "Left Alt", "AltLeft"},
    {184, "Right Alt", "AltRight"},
    {199, "Home", "Home"},
    {207, "End", "End"},
    {201, "Page Up", "PageUp"},
    {209, "Page Down", "PageDown"},
    {210, "Insert", "Insert"},
    {211, "Delete", "Delete"},
    {266, "D-pad up", "PadUp"},
    {267, "D-pad down", "PadDown"},
    {268, "D-pad left", "PadLeft"},
    {269, "D-pad right", "PadRight"},
    {270, "Start", "PadStart"},
    {271, "View / Back", "PadBack"},
    {272, "Left stick", "PadL3"},
    {273, "Right stick", "PadR3"},
    {274, "LB / L1", "PadLB"},
    {275, "RB / R1", "PadRB"},
    {276, "A / Cross", "PadA"},
    {277, "B / Circle", "PadB"},
    {278, "X / Square", "PadX"},
    {279, "Y / Triangle", "PadY"},
    {280, "LT / L2", "PadLT"},
    {281, "RT / R2", "PadRT"}};
using Fields::actions;
using Fields::BrushBindingOffset;
using Fields::ModifyBindingOffset;
using Fields::SecondaryBindingAction;
using Fields::CounterclockwiseBindingAction;
using Fields::fineActions;
using Fields::HistoryBindingOffset;
using Fields::MenuBindingAction;
struct IconPack {
  std::string name;
  std::vector<RE::FormID> forms;
  bool selected{false};
  std::vector<std::filesystem::path> iconPaths;
  std::size_t missing{0};
  std::vector<RE::FormID> missingForms;
};
std::vector<IconPack> iconPacks;
std::mutex iconPacksMutex;
std::optional<std::vector<IconPack>> pendingIconPacks;
std::uint64_t iconPacksRevision = 0;
SkyBuild::UI::Native::Model::WindowInterface *progressWindow = nullptr;
bool regenerationProgress = false;
std::atomic<std::uint64_t> autoCloseGeneration{0};
std::uint64_t expectedGeneration = 0;
void DrawGenerationProgress() {
  const auto progress = UI::ThumbnailGenerator::GetProgress();
  const bool preparing =
      progress.generation < expectedGeneration || progress.total == 0;
  ImGui::TextWrapped("%s", preparing ? "Preparing icons..."
                                     : progress.status.c_str());
  const auto count =
      preparing ? std::string("Starting...")
                : std::format("{} / {}", progress.completed, progress.total);
  ImGui::ProgressBar(preparing ? 0.0F
                               : static_cast<float>(progress.completed) /
                                     static_cast<float>(progress.total),
                     {-1.0F, 0.0F}, count.c_str());
  if (!preparing && progress.failed) {
    const auto errors = std::format(
        "{} icon(s) failed. See SkyBuild.log for details.", progress.failed);
    ImGui::TextWrapped("%s", errors.c_str());
  }
}
void DrawGenerationCancel() {
  const auto progress = UI::ThumbnailGenerator::GetProgress();
  ImGui::BeginDisabled(progress.cancelling);
  if (ImGui::Button(progress.cancelling ? "Cancelling..." : "Cancel")) {
    SKSE::GetTaskInterface()->AddTask([] { UI::ThumbnailGenerator::Cancel(); });
  }
  ImGui::EndDisabled();
}
void __stdcall RenderGenerationProgressWindow() {
  const auto progress = UI::ThumbnailGenerator::GetProgress();
  const auto autoClose = autoCloseGeneration.load();
  if (autoClose && progress.generation == autoClose && !progress.running &&
      !progress.failed && !progress.cancelled &&
      progress.completed == progress.total) {
    progressWindow->IsOpen.store(false);
    autoCloseGeneration.store(0);
    return;
  }
  UI::Widgets::WorkspaceTheme theme;
  ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Always);
  if (ImGui::Begin("Icon Generation", nullptr,
                   ImGui::ImGuiWindowFlags_AlwaysAutoResize)) {
    UI::Widgets::WorkspaceFont();
    DrawGenerationProgress();
    if (UI::ThumbnailGenerator::GetProgress().running)
      DrawGenerationCancel();
    else if (ImGui::Button("Close"))
      progressWindow->IsOpen.store(false);
  }
  if (UI::Widgets::EscapePressed()) {
    if (progress.running)
      SKSE::GetTaskInterface()->AddTask(
          [] { UI::ThumbnailGenerator::Cancel(); });
    progressWindow->IsOpen.store(false);
  }
  ImGui::End();
}
Values draft;
std::string status;
std::atomic<bool> capturing{false};
std::atomic<std::uint32_t> capturedCode{0}, heldCode{0};
int captureAction = -1;
std::atomic_bool captureGamepad{false};
std::unique_ptr<SkyBuild::UI::Native::Model::Event> menuSubscription;
bool CaptureButton(RE::InputEvent *event) {
  const auto button = event ? event->AsButtonEvent() : nullptr;
  if (!button)
    return false;
  const auto device = button->GetDevice();
  if (device != RE::INPUT_DEVICE::kKeyboard &&
      device != RE::INPUT_DEVICE::kMouse &&
      device != RE::INPUT_DEVICE::kGamepad)
    return false;
  const auto code = ControlBindings::InputCode(*button);
  if ((device == RE::INPUT_DEVICE::kGamepad) != captureGamepad.load() && code != 1)
    return true;
  // Wheel inputs are pulses; finish capture immediately without waiting for
  // release.
  if (device == RE::INPUT_DEVICE::kMouse &&
      (button->GetIDCode() == 8 || button->GetIDCode() == 9)) {
    if (!capturing.load())
      return false;
    capturedCode.store(code);
    heldCode.store(0);
    capturing.store(false);
    return true;
  }
  if (heldCode.load() == code) {
    if (button->IsUp()) {
      capturedCode.store(code);
      heldCode.store(0);
      capturing.store(false);
    }
    return true;
  }
  if (!capturing.load())
    return false;
  if (button->IsDown() && heldCode.load() == 0) {
    heldCode.store(code);
  }
  return true;
}
void __stdcall MenuEvent(SkyBuild::UI::Native::Model::EventType event) {
  if (event == SkyBuild::UI::Native::Model::kCloseMenu) {
    capturing.store(false);
    capturedCode.store(0);
    heldCode.store(0);
  }
  if (event == SkyBuild::UI::Native::Model::kOpenMenu ||
      event == SkyBuild::UI::Native::Model::kCloseMenu) {
    SKSE::GetTaskInterface()->AddTask([] { UI::RefreshPresentation(); });
  }
}
const Key *FindKey(std::uint32_t code) {
  const auto it =
      std::find_if(std::begin(keys), std::end(keys),
                   [code](const Key &key) { return key.code == code; });
  return it == std::end(keys) ? nullptr : it;
}
std::filesystem::path Path() { return Paths::Root() / "SkyBuildSettings.json"; }
void Apply(const Values &values) {
  sandboxMode.store(values.sandboxMode);
  buildingCostMultiplier.store(values.buildingCostMultiplier);
  const auto *controls = RE::ControlMap::GetSingleton();
  UI::Widgets::SetGamepadIcons(controls && controls->GetGamePadType() ==
                                               RE::PC_GAMEPAD_TYPE::kOrbis);
  quickMain.store(values.quickSelectMainMenu);
  quickTools.store(values.quickSelectToolMenu);
  Terrain::SetStrengthPerSecond(values.terrainStrengthPerSecond);
  Navmesh::Diagnostics::SetEnabled(false);
  using namespace ControlBindings;
  const auto set = [](Binding &binding, std::uint32_t code,
                      std::uint32_t padCode) {
    const auto *key = FindKey(code);
    const auto *pad = FindKey(padCode);
    binding =
        MakeBinding(code, key ? key->icon : "", padCode, pad ? pad->icon : "");
  };
  const auto &pad = values.gamepad;
  set(openMenu, values.menuKey, pad.menuKey);
  set(secondaryAction, values.secondaryBinding, pad.secondaryBinding);
  set(fineConfirm, values.fineCompletionBindings[0], pad.fineCompletionBindings[0]);
  set(fineCancel, values.fineCompletionBindings[1], pad.fineCompletionBindings[1]);
  set(rotateCounterclockwise, values.counterclockwiseBinding,
      pad.counterclockwiseBinding);
  for (std::size_t i = 0; i < modifyControls.size(); ++i)
    set(modifyControls[i], values.modifyBindings[i], pad.modifyBindings[i]);
  const std::array<Binding *, 11> bindings{
      &scrollLeft,   &scrollRight,    &categoryLeft,   &categoryRight,
      &packPrevious, &packNext,       &rotate,         &placeAction,
      &switchMode,   &toggleSnapping, &fineAdjustments};
  for (std::size_t i = 0; i < bindings.size(); ++i)
    set(*bindings[i], values.bindings[i], pad.bindings[i]);
  for (std::size_t i = 0; i < fineControls.size(); ++i)
    set(fineControls[i], values.fineBindings[i], pad.fineBindings[i]);
  set(historyUndo, values.historyBindings[0], pad.historyBindings[0]);
  set(historyRedo, values.historyBindings[1], pad.historyBindings[1]);
  set(brushIncrease, values.brushBindings[0], pad.brushBindings[0]);
  set(brushDecrease, values.brushBindings[1], pad.brushBindings[1]);
  PlacementPreview::SetOpacity(values.previewOpacity);
  PlacementPreview::SetTint(
      {values.previewTint[0], values.previewTint[1], values.previewTint[2]});
  ModifyMode::SetOpacity(values.modifyOpacity);
  ModifyMode::SetTint(
      {values.moveTint[0], values.moveTint[1], values.moveTint[2]});
  UI::SetOpacity(values.uiOpacity);
  UI::Widgets::SetHUDBackgroundOpacity(values.hudBackgroundOpacity);
  UI::SetIconAppearance(values.iconOpacity, values.iconBrightness);
  UI::RefreshControlBindings();
}
nlohmann::json BindingJSON(const Bindings &b) {
  return {{"mainMenuKey", b.menuKey},
          {"secondaryAction", b.secondaryBinding},
          {"fineCompletionBindings", b.fineCompletionBindings},
          {"rotateCounterclockwise", b.counterclockwiseBinding},
          {"modifyBindings", b.modifyBindings},
          {"buildingBindings", b.bindings},
          {"fineBindings", b.fineBindings},
          {"historyBindings", b.historyBindings},
          {"brushBindings", b.brushBindings}};
}
void ReadBindings(const nlohmann::json &json, Bindings &b) {
  b.menuKey = json.value("mainMenuKey", b.menuKey);
  b.secondaryBinding = json.value("secondaryAction", b.secondaryBinding);
  b.counterclockwiseBinding =
      json.value("rotateCounterclockwise", b.counterclockwiseBinding);
  const auto array = [&](const char *name, auto &target) {
    if (!json.contains(name))
      return;
    const auto &value = json.at(name);
    if (!value.is_array() || value.size() != target.size())
      throw std::runtime_error("Invalid binding count");
    value.get_to(target);
  };
  array("modifyBindings", b.modifyBindings);
  array("buildingBindings", b.bindings);
  array("fineCompletionBindings", b.fineCompletionBindings);
  if (json.contains("fineBindings") && json.at("fineBindings").size() == 12) {
    auto fine = json.at("fineBindings").get<std::vector<std::uint32_t>>();
    fine.insert(fine.begin() + 10, 2, 0);
    const auto roll = std::array{b.fineBindings[10], b.fineBindings[11]};
    std::copy(fine.begin(), fine.end(), b.fineBindings.begin());
    for (int i = 0; i < 2; ++i)
      if (roll[i] && std::ranges::find(fine, roll[i]) == fine.end() &&
          std::ranges::find(b.fineCompletionBindings, roll[i]) ==
              b.fineCompletionBindings.end())
        b.fineBindings[10 + i] = roll[i];
  } else {
    array("fineBindings", b.fineBindings);
  }
  array("historyBindings", b.historyBindings);
  array("brushBindings", b.brushBindings);
  if (!json.contains("fineCompletionBindings")) {
    for (int i = 0; i < 2; ++i)
      AssignBinding(b, FineCompletionBindingOffset + i, b.fineCompletionBindings[i]);
  }

}
bool KnownBinding(std::uint32_t code) {
  return FindKey(code) != nullptr;
}
void Save() {
  try {
    std::filesystem::create_directories(Paths::Root());
    auto path = Path();
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary);
    auto json = nlohmann::json{
        {"terrainStrengthPerSecond", draft.terrainStrengthPerSecond},
        {"sandboxMode", draft.sandboxMode},
        {"buildingCostMultiplier", draft.buildingCostMultiplier},
        {"gamepadBindings", BindingJSON(draft.gamepad)},
        {"quickSelectMainMenu", draft.quickSelectMainMenu},
        {"quickSelectToolMenu", draft.quickSelectToolMenu},
        {"uiOpacity", draft.uiOpacity},
        {"hudBackgroundOpacity", draft.hudBackgroundOpacity},
        {"previewOpacity", draft.previewOpacity},
        {"modifyOpacity", draft.modifyOpacity},
        {"iconOpacity", draft.iconOpacity},
        {"iconBrightness", draft.iconBrightness},
        {"previewTint", draft.previewTint},
        {"moveTint", draft.moveTint}};
    json.update(BindingJSON(draft));
    output << json.dump(2);
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING)) {
      status = "Could not save settings.";
      spdlog::warn("Could not save SkyBuild settings.");
    } else {
      status = "Settings saved.";
    }
  } catch (const std::exception &error) {
    status = "Could not save settings.";
    spdlog::warn("Settings save failed: {}", error.what());
  }
}
void CommitChanges(bool changed) {
  static bool dirty = false;
  if (changed) {
    dirty = true;
    SKSE::GetTaskInterface()->AddTask([values = draft] { Apply(values); });
  }
  if (dirty && !ImGui::IsAnyItemActive()) {
    Save();
    dirty = false;
  }
  if (!status.empty())
    ImGui::TextUnformatted(status.c_str());
}
void __stdcall RenderKeybinds() {
  bool changed = false;
  bool openRebind = false;
  const auto captured = capturedCode.exchange(0);
  if (captured && captureAction >= 0) {
    if (captured == 1)
      status = "Rebinding cancelled.";
    else if ((captured >= 266 && captured < 282) != captureGamepad.load())
      status = "Use a control from the selected column.";
    else if (!KnownBinding(captured))
      status = "That key is not supported.";
    else {
      auto &bindings =
          captureGamepad ? draft.gamepad : static_cast<Bindings &>(draft);
      const auto before = bindings;
      AssignBinding(bindings, captureAction, captured);
      if (!ValidateBindings(bindings, KnownBinding)) {
        bindings = before;
        status = "That key conflicts with another action in this context.";
      } else {
        changed = true;
        status.clear();
      }
    }
    captureAction = -1;
  }
  if (!capturing.load() && !captured)
    captureAction = -1;
  Fields::Keybinds(
      draft,
      [](std::uint32_t code) {
        const auto *key = FindKey(code);
        return std::string(code == 0 ? "Unbound"
                           : key     ? key->label
                                     : "Unknown");
      },
      [&](int action, bool gamepad) {
        if (action <= -2) {
          auto &bindings =
              gamepad ? draft.gamepad : static_cast<Bindings &>(draft);
          BindingAt(bindings, -2 - action) = 0;
          changed = true;
          return;
        }
        captureAction = action;
        captureGamepad = gamepad;
        capturedCode.store(0);
        heldCode.store(0);
        capturing.store(true);
        openRebind = true;
      },
      [](std::uint32_t code) -> std::string_view {
        const auto *key = FindKey(code);
        return key ? key->icon : "";
      });
  // Open the popup outside the table's ID scope so its capture modal
  // retains the same identity as the BeginPopupModal call below.
  if (openRebind) {
    ImGui::GetIO().ClearInputKeys();
    ImGui::OpenPopup("Rebind control");
  }
  if (ImGui::BeginPopupModal("Rebind control", nullptr,
                             ImGui::ImGuiWindowFlags_AlwaysAutoResize)) {
    if (capturing.load()) {
      ImGui::TextUnformatted(
          captureGamepad
              ? "Press and release a gamepad button. Escape cancels."
              : "Press a key, mouse button, or scroll direction. Escape cancels.");
      if (captureGamepad.load()) {
        const auto *key = FindKey(heldCode.load());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.F);
        ImGui::TextWrapped("%s", key ? key->label : "Waiting for input...");
        ImGui::PopTextWrapPos();
      }
    } else {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (UI::Widgets::FormButton("Reset Mouse & Keyboard")) {
    static_cast<Bindings &>(draft) = Bindings{};
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Gamepad")) {
    draft.gamepad = GamepadDefaults();
    changed = true;
  }
  CommitChanges(changed);
}
void __stdcall RenderVisuals() { CommitChanges(Fields::Visuals(draft)); }
void RenderIconRegeneration() {
  {
    std::scoped_lock lock(iconPacksMutex);
    if (pendingIconPacks) {
      iconPacks = std::move(*pendingIconPacks);
      pendingIconPacks.reset();
      ++iconPacksRevision;
    }
  }
  // Rebuild immutable file snapshots only when the editor reloads packs.
  static std::uint64_t filesRevision = ~0ULL;
  static std::shared_ptr<const std::vector<
      std::vector<std::pair<RE::FormID, std::filesystem::path>>>>
      files;
  if (filesRevision != iconPacksRevision) {
    files = std::make_shared<const std::vector<
        std::vector<std::pair<RE::FormID, std::filesystem::path>>>>([] {
      std::vector<std::vector<std::pair<RE::FormID, std::filesystem::path>>>
          result;
      for (const auto &pack : iconPacks) {
        auto &entries = result.emplace_back();
        for (std::size_t i = 0; i < pack.forms.size(); ++i)
          entries.emplace_back(pack.forms[i], pack.iconPaths[i]);
      }
      return result;
    }());
    filesRevision = iconPacksRevision;
  }
  static std::uint64_t missingCheckRevision = 0;
  static std::future<std::vector<std::vector<RE::FormID>>> missingCheck;
  if (missingCheck.valid() && missingCheck.wait_for(std::chrono::seconds{0}) ==
                                  std::future_status::ready) {
    try {
      auto counts = missingCheck.get();
      for (std::size_t i = 0;
           missingCheckRevision == iconPacksRevision && i < iconPacks.size();
           ++i) {
        iconPacks[i].missingForms = std::move(counts[i]);
        iconPacks[i].missing = iconPacks[i].missingForms.size();
      }
    } catch (const std::exception &error) {
      spdlog::warn("Missing icon scan failed: {}", error.what());
    }
  }
  const auto refreshMissing = [] {
    if (missingCheck.valid())
      return;
    missingCheckRevision = iconPacksRevision;
    missingCheck = std::async(std::launch::async, [snapshot = files] {
      std::vector<std::vector<RE::FormID>> result;
      for (const auto &pack : *snapshot) {
        auto &missing = result.emplace_back();
        for (const auto &[id, path] : pack) {
          std::error_code error;
          if (path.empty() || !std::filesystem::is_regular_file(path, error))
            missing.push_back(id);
        }
      }
      return result;
    });
  };
  static std::chrono::steady_clock::time_point nextRefresh{};
  const auto now = std::chrono::steady_clock::now();
  if (now >= nextRefresh) {
    refreshMissing();
    nextRefresh = now + std::chrono::seconds(5);
  }
  std::size_t totalMissing = 0;
  for (const auto &pack : iconPacks)
    totalMissing += pack.missing;
  const auto buttonLabel =
      totalMissing
          ? std::format("Regenerate Icons ({} Missing)###RegenerateIcons",
                        totalMissing)
          : std::string("Regenerate Icons###RegenerateIcons");
  if (ImGui::Button(buttonLabel.c_str())) {
    refreshMissing();
    for (auto &pack : iconPacks)
      pack.selected = true;
    regenerationProgress = UI::ThumbnailGenerator::GetProgress().running;
    expectedGeneration = UI::ThumbnailGenerator::GetProgress().generation;
    ImGui::OpenPopup("Regenerate Icons");
  }
  ImGui::SetNextWindowSize({600.0F, 0.0F}, ImGuiCond_Always);
  bool open = true;
  if (!ImGui::BeginPopupModal("Regenerate Icons",
                              regenerationProgress ? nullptr : &open,
                              ImGui::ImGuiWindowFlags_AlwaysAutoResize))
    return;
  if (!open || UI::Widgets::EscapePressed()) {
    if (UI::ThumbnailGenerator::GetProgress().running)
      SKSE::GetTaskInterface()->AddTask(
          [] { UI::ThumbnailGenerator::Cancel(); });
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  if (regenerationProgress) {
    DrawGenerationProgress();
    const auto progress = UI::ThumbnailGenerator::GetProgress();
    if (progress.running || progress.generation < expectedGeneration)
      DrawGenerationCancel();
    else {
      if (ImGui::Button("Back to packs"))
        regenerationProgress = false;
      ImGui::SameLine();
      if (ImGui::Button("Close"))
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return;
  }
  UI::Widgets::Section("Material resources");
  if (ImGui::Button("Regenerate Material Resources")) {
    SKSE::GetTaskInterface()->AddTask(
        [] { UI::PackEditor::RegenerateResourceIcons(); });
    const auto progress = UI::ThumbnailGenerator::GetProgress();
    expectedGeneration = progress.generation + (progress.running ? 0 : 1);
    regenerationProgress = true;
  }
  std::size_t selectedCount = 0, selectedMissingCount = 0;
  for (const auto &pack : iconPacks)
    if (pack.selected) {
      selectedCount += pack.forms.size();
      selectedMissingCount += pack.missingForms.size();
    }
  const auto regenerate = [](bool missingOnly, bool allPacks) {
    std::vector<UI::ThumbnailGenerator::IconRequest> selectedForms;
    for (const auto &pack : iconPacks)
      if (allPacks || pack.selected) {
        const std::unordered_set<RE::FormID> missing(pack.missingForms.begin(),
                                                     pack.missingForms.end());
        for (std::size_t i = 0; i < pack.forms.size(); ++i) {
          if ((!missingOnly || missing.contains(pack.forms[i])) &&
              i < pack.iconPaths.size())
            selectedForms.emplace_back(
                pack.forms[i],
                pack.iconPaths[i]
                    .lexically_relative(std::filesystem::current_path() /
                                        "Data/SkyBuild/UI")
                    .generic_string());
        }
      }
    if (selectedForms.empty())
      return;
    status = std::format(
        "Queued {} icon(s) for generation. You can keep this menu open.",
        selectedForms.size());
    SKSE::GetTaskInterface()->AddTask([forms = std::move(selectedForms)] {
      UI::ThumbnailGenerator::QueueRegeneration(forms);
    });
    const auto progress = UI::ThumbnailGenerator::GetProgress();
    expectedGeneration = progress.generation + (progress.running ? 0 : 1);
    regenerationProgress = true;
  };
  const auto allMissingLabel = std::format(
      "Generate All {} Missing Icons###GenerateAllMissing", totalMissing);
  UI::Widgets::Section("Placeable packs");
  ImGui::BeginDisabled(totalMissing == 0);
  if (ImGui::Button(allMissingLabel.c_str(), {-1.0F, 0.0F}) && totalMissing)
    regenerate(true, true);
  ImGui::EndDisabled();
  UI::Widgets::SpaceBefore(UI::Widgets::Gap::Section);
  ImGui::Separator();
  UI::Widgets::SpaceBefore();
  ImGui::BeginDisabled(selectedCount == 0);
  if (ImGui::Button("Regenerate Selected Packs") && selectedCount)
    regenerate(false, false);
  ImGui::EndDisabled();
  ImGui::SameLine();
  const auto selectedMissingLabel =
      std::format("Generate {} Missing Icons###RegenerateSelectedMissing",
                  selectedMissingCount);
  ImGui::BeginDisabled(selectedMissingCount == 0);
  if (ImGui::Button(selectedMissingLabel.c_str()) && selectedMissingCount)
    regenerate(true, false);
  ImGui::EndDisabled();
  if (iconPacks.empty())
    ImGui::TextUnformatted("No placeable packs are available.");
  UI::Widgets::SpaceBefore();
  ImGui::BeginChild(
      "Icon packs",
      {0.0F,
       std::min(240.F, std::max(ImGui::GetFrameHeight(),
                                iconPacks.size() *
                                    ImGui::GetFrameHeightWithSpacing()))});
  for (std::size_t i = 0; i < iconPacks.size(); ++i) {
    auto &pack = iconPacks[i];
    const auto name =
        pack.missing ? std::format("{} ({} Missing)", pack.name, pack.missing)
                     : pack.name;
    const auto label = std::format("{}###RegeneratePack{}", name, i);
    ImGui::Checkbox(label.c_str(), &pack.selected);
  }
  ImGui::EndChild();
  if (ImGui::Button("Select All"))
    for (auto &pack : iconPacks)
      pack.selected = true;
  ImGui::SameLine();
  if (ImGui::Button("Select None"))
    for (auto &pack : iconPacks)
      pack.selected = false;
  ImGui::EndPopup();
}
void __stdcall RenderUI() {
  CommitChanges(Fields::Appearance(draft));
  UI::Widgets::Section("Icon previews",
                       "Generate previews for new or changed placeables.");
  RenderIconRegeneration();
}
} // namespace
void ShowIconGenerationProgress(bool autoClose) {
  const auto progress = UI::ThumbnailGenerator::GetProgress();
  autoCloseGeneration.store(autoClose && progress.running ? progress.generation
                                                          : 0);
  if (progressWindow)
    progressWindow->IsOpen.store(true);
}
bool CaptureInput(RE::InputEvent *const *events) {
  if (!events ||
      (!capturing.load() && heldCode.load() == 0 && capturedCode.load() == 0))
    return false;
  // Shield the entire batch, including Escape and the framework toggle key.
  // Keep shielding until the render callback acknowledges the released key.
  for (auto event = *events; event; event = event->next) {
    ControlBindings::ObserveEvent(*event);
    CaptureButton(event);
  }
  return true;
}
void SetMenuOpen(bool open) {

  if (const auto main = SkyBuild::UI::Native::GetMainWindow())
    main->IsOpen.store(open);
}
bool IsMenuOpen() {

  const auto main = SkyBuild::UI::Native::GetMainWindow();
  return (main && main->IsOpen.load()) ||
         SkyBuild::UI::Native::IsAnyBlockingWindowOpened();
}
void RefreshIconPacks() {
  std::vector<IconPack> fresh;
  for (const auto &placeable : PlacementPreview::GetPlaceables()) {
    auto found =
        std::find_if(fresh.begin(), fresh.end(), [&](const auto &pack) {
          return pack.name == placeable.packName;
        });
    if (found == fresh.end()) {
      fresh.push_back({std::string(placeable.packName), {}});
      found = std::prev(fresh.end());
    }
    found->forms.push_back(placeable.formID);
    const auto relativePath = UI::ThumbnailGenerator::GetRelativePath(
        placeable.formID, placeable.packName);
    found->iconPaths.push_back(relativePath
                                   ? std::filesystem::current_path() /
                                         "Data/SkyBuild/UI" / *relativePath
                                   : std::filesystem::path{});
  }

  std::scoped_lock lock(iconPacksMutex);
  pendingIconPacks = std::move(fresh);
}

void __stdcall RenderGameplay() { CommitChanges(Fields::Gameplay(draft)); }

void __stdcall RenderTerrain() { CommitChanges(Fields::Brush(draft)); }

void __stdcall RenderAdvanced() {
  Fields::Advanced(Navmesh::DebugVisualization::RenderNavigationSettings);
}

bool SandboxMode() { return sandboxMode.load(); }
float BuildingCostMultiplier() { return buildingCostMultiplier.load(); }
bool QuickSelectMainMenu() { return quickMain.load(); }
bool QuickSelectToolMenu() { return quickTools.load(); }

void Initialize() {
  RefreshIconPacks();
  try {
    std::ifstream input(Path());
    if (input) {
      const auto json = nlohmann::json::parse(input);
      Values loaded;
      ReadBindings(json, loaded);
      if (json.contains("gamepadBindings"))
        ReadBindings(json.at("gamepadBindings"), loaded.gamepad);
      if (!ValidateBindings(loaded.gamepad, [](auto code) {
            return KnownBinding(code) && code >= 266 && code < 282;
          }))
        loaded.gamepad = GamepadDefaults();
      loaded.sandboxMode = json.value("sandboxMode", false);
      loaded.buildingCostMultiplier = json.value("buildingCostMultiplier", 1.F);
      loaded.quickSelectMainMenu = json.value("quickSelectMainMenu", false);
      loaded.quickSelectToolMenu = json.value("quickSelectToolMenu", true);
      loaded.terrainStrengthPerSecond = json.value(
          "terrainStrengthPerSecond", Terrain::BrushStrengthPerSecond);
      if (loaded.terrainStrengthPerSecond > Terrain::MaxBrushStrengthPerSecond)
        loaded.terrainStrengthPerSecond = Terrain::BrushStrengthPerSecond;
      loaded.uiOpacity = json.value("uiOpacity", 1.0F);
      loaded.hudBackgroundOpacity =
          json.value("hudBackgroundOpacity", Values{}.hudBackgroundOpacity);
      loaded.modifyOpacity =
          json.value("modifyOpacity", Values{}.modifyOpacity);
      loaded.previewOpacity = json.value("previewOpacity", 0.5F);
      loaded.iconOpacity = json.value("iconOpacity", 0.5F);
      loaded.iconBrightness = json.value("iconBrightness", 1.0F);
      loaded.previewTint = json.value("previewTint", loaded.previewTint);
      loaded.moveTint = json.value("moveTint", loaded.moveTint);
      if (!ValidateAndNormalize(loaded, [](auto code) {
            return KnownBinding(code) && code < 266;
          })) {
        throw std::runtime_error(
            "Invalid settings values or duplicate bindings");
      }
      draft = loaded;
    }
  } catch (const std::exception &error) {
    spdlog::warn("Using default SkyBuild settings: {}", error.what());
  }
  Apply(draft);
  {
    menuSubscription =
        std::make_unique<SkyBuild::UI::Native::Model::Event>(MenuEvent, 0.0F);
    progressWindow =
        SkyBuild::UI::Native::AddWindow(RenderGenerationProgressWindow);
    SkyBuild::UI::Native::SetSection("SkyBuild");
    SkyBuild::UI::Native::AddSectionItem("Gameplay", RenderGameplay);
    SkyBuild::UI::Native::AddSectionItem("UI", RenderUI);
    SkyBuild::UI::Native::AddSectionItem("Visuals", RenderVisuals);
    SkyBuild::UI::Native::AddSectionItem("Terrain", RenderTerrain);
    SkyBuild::UI::Native::AddSectionItem("Keybinds", RenderKeybinds);
    SkyBuild::UI::Native::AddSectionItem("Advanced", RenderAdvanced);
    spdlog::info("Registered standalone SkyBuild settings.");
  }
}
} // namespace SkyBuild::Settings
