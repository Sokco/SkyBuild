#pragma once
#include "map-markers/MarkerData.h"
#include <memory>
#include <string>
#include <vector>
namespace SkyBuild::MapMarkers::Icons {
struct Icon {
  std::string key, label, source, symbol;
  std::uint16_t type{};
  bool custom{};
};
// Called on the game thread when the editor opens, after CoMAP has initialized.
void Refresh();
std::shared_ptr<const std::vector<Icon>> Catalog();
std::uint16_t Resolve(const Marker &marker);
void Listen();
// ImGui render thread: draw the actual installed vector art, clipped to its
// tile.
void Thumbnail(const Icon &icon, float x, float y, float size);
} // namespace SkyBuild::MapMarkers::Icons
