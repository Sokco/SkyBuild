#include "rendering/WorldFrame.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/InputOwnership.h"
#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "terrain/TerrainTool.h"
#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include "ui/fine-adjustments/FineAdjustments.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/settings/Settings.h"
#include "ui/settings/SettingsWorkspace.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <d3d11.h>
#include <filesystem>
#include <im_anim.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

// Provided by the pinned Win32 backend; key translation itself does not touch a
// context.
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM, LPARAM);

namespace SkyBuild::UI::Native {
namespace {
using Microsoft::WRL::ComPtr;
struct Window {
  Model::WindowInterface state;
  Callback draw;
};
using Section = SettingsWorkspace::Page;
std::mutex registryMutex, inputMutex;
std::vector<std::unique_ptr<Window>> windows;
std::vector<Callback> huds;
std::vector<Model::EventCallback> subscribers;
std::vector<Section> sections;
Model::WindowInterface mainWindow;
ImGuiContext *context{};
std::atomic_bool ready{}, clearInput{};
std::atomic_bool shuttingDown{};
// HUD owners live in other translation units. Their static destructors may run
// after this registry; do not unregister from a registry already being
// destroyed.
struct RegistryLifetime {
  ~RegistryLifetime() { shuttingDown.store(true); }
} registryLifetime;
HWND hwnd{};
WNDPROC originalWndProc{};
REL::Relocation<std::int64_t(std::int64_t)> originalWorldUI, originalMenuUI;
enum class InputKind { Key, Mouse, Wheel, Character, Motion, Gamepad, Stick };
struct Input {
  InputKind kind;
  unsigned code;
  bool down;
  float x{}, y{};
};
std::vector<Input> inputQueue;
std::array<bool, 8> modifiers{};
InputOwnership inputOwnership; // Protected by inputMutex.
ImVec2 cursor{}, gamepadCursor{};
constexpr auto menuName = "SkyBuildWorkspace";

class WorkspaceMenu : public RE::IMenu {
public:
  static RE::IMenu *Create() {
    auto *menu = new WorkspaceMenu;
    // A real Skyrim menu owns the pause counter. Never overwrite global freeze
    // state.
    menu->menuFlags.set(RE::UI_MENU_FLAGS::kPausesGame,
                        RE::UI_MENU_FLAGS::kDisablePauseMenu,
                        RE::UI_MENU_FLAGS::kAllowSaving);
    return menu;
  }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_KILLFOCUS)
    clearInput.store(true);
  // Skyrim supplies keyboard, text, buttons and relative motion through its
  // input queue. Feeding WM_* too would duplicate clicks and characters.
  return CallWindowProcA(originalWndProc, window, message, w, l);
}

bool InitializeRenderer() {
  const auto *renderer = RE::BSGraphics::Renderer::GetSingleton();
  if (!renderer)
    return false;
  const auto &data = renderer->GetRuntimeData();
  auto *device = reinterpret_cast<ID3D11Device *>(data.forwarder);
  auto *immediate = reinterpret_cast<ID3D11DeviceContext *>(data.context);
  auto *swap =
      reinterpret_cast<IDXGISwapChain *>(data.renderWindows[0].swapChain);
  DXGI_SWAP_CHAIN_DESC desc{};
  if (!device || !immediate || !swap || FAILED(swap->GetDesc(&desc)))
    return false;
  auto *previous = ImGui::GetCurrentContext();
  context = ImGui::CreateContext();
  auto &io = ImGui::GetIO();
  io.IniFilename = "Data/SkyBuild/ImGui.ini";
  io.LogFilename = nullptr;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  io.ConfigInputTrickleEventQueue = true;
  ImFontConfig fontConfig;
  fontConfig.SizePixels = 18.F;
  wchar_t systemDirectory[MAX_PATH]{};
  GetWindowsDirectoryW(systemDirectory, MAX_PATH);
  const auto fontPath =
      std::filesystem::path(systemDirectory) / "Fonts/segoeui.ttf";
  if (!std::filesystem::exists(fontPath) ||
      !io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 18.F,
                                    &fontConfig))
    io.Fonts->AddFontDefault(&fontConfig);
  // Connection direction labels use two symbols outside the default Latin
  // range.
  const auto symbolFont =
      std::filesystem::path(systemDirectory) / "Fonts/seguisym.ttf";
  if (std::filesystem::exists(symbolFont)) {
    static constexpr ImWchar ranges[]{0x2934, 0x2935, 0};
    ImFontConfig symbols;
    symbols.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(symbolFont.string().c_str(), 18.F, &symbols,
                                 ranges);
  }
  Widgets::ApplyBaseStyle();
  bool win32 = ImGui_ImplWin32_Init(desc.OutputWindow);
  bool dx11 = win32 && ImGui_ImplDX11_Init(device, immediate);
  if (!dx11) {
    if (win32)
      ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    context = nullptr;
    ImGui::SetCurrentContext(previous);
    spdlog::error(
        "SkyBuild could not initialize its independent ImGui renderer.");
    return false;
  }
  hwnd = desc.OutputWindow;
  originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc)));
  RECT client{};
  GetClientRect(hwnd, &client);
  cursor = {(client.right - client.left) * .5F,
            (client.bottom - client.top) * .5F};
  if (!originalWndProc) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    context = nullptr;
    ImGui::SetCurrentContext(previous);
    spdlog::error("SkyBuild could not chain the game window procedure.");
    return false;
  }
  Terrain::BrushOverlay::Trace::Initialize(immediate);
  ready.store(true);
  ImGui::SetCurrentContext(previous);
  spdlog::info(
      "Standalone Dear ImGui {} initialized. SkyBuild settings binding is configurable.",
      IMGUI_VERSION);
  return true;
}

void DrainInput(bool interactive) {
  std::vector<Input> pending;
  {
    std::scoped_lock lock(inputMutex);
    pending.swap(inputQueue);
  }
  auto &io = ImGui::GetIO();
  if (clearInput.exchange(false)) {
    io.ClearInputKeys();
    pending.clear();
    modifiers.fill(false);
    gamepadCursor = {};
    SKSE::GetTaskInterface()->AddTask([] {
      UI::CancelToolWheel();
      Terrain::SuspendInput();
    });
  }
  if (interactive)
    io.AddMousePosEvent(cursor.x, cursor.y);
  for (const auto &event : pending) {
    switch (event.kind) {
    case InputKind::Key: {
      const auto scan = event.code;
      const UINT extendedScan = scan & 0x80 ? 0xE000 | (scan & 0x7F) : scan;
      const auto vk = MapVirtualKeyW(extendedScan, MAPVK_VSC_TO_VK_EX);
      const auto key = ImGui_ImplWin32_KeyEventToImGuiKey(
          vk, static_cast<LPARAM>((scan & 0x7F) << 16) |
                  (scan & 0x80 ? 1L << 24 : 0));
      if (key != ImGuiKey_None)
        io.AddKeyEvent(key, event.down);
      constexpr std::array<UINT, 8> modifierKeys{
          VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT,
          VK_LMENU,    VK_RMENU,    VK_LWIN,   VK_RWIN};
      constexpr std::array<ImGuiKey, 4> modifierFlags{
          ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super};
      for (std::size_t i = 0; i < modifierKeys.size(); ++i)
        if (vk == modifierKeys[i]) {
          modifiers[i] = event.down;
          io.AddKeyEvent(modifierFlags[i / 2],
                         modifiers[i / 2 * 2] || modifiers[i / 2 * 2 + 1]);
        }
      break;
    }
    case InputKind::Gamepad: {
      static constexpr std::array<ImGuiKey, 16> keys{
          ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown,
          ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
          ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack,
          ImGuiKey_GamepadL3,       ImGuiKey_GamepadR3,
          ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,
          ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
          ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
          ImGuiKey_GamepadL2,       ImGuiKey_GamepadR2};
      if (event.code >= 266 && event.code < 282) {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.AddKeyEvent(keys[event.code - 266], event.down);
        if (event.code == 280 || event.code == 281)
          io.AddMouseButtonEvent(event.code == 281 ? 0 : 1, event.down);
      }
      break;
    }
    case InputKind::Stick:
      if (event.code == 1) {
        gamepadCursor = {event.x, -event.y};
      } else {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        const auto axis = [&](ImGuiKey key, float value) {
          const float amount = std::clamp((value - .2F) / .8F, 0.F, 1.F);
          io.AddKeyAnalogEvent(key, amount > 0.F, amount);
        };
        axis(ImGuiKey_GamepadLStickLeft, -event.x);
        axis(ImGuiKey_GamepadLStickRight, event.x);
        axis(ImGuiKey_GamepadLStickUp, event.y);
        axis(ImGuiKey_GamepadLStickDown, -event.y);
      }
      break;
    case InputKind::Mouse:
      io.AddMouseButtonEvent(event.code, event.down);
      break;
    case InputKind::Wheel:
      io.AddMouseWheelEvent(0, event.y);
      break;
    case InputKind::Character:
      io.AddInputCharacter(event.code);
      break;
    case InputKind::Motion:
      cursor.x = std::clamp(cursor.x + event.x, 0.F, io.DisplaySize.x);
      cursor.y = std::clamp(cursor.y + event.y, 0.F, io.DisplaySize.y);
      if (interactive)
        io.AddMousePosEvent(cursor.x, cursor.y);
      break;
    }
  }
  if (interactive) {
    const auto speed = [](float value) {
      const float magnitude = std::abs(value);
      return magnitude > .2F ? std::copysign((magnitude - .2F) / .8F, value)
                             : 0.F;
    };
    cursor.x =
        std::clamp(cursor.x + speed(gamepadCursor.x) * 900.F * io.DeltaTime,
                   0.F, io.DisplaySize.x);
    cursor.y =
        std::clamp(cursor.y + speed(gamepadCursor.y) * 900.F * io.DeltaTime,
                   0.F, io.DisplaySize.y);
    io.AddMousePosEvent(cursor.x, cursor.y);
  } else {
    gamepadCursor = {};
  }
  io.MouseDrawCursor = interactive && GetForegroundWindow() == hwnd;
}

void DrawSettings(const std::vector<Section> &pages) {
  if (!mainWindow.IsOpen.load())
    return;
  bool open = true;
  static std::size_t selected{};
  SettingsWorkspace::Draw(pages, open, selected);
  if (!open)
    mainWindow.IsOpen.store(false);
}

void Render() {
  const bool firstFrame = !ready.load();
  const auto startup = firstFrame ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
  if (firstFrame && !InitializeRenderer())
    return;
  auto *previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context);
  struct Restore {
    ImGuiContext *previous;
    ~Restore() { ImGui::SetCurrentContext(previous); }
  } restore{previous};
  std::vector<Callback> draws;
  std::vector<Window *> windowCopy;
  std::vector<Section> pages;
  std::vector<Model::EventCallback> events;
  {
    std::scoped_lock lock(registryMutex);
    draws = huds;
    pages = sections;
    events = subscribers;
    for (const auto &window : windows)
      windowCopy.push_back(window.get());
  }
  const bool interactive = IsAnyBlockingWindowOpened();
  static bool wasInteractive{};
  if (interactive != wasInteractive) {
    wasInteractive = interactive;
    clearInput.store(true);
    for (const auto event : events)
      event(interactive ? Model::kOpenMenu : Model::kCloseMenu);
    SKSE::GetTaskInterface()->AddTask([interactive] {
      if (interactive) {
        UI::CancelToolWheel();
        UI::FineAdjustments::Close();
        Terrain::SuspendInput();
      }
      if (const auto queue = RE::UIMessageQueue::GetSingleton())
        queue->AddMessage(menuName,
                          interactive ? RE::UI_MESSAGE_TYPE::kShow
                                      : RE::UI_MESSAGE_TYPE::kHide,
                          nullptr);
      UI::RefreshPresentation();
    });
  }
  auto &data = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData();
  auto *immediate = reinterpret_cast<ID3D11DeviceContext *>(data.context);
  ComPtr<ID3D11RenderTargetView> output;
  ComPtr<ID3D11DepthStencilView> depth;
  immediate->OMGetRenderTargets(1, &output, &depth);
  auto *target = output.Get();
  if (!target)
    return;
  ComPtr<ID3D11Resource> resource;
  ComPtr<ID3D11Texture2D> texture;
  target->GetResource(&resource);
  if (!resource || FAILED(resource.As(&texture)))
    return;
  D3D11_TEXTURE2D_DESC targetDesc{};
  texture->GetDesc(&targetDesc);
  D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
  target->GetDesc(&viewDesc);
  const UINT mip = viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D
                       ? viewDesc.Texture2D.MipSlice
                       : 0;
  const auto width = std::max(1U, targetDesc.Width >> mip);
  const auto height = std::max(1U, targetDesc.Height >> mip);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  auto &io = ImGui::GetIO();
  if (io.DisplaySize.x <= 0.F || io.DisplaySize.y <= 0.F)
    return;
  // Layout and input use client coordinates; rasterization uses target pixels.
  io.DisplayFramebufferScale = {width / io.DisplaySize.x,
                                height / io.DisplaySize.y};
  cursor.x = std::clamp(cursor.x, 0.F, io.DisplaySize.x);
  cursor.y = std::clamp(cursor.y, 0.F, io.DisplaySize.y);
  DrainInput(interactive || UI::WheelHasCursor());
  Widgets::PrepareGamepadNavigation();
  ImGui::NewFrame();
  Widgets::BeginFrame();
  iam_update_begin_frame();
  if (ImGui::GetFrameCount() % 60 == 0)
    iam_gc(600);
  for (const auto draw : draws)
    draw();
  DrawSettings(pages);
  for (const auto window : windowCopy)
    if (window->state.IsOpen.load())
      window->draw();
  ImGui::Render();
  // Keep the native interface's output, including HDR's redirected UI buffer.
  immediate->OMSetRenderTargets(1, &target, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  immediate->OMSetRenderTargets(1, &target, depth.Get());
  if (firstFrame) {
    spdlog::info("Native UI initialization and first draw: {:.2f} ms CPU.",
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - startup)
                     .count());
  }
}
std::int64_t CompleteWorldUI(std::int64_t manager) {
  const auto result = originalWorldUI(manager);
  Render();
  return result;
}
std::int64_t CompleteMenuUI(std::int64_t manager) {
  const auto result = originalMenuUI(manager);
  Render();
  return result;
}
} // namespace

bool Install() {
  if (!SkyBuild::Rendering::WorldFrame::Install())
    spdlog::error("World-space previews have no world completion hook.");
  const auto version = REL::Module::get().version();
  const bool se = REL::Module::IsSE();
  if (REL::Module::IsVR() ||
      (version != REL::Version(1, 5, 97, 0) &&
       version != REL::Version(1, 6, 640, 0) &&
       version != REL::Version(1, 6, 1170, 0) &&
       version != REL::Version(1, 7, 104, 0)))
    return false;
  const auto world = REL::RelocationID(35556, 36555).address() +
                     (se ? 0x3AB : 0x371);
  const auto menu = REL::RelocationID(38085, 39039).address() +
                    (version == REL::Version(1, 7, 104, 0) ? 0x1A0 : 0x19A);
  if (*reinterpret_cast<const std::uint8_t *>(world) != 0xE8 ||
      *reinterpret_cast<const std::uint8_t *>(menu) != 0xE8) {
    spdlog::critical("SkyBuild rejected unexpected native UI completion calls.");
    return false;
  }
  auto &trampoline = SKSE::GetTrampoline();
  originalWorldUI = trampoline.write_call<5>(world, CompleteWorldUI);
  originalMenuUI = trampoline.write_call<5>(menu, CompleteMenuUI);
  return true;
}
void RegisterMenu() {
  if (auto *ui = RE::UI::GetSingleton())
    ui->Register(menuName, WorkspaceMenu::Create);
}
bool CaptureInputEvent(RE::InputEvent *event) {
  if (!ready.load() || !event)
    return false;
  const bool blockingWindow = IsAnyBlockingWindowOpened();
  bool consume = blockingWindow || UI::WheelHasCursor();
  std::scoped_lock lock(inputMutex);
  {
    if (auto *button = event->AsButtonEvent()) {
      const bool mouse = button->GetDevice() == RE::INPUT_DEVICE::kMouse;
      const bool keyboard = button->GetDevice() == RE::INPUT_DEVICE::kKeyboard;
      const bool gamepad = button->GetDevice() == RE::INPUT_DEVICE::kGamepad;
      if (!mouse && !keyboard && !gamepad)
        return false;
      if (gamepad) {
        const auto *controls = RE::ControlMap::GetSingleton();
        Widgets::SetGamepadIcons(controls && controls->GetGamePadType() ==
                                                 RE::PC_GAMEPAD_TYPE::kOrbis);
      }
      const auto code = button->GetIDCode();
      // Wheel directions are pulses, not buttons with a matching release.
      // Never retain ownership of them: doing so captures gameplay scrolling
      // after the workspace closes.
      if (mouse && (code == 8 || code == 9)) {
        if (consume && button->IsPressed())
          inputQueue.push_back(
              {InputKind::Wheel, 0, false, 0, code == 8 ? 1.F : -1.F});
        return consume;
      }
      const auto slot = ControlBindings::InputCode(*button);
      consume = inputOwnership.Consume(slot, button->IsDown(), button->IsUp(),
                                       consume);
      if (!consume)
        return false;
      if (!button->IsDown() && !button->IsUp())
        return consume;
      if (mouse && code < 5)
        inputQueue.push_back({InputKind::Mouse, code, button->IsDown()});
      else if (keyboard)
        inputQueue.push_back({InputKind::Key, code, button->IsDown()});
      else if (gamepad && (blockingWindow || button->IsUp()))
        inputQueue.push_back({InputKind::Gamepad, slot, button->IsDown()});
    } else if (const auto *stick = event->AsThumbstickEvent()) {
      if (UI::IsToolWheelOpen() && !blockingWindow) {
        UI::PointToolWheel(stick->xValue, -stick->yValue);
        return true;
      }
      if (blockingWindow) {
        inputQueue.push_back({InputKind::Stick, stick->IsRight() ? 1U : 0U,
                              false, stick->xValue, stick->yValue});
        return true;
      }
    } else if (consume) {
      if (const auto *character = event->AsCharEvent())
        inputQueue.push_back({InputKind::Character, character->keyCode, false});
      else if (const auto *motion = event->AsMouseMoveEvent())
        inputQueue.push_back({InputKind::Motion, 0, false,
                              static_cast<float>(motion->mouseInputX),
                              static_cast<float>(motion->mouseInputY)});
    }
  }
  if (inputQueue.size() > 8192) {
    inputQueue.clear();
    clearInput.store(true);
  }
  return consume;
}
void ResetInput() {
  std::scoped_lock lock(inputMutex);
  inputOwnership.Reset();
  inputQueue.clear();
  clearInput.store(true);
}
Model::WindowInterface *AddWindow(Callback callback, bool block) {
  auto window = std::make_unique<Window>();
  window->draw = callback;
  window->state.BlockUserInput.store(block);
  auto *state = &window->state;
  std::scoped_lock lock(registryMutex);
  windows.push_back(std::move(window));
  return state;
}
Model::WindowInterface *GetMainWindow() { return &mainWindow; }
bool IsAnyBlockingWindowOpened() {
  if (mainWindow.IsOpen.load())
    return true;
  std::scoped_lock lock(registryMutex);
  return std::any_of(windows.begin(), windows.end(), [](const auto &window) {
    return window->state.IsOpen.load() && window->state.BlockUserInput.load();
  });
}
void SetSection(const char *) {}
void AddSectionItem(const char *name, Callback callback) {
  std::scoped_lock lock(registryMutex);
  sections.push_back({name, callback});
}
Model::Event::Event(EventCallback cb, float) : callback(cb) {
  std::scoped_lock lock(registryMutex);
  subscribers.push_back(cb);
}
Model::Event::~Event() {
  if (shuttingDown.load())
    return;
  std::scoped_lock lock(registryMutex);
  std::erase(subscribers, callback);
}
Model::HudElement::HudElement(Callback cb) : callback(cb) {
  std::scoped_lock lock(registryMutex);
  huds.push_back(cb);
}
Model::HudElement::~HudElement() {
  if (shuttingDown.load())
    return;
  std::scoped_lock lock(registryMutex);
  std::erase(huds, callback);
}
} // namespace SkyBuild::UI::Native
