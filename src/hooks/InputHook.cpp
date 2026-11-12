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
#include "ui/settings/SettingsValues.h"
#include "ui/pack-editor/PackEditor.h"

namespace SkyBuild::Hooks {
namespace {
using DispatchInput = void (*)(RE::BSTEventSource<RE::InputEvent *> *,
                               RE::InputEvent *const *);
REL::Relocation<DispatchInput> originalDispatch;
bool ConsumeToolEvent(RE::InputEvent *event);
unsigned UpdateBindingScope(bool rebinding);
UI::InputDelivery delivery;
void Dispatch(RE::BSTEventSource<RE::InputEvent *> *source,
              RE::InputEvent *const *events) {
  const bool wasGamepad = ControlBindings::gamepadInUse.load();
  const bool rebinding = Settings::CaptureInput(events);
  UpdateBindingScope(rebinding);
  FilteredInputBatch<RE::InputEvent> filtered(
      events ? *events : nullptr, [rebinding](RE::InputEvent *event) {
        if (!rebinding)
          ControlBindings::ObserveEvent(*event);
        const bool uiConsumed = !rebinding && UI::Native::CaptureInputEvent(event);
        const bool toolConsumed = !rebinding && ConsumeToolEvent(event);
        const bool consume = rebinding || toolConsumed || uiConsumed;
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
bool HandleButton(const ControlBindings::Event *button) {
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
using namespace ControlBindings;
std::array<Binding *, Settings::BindingCount> RuntimeBindings() {
  return {&scrollLeft, &scrollRight, &categoryLeft, &categoryRight,
          &packPrevious, &packNext, &rotate, &placeAction, &switchMode,
          &toggleSnapping, &fineAdjustments,
          &fineControls[0], &fineControls[1], &fineControls[2], &fineControls[3],
          &fineControls[4], &fineControls[5], &fineControls[6], &fineControls[7],
          &fineControls[8], &fineControls[9], &fineControls[10], &fineControls[11],
          &fineControls[12], &fineControls[13], &historyUndo, &historyRedo,
          &openMenu, &brushIncrease, &brushDecrease, &modifyControls[0],
          &modifyControls[1], &secondaryAction, &rotateCounterclockwise,
          &fineConfirm, &fineCancel};
}
Input::ChordResolver chords;
unsigned CurrentScope() {
  using namespace Settings;
  if ((!BuildingMode::IsHammerReady() && !Terrain::Ready()) ||
      UI::IsGameMenuBlocking(true))
    return 0;
  if (UI::FineAdjustments::IsOpen()) return ScopeFine;
  if (UI::IsToolWheelOpen()) return ScopeWheel;
  if (Terrain::Ready())
    return Terrain::GetTool() == Terrain::Tool::Idle ? ScopeIdle : ScopeTerrain;
  if (PlacementPreview::IsMovingObject()) return ScopeMove;
  if (BuildingMode::GetTool() == BuildingMode::InteractionMode::kPicker) return ScopePicker;
  if (BuildingMode::GetTool() == BuildingMode::InteractionMode::kModify) return ScopeModify;
  return BuildingMode::IsBuildMode() ? ScopeBuild : ScopeIdle;
}
void DispatchAction(const Input::ActionEvent &action) {
  const auto bindings = RuntimeBindings();
  const auto *binding = action.action < Settings::BindingCount * 2
                            ? bindings[action.action % Settings::BindingCount]
                            : action.action == Settings::BindingCount * 2 ? &placeAction : &secondaryAction;
  const Event event{binding, action.terminal, action.phase, action.duration};
  if (Matches(event, openMenu) || Matches(event, switchMode)) {
    if (event.IsDown()) {
      if (Terrain::Ready()) Terrain::SuspendInput();
      if (Matches(event, openMenu)) UI::OpenMainMenu();
      else UI::OpenToolWheel();
    } else if (event.IsUp()) UI::FinishToolWheel();
    return;
  }
  if (UI::IsToolWheelOpen()) {
    if (event.IsDown()) {
      if (Matches(event, secondaryAction)) UI::CancelToolWheel();
      else if (Matches(event, placeAction)) UI::FinishToolWheel(true);
    }
    // A tool-menu modifier may release during a terrain stroke.
    if (event.IsUp() && Terrain::Ready()) Terrain::SuspendInput();
    return;
  }
  if (Terrain::Ready()) Terrain::HandleButton(&event);
  else HandleButton(&event);
}
unsigned UpdateBindingScope(bool rebinding) {
  static std::uint64_t appliedRevision = ~std::uint64_t{0};
  if (const auto version = revision.load(); version != appliedRevision) {
    if (Terrain::Ready()) Terrain::SuspendInput();
    std::vector<Input::ChordBinding> entries;
    const auto bindings = RuntimeBindings();
    for (bool pad : {false, true})
      for (int i = 0; i < Settings::BindingCount; ++i)
        entries.push_back({(pad ? bindings[i]->gamepad : bindings[i]->keyboardMouse).chord,
                           Settings::BindingScopes(i)});
    // A/Cross and B/Circle also participate in modifier deferral in the wheel.
    for (const auto key : {276U, 277U}) {
      const bool assigned = std::ranges::any_of(entries, [&](const auto &entry) {
        return entry.chord == key && (entry.scopes & Settings::ScopeWheel);
      });
      entries.push_back({key, assigned ? 0U : Settings::ScopeWheel});
    }
    chords.Configure(std::move(entries));
    appliedRevision = version;
  }
  const auto scope = rebinding ? 0U : CurrentScope();
  static unsigned previousScope{};
  if (!scope && previousScope && Terrain::Ready()) Terrain::SuspendInput();
  previousScope = scope;
  chords.SetScope(scope, [](const Input::ActionEvent &event) {
    // Context changes cancel holds; they must not confirm a wheel selection.
    if (event.phase == Input::Phase::Up && Terrain::Ready()) Terrain::SuspendInput();
  });
  return scope;
}
bool ConsumeToolEvent(RE::InputEvent *event) {
  if (!UpdateBindingScope(false)) return false;
  if (UI::IsToolWheelOpen())
    if (const auto motion = event->AsMouseMoveEvent()) {
      UI::MoveToolWheel(static_cast<float>(motion->mouseInputX),
                        static_cast<float>(motion->mouseInputY));
      return true;
    }
  const auto button = event->AsButtonEvent();
  if (!button) return false;
  const auto code = InputCode(*button);
  const auto phase = button->IsDown() ? Input::Phase::Down :
                     button->IsUp() ? Input::Phase::Up : Input::Phase::Held;
  const bool matched = chords.Process(code, phase, button->HeldDuration(), DispatchAction);
  bool escape = false;
  if (code == 1) {
    const Event raw{nullptr, code, phase, button->HeldDuration()};
    if (UI::IsToolWheelOpen()) {
      if (button->IsDown()) UI::CancelToolWheel();
      escape = true;
    } else escape = Terrain::Ready() ? Terrain::HandleButton(&raw) : HandleButton(&raw);
  }
  const auto actions = RE::UserEvents::GetSingleton();
  const auto action = button->QUserEvent();
  const bool attack = actions && (action == actions->leftAttack ||
                                  action == actions->rightAttack ||
                                  action == actions->dualAttack);
  return matched || escape || UI::IsToolWheelOpen() ||
         (BuildingMode::IsActive() && actions && action == actions->activate) ||
         (attack && (Terrain::Ready() ? Terrain::GetTool() != Terrain::Tool::Idle
                                    : BuildingMode::IsActive()));
}
} // namespace
} // namespace SkyBuild::Hooks
