#include "ui/search/SearchMenu.h"
#include "building/BuildingMode.h"
#include "building/PlacementPreview.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/search/SearchView.h"
#include "ui/settings/Settings.h"
#include <atomic>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
namespace SkyBuild::UI::SearchMenu {
namespace {
using Json = nlohmann::json;
std::atomic_bool open{};
SkyBuild::UI::Native::Model::WindowInterface *window{};
std::atomic<std::shared_ptr<const Json>> catalog, appearance;
void __stdcall Draw() {
  SearchView::Draw(
      catalog.load(), appearance.load(),
      [](const Json &item) {
        SKSE::GetTaskInterface()->AddTask([item] {
          if (!IsOpen() || !window || !window->IsOpen.load() ||
              !BuildingMode::IsHammerReady())
            return;
          const auto id = static_cast<RE::FormID>(
              std::stoul(item.at("formId").get<std::string>(), nullptr, 16));
          if (PlacementPreview::SelectInstalledPlaceable(
                  id, item.at("pack").get<std::string>(),
                  item.at("category").get<std::string>())) {
            Close();
            BuildingMode::SelectTool(BuildingMode::InteractionMode::kBuild);
          }
        });
      },
      [] { SKSE::GetTaskInterface()->AddTask([] { Close(); }); });
}
} // namespace
void Open() {
  if (!window)
    window = SkyBuild::UI::Native::AddWindow(Draw);
  if (!window)
    return;
  LoadPlaceableCarousel();
  catalog.store(std::make_shared<const Json>(GetPlaceableCatalog()));
  appearance.store(std::make_shared<const Json>(GetPlaceableAppearance()));
  open.store(true);
  window->IsOpen.store(true);
  Settings::SetMenuOpen(false);
  RefreshPresentation();
}
void Close() {
  open.store(false);
  if (window)
    window->IsOpen.store(false);
  RefreshPresentation();
}
void Update() {
  if (open.load() && window && !window->IsOpen.load())
    Close();
}
bool IsOpen() { return open.load(); }
} // namespace SkyBuild::UI::SearchMenu
