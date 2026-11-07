#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace SkyBuild::MapMarkers {
inline constexpr std::uint32_t SaveRecord = 0x53424D4D;
inline constexpr std::size_t MaxMarkers = 4096;
inline constexpr std::size_t MaxRecordBytes = 4 * 1024 * 1024;
inline constexpr std::array<const char *, 59> VanillaNames{"City",
                                                           "Town",
                                                           "Settlement",
                                                           "Cave",
                                                           "Camp",
                                                           "Fort",
                                                           "Nordic ruin",
                                                           "Dwemer ruin",
                                                           "Shipwreck",
                                                           "Grove",
                                                           "Landmark",
                                                           "Dragon lair",
                                                           "Farm",
                                                           "Wood mill",
                                                           "Mine",
                                                           "Imperial camp",
                                                           "Stormcloak camp",
                                                           "Standing stone",
                                                           "Wheat mill",
                                                           "Smelter",
                                                           "Stable",
                                                           "Imperial tower",
                                                           "Clearing",
                                                           "Pass",
                                                           "Altar",
                                                           "Rock",
                                                           "Lighthouse",
                                                           "Orc stronghold",
                                                           "Giant camp",
                                                           "Shack",
                                                           "Nordic tower",
                                                           "Nordic dwelling",
                                                           "Docks",
                                                           "Shrine",
                                                           "Riften castle",
                                                           "Riften capital",
                                                           "Windhelm castle",
                                                           "Windhelm capital",
                                                           "Whiterun castle",
                                                           "Whiterun capital",
                                                           "Solitude castle",
                                                           "Solitude capital",
                                                           "Markarth castle",
                                                           "Markarth capital",
                                                           "Winterhold castle",
                                                           "Winterhold capital",
                                                           "Morthal castle",
                                                           "Morthal capital",
                                                           "Falkreath castle",
                                                           "Falkreath capital",
                                                           "Dawnstar castle",
                                                           "Dawnstar capital",
                                                           "Miraak's temple",
                                                           "Raven Rock",
                                                           "All-Maker stone",
                                                           "Telvanni tower",
                                                           "To Skyrim",
                                                           "To Solstheim",
                                                           "Castle Karstaag"};
struct Marker {
  std::uint32_t reference{}, arrival{}, world{};
  std::string name;
  std::uint16_t vanillaIcon{11};
  // CoMAP names are stable; its numeric indices depend on the installed icons.
  std::string customIcon;
};
inline std::string CleanName(std::string name) {
  const auto first = name.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
  if (name.size() > 256 ||
      std::ranges::any_of(name, [](unsigned char c) { return c < 32; }))
    return {};
  return name;
}
inline nlohmann::json Encode(const std::vector<Marker> &markers) {
  auto data = nlohmann::json::array();
  for (const auto &m : markers)
    data.push_back({{"reference", m.reference},
                    {"arrival", m.arrival},
                    {"world", m.world},
                    {"name", m.name},
                    {"icon", m.vanillaIcon},
                    {"customIcon", m.customIcon}});
  return data;
}
inline std::vector<Marker> Decode(const nlohmann::json &data) {
  if (!data.is_array() || data.size() > MaxMarkers)
    throw std::runtime_error("Invalid map marker list.");
  std::vector<Marker> result;
  std::set<std::uint32_t> references;
  for (const auto &item : data) {
    Marker m;
    const auto id = [&](const char *field) {
      const auto &v = item.at(field);
      if (!v.is_number_integer() || v.get<std::int64_t>() <= 0 ||
          v.get<std::uint64_t>() > UINT32_MAX)
        throw std::runtime_error("Invalid map marker reference.");
      return v.get<std::uint32_t>();
    };
    m.reference = id("reference");
    m.arrival = id("arrival");
    m.world = id("world");
    m.name = CleanName(item.at("name").get<std::string>());
    if (!item.at("icon").is_number_integer())
      throw std::runtime_error("Invalid map marker icon.");
    const auto icon = item.at("icon").get<std::int64_t>();
    m.customIcon = item.value("customIcon", "");
    if (m.name.empty() || icon < 1 ||
        icon > static_cast<std::int64_t>(VanillaNames.size()) ||
        m.customIcon.size() > 256 || !references.insert(m.reference).second ||
        !references.insert(m.arrival).second)
      throw std::runtime_error("Invalid map marker settings.");
    m.vanillaIcon = static_cast<std::uint16_t>(icon);
    result.push_back(std::move(m));
  }
  return result;
}
} // namespace SkyBuild::MapMarkers
