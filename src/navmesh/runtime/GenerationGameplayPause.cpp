#include "navmesh/runtime/GenerationGameplayPause.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "navmesh/diagnostics/DebugVisualization.h"
#include <atomic>
namespace SkyBuild::Navmesh::GenerationGameplayPause {
namespace {
constexpr std::string_view name = "SkyBuildNavigationGeneration";
std::atomic_bool owned{}, queued{};
bool registered{};
void Schedule() {
  if (!owned.load() || queued.exchange(true))
    return;
  if (const auto tasks = SKSE::GetTaskInterface()) {
    tasks->AddTask([] {
      queued.store(false);
      if (owned.load()) {
        try {
          SkyBuild::Navmesh::Update();
          DebugVisualization::Update();
        } catch (const std::exception &error) {
          SkyBuild::Navmesh::AbortGeneration(error.what());
        }
      }
    });
  } else
    queued.store(false);
}
class Menu final : public RE::IMenu {
public:
  static RE::IMenu *Create() {
    auto *menu = new Menu;
    menu->menuFlags.set(
        RE::UI_MENU_FLAGS::kPausesGame, RE::UI_MENU_FLAGS::kDisablePauseMenu,
        RE::UI_MENU_FLAGS::kCustomRendering, RE::UI_MENU_FLAGS::kAlwaysOpen,
        RE::UI_MENU_FLAGS::kTopmostRenderedMenu);
    return menu;
  }
  void AdvanceMovie(float, std::uint32_t) override { Schedule(); }
  void PostDisplay() override { Schedule(); }
  RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage &message) override {
    // Only the lifecycle owner may end this pause. Native menu teardown still
    // owns the pause counter; no direct global state/counter writes occur.
    if (message.type == RE::UI_MESSAGE_TYPE::kHide && owned.load())
      return RE::UI_MESSAGE_RESULTS::kIgnore;
    if (message.type == RE::UI_MESSAGE_TYPE::kShow && !owned.load())
      return RE::UI_MESSAGE_RESULTS::kIgnore;
    return RE::UI_MESSAGE_RESULTS::kHandled;
  }
};
} // namespace
void Initialize() {
  if (!registered)
    if (const auto ui = RE::UI::GetSingleton()) {
      ui->Register(name, Menu::Create);
      registered = true;
    }
}
bool Begin() {
  const auto queue = RE::UIMessageQueue::GetSingleton();
  if (!registered || !queue || !SKSE::GetTaskInterface() ||
      owned.exchange(true))
    return false;
  queue->AddMessage(name.data(), RE::UI_MESSAGE_TYPE::kShow, nullptr);
  spdlog::info("Acquiring gameplay pause for explicit navigation generation.");
  return true;
}
void End() {
  if (!owned.exchange(false))
    return;
  if (const auto queue = RE::UIMessageQueue::GetSingleton())
    queue->AddMessage(name.data(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
  spdlog::info("Released explicit generation gameplay-pause ownership.");
}
bool Owned() noexcept { return owned.load(); }
bool Ready() {
  const auto ui = RE::UI::GetSingleton();
  return Owned() && ui && ui->IsMenuOpen(name) && ui->GameIsPaused();
}
bool OnlyGameplayPause() {
  const auto ui = RE::UI::GetSingleton();
  return Ready() && ui->numPausesGame == 1 && !ui->IsMenuOpen("Loading Menu");
}
} // namespace SkyBuild::Navmesh::GenerationGameplayPause
