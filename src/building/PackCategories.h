#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
namespace SkyBuild::Packs {
inline constexpr std::array<std::string_view, 12> DefaultCategories{
    "Floors",    "Walls",      "Roofs",  "Stairs", "Beams", "Doors",
    "Furniture", "Containers", "Lights", "Decor",  "Trees", "Other"};
inline std::string SuggestCategory(std::string_view type,
                                   std::string_view editorID,
                                   std::string_view name) {
  std::string text(editorID);
  text += ' ';
  text += name;
  for (auto &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto has = [&](std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(
        words, [&](auto word) { return text.find(word) != std::string::npos; });
  };
  if (type == "Tree")
    return "Trees";
  if (type == "Door")
    return "Doors";
  if (type == "Container")
    return "Containers";
  if (type == "Furniture")
    return "Furniture";
  if (type == "Light" || has({"candle", "sconce", "lantern", "brazier", "torch",
                              "chandelier", "lamp", "firepit", "campfire"}))
    return "Lights";
  if (has({"doorway", "doorframe"}))
    return "Walls";
  if (has({"door", "gate"}))
    return "Doors";
  if (has({"stair", "steps", "ladder"}))
    return "Stairs";
  if (has({"roof", "ceiling"}))
    return "Roofs";
  if (has({"beam", "pillar", "column", "supportpost"}))
    return "Beams";
  if (has({"floor", "foundation", "platform"}))
    return "Floors";
  if (has({"wall", "window", "railing", "fence"}))
    return "Walls";
  if (has({"chest", "barrel", "crate", "sack", "basket", "cupboard", "wardrobe",
           "dresser"}))
    return "Containers";
  if (has({"statue", "painting", "tapestry", "banner", "candle", "sconce",
           "lantern", "brazier", "vase", "urn", "carpet", "rug", "decoration"}))
    return "Decor";
  if (has({"tree", "sapling", "shrub", "bush", "pine", "birch"}))
    return "Trees";
  if (has({"chair", "bench", "stool", "table", "bed", "throne", "workbench",
           "forge", "smelter", "rug", "shelf"}))
    return "Furniture";
  return "Other";
}
inline std::string CategoryKey(std::string_view name) {
  std::string key(name);
  for (auto &c : key)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return key;
}
inline bool IsDefaultCategory(std::string_view name) {
  const auto key = CategoryKey(name);
  return std::ranges::any_of(
      DefaultCategories, [&](auto value) { return CategoryKey(value) == key; });
}
inline void EnsureDefaultCategories(nlohmann::json &categories) {
  if (!categories.is_array())
    return;
  for (const auto name : DefaultCategories) {
    const auto key = CategoryKey(name);
    const auto found = std::find_if(
        categories.begin(), categories.end(), [&](const auto &entry) {
          return entry.is_string() &&
                 CategoryKey(entry.template get<std::string>()) == key;
        });
    if (found == categories.end()) {
      const auto other =
          std::find(categories.begin(), categories.end(), "Other");
      categories.insert(other, std::string(name));
    } else
      *found = std::string(name);
  }
  // Other is a fixed catch-all after every built-in and custom category.
  const auto other = std::find(categories.begin(), categories.end(), "Other");
  if (other != categories.end()) {
    categories.erase(other);
    categories.push_back("Other");
  }
}
} // namespace SkyBuild::Packs
