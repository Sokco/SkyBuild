#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <SKSE/InputMap.h>
#include <atomic>
#include <string>
#include "building/InputChord.h"

namespace SkyBuild::ControlBindings {
inline std::atomic_bool gamepadInUse{false};
struct InputBinding {
  InputChord chord;
  std::string icon;
};
struct Binding {
  InputBinding keyboardMouse{}, gamepad{};
  const char *Icon() const {
    return (gamepadInUse.load() ? gamepad : keyboardMouse).icon.c_str();
  }
};

inline std::uint32_t InputCode(const RE::ButtonEvent &button) {
  switch (button.GetDevice()) {
  case RE::INPUT_DEVICE::kKeyboard:
    return button.GetIDCode();
  case RE::INPUT_DEVICE::kMouse:
    return button.GetIDCode() + 256U;
  case RE::INPUT_DEVICE::kGamepad:
    return SKSE::InputMap::GamepadMaskToKeycode(button.GetIDCode());
  default:
    return SKSE::InputMap::kMaxMacros;
  }
}
inline void Observe(const RE::ButtonEvent &button) {
  if (button.IsDown())
    gamepadInUse.store(button.GetDevice() == RE::INPUT_DEVICE::kGamepad);
}
inline void ObserveEvent(const RE::InputEvent &event) {
  if (const auto *button = event.AsButtonEvent())
    Observe(*button);
  else if (const auto *stick = event.AsThumbstickEvent()) {
    if (std::hypot(stick->xValue, stick->yValue) > .25F)
      gamepadInUse.store(true);
  } else if (const auto *mouse = event.AsMouseMoveEvent()) {
    if (mouse->mouseInputX || mouse->mouseInputY)
      gamepadInUse.store(false);
  }
}
struct Event {
  const Binding *binding{};
  std::uint32_t code{};
  Input::Phase phase{};
  float duration{};
  bool IsDown() const { return phase == Input::Phase::Down; }
  bool IsUp() const { return phase == Input::Phase::Up; }
  bool IsPressed() const { return !IsUp(); }
  float HeldDuration() const { return duration; }
  RE::INPUT_DEVICE GetDevice() const {
    return code >= 266 ? RE::INPUT_DEVICE::kGamepad :
           code >= 256 ? RE::INPUT_DEVICE::kMouse : RE::INPUT_DEVICE::kKeyboard;
  }
  std::uint32_t GetIDCode() const {
    if (code >= 266) return SKSE::InputMap::GamepadKeycodeToMask(code);
    return code >= 256 ? code - 256 : code;
  }
};
inline bool Matches(const Event &event, const Binding &binding) {
  return event.binding == &binding;
}
inline std::atomic_uint64_t revision{0};

// Settings::Apply initializes both device bindings before input hooks are installed.
inline std::array<Binding, 2> modifyControls{};
inline std::array<Binding, 14> fineControls{};
inline Binding fineConfirm{}, fineCancel{}, openMenu{};
inline Binding brushIncrease{}, brushDecrease{}, historyUndo{}, historyRedo{};
inline Binding toggleSnapping{}, fineAdjustments{}, rotate{}, rotateCounterclockwise{};
inline Binding secondaryAction{}, placeAction{}, switchMode{};
inline Binding packPrevious{}, packNext{}, categoryLeft{}, categoryRight{};
inline Binding scrollLeft{}, scrollRight{};
} // namespace SkyBuild::ControlBindings
