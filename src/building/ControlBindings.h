#pragma once

namespace SkyBuild::ControlBindings {
struct KeyboardBinding {
  std::uint32_t code;
  const char *iconKey;
  RE::INPUT_DEVICE device{RE::INPUT_DEVICE::kKeyboard};
};

inline std::array<KeyboardBinding, 2> modifyControls{
    {{RE::BSKeyboardDevice::Key::kC, "KeyC"},
     {RE::BSKeyboardDevice::Key::kE, "KeyE"}}};
inline KeyboardBinding openMenu{RE::BSKeyboardDevice::Key::kM, "KeyM"};
inline KeyboardBinding brushIncrease{8, "MouseWheelUp",
                                     RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding brushDecrease{9, "MouseWheelDown",
                                     RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding historyUndo{RE::BSKeyboardDevice::Key::kZ, "KeyZ"};
inline KeyboardBinding historyRedo{RE::BSKeyboardDevice::Key::kX, "KeyX"};
// Fine adjustments have their own context, so bindings may overlap build
// controls.
inline std::array<KeyboardBinding, 14> fineControls{{{203, "ArrowLeft"},
                                                     {205, "ArrowRight"},
                                                     {200, "ArrowUp"},
                                                     {208, "ArrowDown"},
                                                     {201, "PageUp"},
                                                     {209, "PageDown"},
                                                     {36, "KeyJ"},
                                                     {38, "KeyL"},
                                                     {23, "KeyI"},
                                                     {37, "KeyK"},
                                                     {22, "KeyU"},
                                                     {24, "KeyO"},
                                                     {12, "Minus"},
                                                     {13, "Equal"}}};
// Input handling and control hints share these bindings.
inline KeyboardBinding toggleSnapping{RE::BSKeyboardDevice::Key::kT, "KeyT"};
inline KeyboardBinding fineAdjustments{RE::BSKeyboardDevice::Key::kF, "KeyF"};
inline KeyboardBinding rotate{RE::BSKeyboardDevice::Key::kR, "KeyR"};
inline KeyboardBinding rotateCounterclockwise{0, ""};
inline KeyboardBinding secondaryAction{1, "MouseRight",
                                       RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding placeAction{0, "MouseLeft", RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding switchMode{2, "MouseMiddle", RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding packPrevious{RE::BSKeyboardDevice::Key::kC, "KeyC"};
inline KeyboardBinding packNext{RE::BSKeyboardDevice::Key::kV, "KeyV"};
inline KeyboardBinding categoryLeft{RE::BSKeyboardDevice::Key::kQ, "KeyQ"};
inline KeyboardBinding categoryRight{RE::BSKeyboardDevice::Key::kE, "KeyE"};
inline KeyboardBinding scrollLeft{9, "MouseWheelDown",
                                  RE::INPUT_DEVICE::kMouse};
inline KeyboardBinding scrollRight{8, "MouseWheelUp", RE::INPUT_DEVICE::kMouse};
} // namespace SkyBuild::ControlBindings
