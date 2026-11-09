#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <SKSE/InputMap.h>
#include <atomic>

namespace SkyBuild::ControlBindings {
inline std::atomic_bool gamepadInUse{false};
struct InputBinding {
  std::uint32_t code{};
  const char *icon{""};
};
struct Binding {
  InputBinding keyboardMouse{}, gamepad{};
  const char *Icon() const {
    return (gamepadInUse.load() ? gamepad : keyboardMouse).icon;
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
inline bool Matches(const RE::ButtonEvent &button, const Binding &binding) {
  const auto &input = button.GetDevice() == RE::INPUT_DEVICE::kGamepad
                          ? binding.gamepad : binding.keyboardMouse;
  return input.code != 0 && InputCode(button) == input.code;
}
inline Binding MakeBinding(std::uint32_t code, const char *icon,
                           std::uint32_t gamepadCode, const char *gamepadIcon) {
  return {{code, icon}, {gamepadCode, gamepadIcon}};
}

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
