#include "hooks/InputHook.h"
#include "activators/Connections.h"
#include "building/BuildingHistory.h"
#include "building/KeyRepeat.h"
#include "hooks/FilteredInputBatch.h"
#include "ui/imgui/InputOwnership.h"

#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "building/ModifyMode.h"
#include "building/PickerMode.h"
#include "building/PlacementPreview.h"
#include "terrain/TerrainTool.h"
#include "ui/fine-adjustments/FineAdjustments.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/UI.h"
#include "ui/settings/Settings.h"
#include "ui/pack-editor/PackEditor.h"

namespace SkyBuild::Hooks {
namespace {
using DispatchInput = void (*)(RE::BSTEventSource<RE::InputEvent *> *,
                               RE::InputEvent *const *);
REL::Relocation<DispatchInput> originalDispatch;
bool ConsumeToolEvent(RE::InputEvent *event);
UI::InputDelivery delivery;
void Dispatch(RE::BSTEventSource<RE::InputEvent *> *source,
              RE::InputEvent *const *events) {
  const bool wasGamepad = ControlBindings::gamepadInUse.load();
  const bool rebinding = Settings::CaptureInput(events);
  FilteredInputBatch<RE::InputEvent> filtered(
      events ? *events : nullptr, [rebinding](RE::InputEvent *event) {
        if (!rebinding)
          ControlBindings::ObserveEvent(*event);
        const bool consume = rebinding ||
                             UI::Native::CaptureInputEvent(event) ||
                             ConsumeToolEvent(event);
        const auto button = event->AsButtonEvent();
        if (!button)
          return consume;
        const bool mouse = button->GetDevice() == RE::INPUT_DEVICE::kMouse;
        if ((!mouse && button->GetDevice() != RE::INPUT_DEVICE::kKeyboard &&
             button->GetDevice() != RE::INPUT_DEVICE::kGamepad) ||
            (mouse && (button->GetIDCode() == 8 || button->GetIDCode() == 9)))
          return consume;
        return delivery.Consume(ControlBindings::InputCode(*button),
                                button->IsDown(), button->IsUp(), consume);
      });
  if (wasGamepad != ControlBindings::gamepadInUse.load()) {
    UI::RefreshControlBindings();
    UI::FineAdjustments::Update();
  }
  originalDispatch(source, &filtered.head);
}
bool HandleButton(RE::ButtonEvent *button) {
  using namespace ControlBindings;
  const auto matches = [button](const Binding &binding) {
    return ControlBindings::Matches(*button, binding);
  };
  if (UI::FineAdjustments::IsOpen())
    return UI::FineAdjustments::HandleButton(button);
  if (BuildingMode::GetTool() == BuildingMode::InteractionMode::kPicker &&
      (matches(secondaryAction) ||
       (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
        button->GetIDCode() == RE::BSKeyboardDevice::Key::kEscape))) {
    if (button->IsDown())
      UI::PackEditor::CancelSelection();
    return true;
  }
  if (matches(switchMode)) {
    if (button->IsDown())
      UI::OpenToolWheel();
    else if (button->IsUp())
      UI::FinishToolWheel();
    return true;
  }
  if (UI::IsToolWheelOpen() && !matches(switchMode)) {
    if (matches(secondaryAction)) {
      if (button->IsDown())
        UI::CancelToolWheel();
      return true;
    }
    if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
        button->GetIDCode() == RE::BSKeyboardDevice::Key::kEscape &&
        button->IsDown())
      UI::CancelToolWheel();
    return true;
  }
  if (Activators::IsConnecting())
    return Activators::HandleButton(button);
  if (!BuildingMode::IsActive())
    return false;
  if (BuildingMode::GetTool() == BuildingMode::InteractionMode::kPicker) {
    if (matches(placeAction)) {
      if (button->IsDown())
        PickerMode::Use();
      return true;
    }
    return false;
  }
  if (BuildingMode::GetTool() == BuildingMode::InteractionMode::kModify) {
    if (matches(secondaryAction)) {
      if (button->IsDown()) {
        if (PlacementPreview::IsMovingObject())
          PlacementPreview::CancelMove();
        else
          ModifyMode::Break();
      }
      return true;
    }
    if (!PlacementPreview::IsMovingObject())
    for (std::size_t i = 0; i < modifyControls.size(); ++i)
      if (matches(modifyControls[i])) {
        if (button->IsDown() && !PlacementPreview::IsMovingObject()) {
          if (i == 0)
            ModifyMode::Copy();
          else
            ModifyMode::EditProperties();
        }
        return true;
      }
  }
  if (matches(historyUndo) || matches(historyRedo)) {
    if (button->IsDown()) {
      if (matches(historyUndo))
        BuildingHistory::Undo();
      else
        BuildingHistory::Redo();
    }
    return true;
  }

  if (!UI::IsToolWheelOpen() && PlacementPreview::IsMovingObject() &&
      button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
      button->GetIDCode() == RE::BSKeyboardDevice::Key::kEscape) {
    if (button->IsDown())
      PlacementPreview::CancelMove();
    return true;
  }
  if (matches(placeAction)) {
    if (button->IsDown()) {
      if (BuildingMode::IsBuildMode())
        PlacementPreview::PlacePreview();
      else if (BuildingMode::GetTool() ==
               BuildingMode::InteractionMode::kModify)
        ModifyMode::Use();
    }
    return true;
  }
  if (matches(fineAdjustments)) {
    if (button->IsDown() &&
        (BuildingMode::IsBuildMode() || PlacementPreview::IsMovingObject()))
      UI::FineAdjustments::Open();
    return true;
  }
  if (matches(rotate) || matches(rotateCounterclockwise)) {
    static std::array<KeyRepeat, 2> repeats;
    const bool clockwise = matches(rotate);
    if (repeats[clockwise ? 0 : 1].Pulse(button->IsDown(), button->IsPressed(),
                                         button->HeldDuration()) &&
        (BuildingMode::IsBuildMode() || PlacementPreview::IsMovingObject()))
      PlacementPreview::RotateSelectedPlaceable(clockwise);
    return true;
  }
  const bool buildMode = BuildingMode::IsBuildMode();
  if (matches(toggleSnapping)) {
    if (button->IsDown() && (buildMode || PlacementPreview::IsMovingObject()))
      PlacementPreview::ToggleSnapping();
    return true;
  }
  if (matches(packPrevious) || matches(packNext)) {
    if (button->IsDown() && buildMode)
      PlacementPreview::SelectPack(matches(packPrevious) ? -1 : 1);
    return true;
  }
  if (matches(categoryLeft) || matches(categoryRight)) {
    static std::array<KeyRepeat, 2> repeats;
    if (repeats[matches(categoryLeft) ? 0 : 1].Pulse(
            button->IsDown(), button->IsPressed(), button->HeldDuration()) &&
        buildMode)
      PlacementPreview::SelectCategory(matches(categoryLeft) ? -1 : 1);
    return true;
  }
  if (matches(scrollLeft) || matches(scrollRight)) {
    static std::array<KeyRepeat, 2> repeats;
    if (repeats[matches(scrollLeft) ? 0 : 1].Pulse(
            button->IsDown(), button->IsPressed(), button->HeldDuration()) &&
        buildMode) {
      if (matches(scrollLeft))
        PlacementPreview::SelectPreviousPlaceableObject();
      else
        PlacementPreview::SelectNextPlaceableObject();
    }
    return true;
  }
  return false;
}

} // namespace

void InputHook::Install() {
  const auto inputManager = RE::BSInputDeviceManager::GetSingleton();
  if (!inputManager) {
    spdlog::error("Could not register SkyBuild input handling.");
    return;
  }

  // Install at DataLoaded and chain existing hooks. Interactive SkyBuild
  // windows consume the batch before gameplay receives it.
  const auto callSite =
      REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B);
  // write_call<5> assumes a five-byte relative call when decoding the original
  // destination. Reject a moved call site or an incompatible hook encoding
  // before writing anything.
  if (*reinterpret_cast<const std::uint8_t *>(callSite) != 0xE8) {
    spdlog::error(
        "SkyBuild input hook rejected unexpected instruction at "
        "runtime {} input call site. Input handling was not installed.",
        REL::Module::get().version().string());
    return;
  }
  originalDispatch = SKSE::GetTrampoline().write_call<5>(callSite, Dispatch);
  spdlog::info("Registered SkyBuild input handling.");
}

namespace {
bool ConsumeToolEvent(RE::InputEvent *event) {
  if ((!BuildingMode::IsHammerReady() && !Terrain::Ready()) ||
      UI::IsGameMenuBlocking(true))
    return false;
  if (UI::IsToolWheelOpen())
    if (const auto motion = event->AsMouseMoveEvent()) {
      UI::MoveToolWheel(static_cast<float>(motion->mouseInputX),
                        static_cast<float>(motion->mouseInputY));
      return true;
    }
  const auto button = event->AsButtonEvent();
  if (!button)
    return false;
  const auto actions = RE::UserEvents::GetSingleton();
  const auto action = button->QUserEvent();
  const bool attack = actions && (action == actions->leftAttack ||
                                  action == actions->rightAttack ||
                                  action == actions->dualAttack);
  return (Terrain::Ready() ? Terrain::HandleButton(button)
                           : HandleButton(button)) ||
         (BuildingMode::IsActive() && actions && action == actions->activate) ||
         (attack &&
          (Terrain::Ready() ? Terrain::GetTool() != Terrain::Tool::Idle
                            : BuildingMode::IsActive()));
}
} // namespace
} // namespace SkyBuild::Hooks
