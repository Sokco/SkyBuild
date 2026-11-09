#pragma once
#include "building/AutomaticBuildCosts.h"
#include "building/BuildCostData.h"
#include "building/PackIdentityJson.h"
#include "building/PackCategories.h"
#include "building/PackSchema.h"
#include "ui/thumbnails/ThumbnailFormID.h"
#include "ui/thumbnails/ThumbnailPaths.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
namespace SkyBuild::UI::PackDocument {
inline std::string Lower(std::string value) {
  for (auto &c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}
// Suggestions are applied only when picking a new record, never to saved items.
inline std::string SuggestPlacementPreset(bool isStatic, std::string editorID) {
  if (!isStatic)
    return "prop";
  editorID = Lower(std::move(editorID));
  const auto contains = [&](std::string_view word) {
    return editorID.find(word) != std::string::npos;
  };
  // Resolve overlapping hints in placement-type priority order.
  if (contains("wall") || contains("fence") || contains("railing"))
    return "wall";
  if (contains("foundation"))
    return "foundation";
  if (contains("floor") || contains("ceiling") || contains("platform"))
    return "floor";
  if (contains("pillar") || contains("beam") || contains("column"))
    return "pillar";
  if (contains("cell") || contains("stair"))
    return "cell";
  return "prop";
}
inline nlohmann::json SnappingPreset(std::string_view preset) {
  const auto occupy = preset == "foundation"
                          ? nlohmann::json::array({"center", "top"})
                      : preset == "floor" ? nlohmann::json::array({"bottom"})
                      : preset == "cell"  ? nlohmann::json::array({"center"})
                                          : nlohmann::json::array();
  return {{"type", preset == "wall"     ? "side"
                   : preset == "pillar" ? "corner"
                   : preset == "prop"   ? "none"
                                        : "basic"},
          {"occupy", occupy}};
}
inline std::string MatchingSnappingPreset(const nlohmann::json &item) {
  for (const auto *preset :
       {"prop", "floor", "foundation", "cell", "wall", "pillar"}) {
    const auto expected = SnappingPreset(preset);
    auto actualOccupancy = item.value("occupy", nlohmann::json::array());
    auto expectedOccupancy = expected.at("occupy");
    std::sort(actualOccupancy.begin(), actualOccupancy.end());
    std::sort(expectedOccupancy.begin(), expectedOccupancy.end());
    if (expected.at("type") == item.value("type", "basic") &&
        actualOccupancy == expectedOccupancy)
      return preset;
  }
  return "Custom";
}
inline bool MatchesRecordSearch(std::string_view query,
                                std::string_view editorSearch,
                                std::string_view name) {
  const auto key = Lower(std::string(query));
  return key.empty() || editorSearch.find(key) != std::string_view::npos ||
         Lower(std::string(name)).find(key) != std::string::npos;
}
inline bool MatchesFilter(const nlohmann::json &item, const std::string &query,
                          const std::optional<std::string> &category) {
  if (category && item.value("category", "") != *category)
    return false;
  return query.empty() ||
         Lower(item.value("name", "") + " " +
               item["plugin"].get<std::string>() + " " + item["formId"].dump())
                 .find(Lower(query)) != std::string::npos;
}
inline bool ValidName(const std::string &name) {
  if (name.empty() || name.size() > 80 || name.front() == ' ' ||
      name.back() == ' ')
    return false;
  for (const auto c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' &&
        c != '_')
      return false;
  const auto lower = Lower(name);
  if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
    return false;
  if (lower.size() == 4 &&
      (lower.starts_with("com") || lower.starts_with("lpt")) &&
      lower[3] >= '1' && lower[3] <= '9')
    return false;
  return true;
}
inline nlohmann::json Create() {
  nlohmann::json categories = nlohmann::json::array();
  Packs::EnsureDefaultCategories(categories);
  return {{"schemaVersion", Packs::CurrentSchemaVersion},
          {"packId", Packs::Id::New().String()},
          {"categories", categories},
          {"placeables", nlohmann::json::array()}};
}
// Render snapshots can outlive a row reorder. Commands identify records by
// stable item ID (or source identity for unsaved legacy fragments), never by
// a stale table position.
inline bool SameItemSource(const nlohmann::json &a, const nlohmann::json &b) {
  if (a.contains("itemId") && b.contains("itemId"))
    return a["itemId"] == b["itemId"];

  const auto local =
      [](const nlohmann::json &item) -> std::optional<std::uint32_t> {
    const auto &id = item.at("formId");
    if (id.is_string())
      return ParseThumbnailFormID(id.get<std::string>());
    if (id.is_number_unsigned()) {
      const auto value = id.get<std::uint64_t>();
      if (value <= 0xFFFFFFFFULL)
        return static_cast<std::uint32_t>(value);
    } else if (id.is_number_integer()) {
      const auto value = id.get<std::int64_t>();
      if (value >= 0 && value <= 0xFFFFFFFFLL)
        return static_cast<std::uint32_t>(value);
    }
    return std::nullopt;
  };
  const auto left = local(a), right = local(b);
  return left && right && left == right &&
         Lower(a.at("plugin").get<std::string>()) ==
             Lower(b.at("plugin").get<std::string>());
}
inline std::optional<std::size_t>
FindItemIndex(const nlohmann::json &document, const nlohmann::json &source,
              std::optional<std::size_t> hint = std::nullopt) {
  const auto &items = document.at("placeables");
  if (hint && *hint < items.size() && SameItemSource(items[*hint], source))
    return hint;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (SameItemSource(items[i], source))
      return i;
  return std::nullopt;
}
inline void EnsureCategories(nlohmann::json &document) {
  if (!document.contains("categories"))
    document["categories"] = nlohmann::json::array();
  auto &categories = document["categories"];
  if (!categories.is_array())
    return;
  categories.erase(
      std::remove_if(categories.begin(), categories.end(),
                     [](const auto &entry) {
                       return entry.is_string() &&
                              (entry.template get<std::string>().empty() ||
                               Lower(entry.template get<std::string>()) ==
                                   "uncategorized");
                     }),
      categories.end());
  for (auto &item : document.at("placeables")) {
    if (!item.contains("category"))
      item["category"] = "Other";
    if (!item["category"].is_string())
      continue;
    auto category = item["category"].get<std::string>();
    if (category.empty() || Lower(category) == "uncategorized") {
      item["category"] = "Other";
      continue;
    }
    if (!category.empty() && std::find(categories.begin(), categories.end(),
                                       category) == categories.end())
      categories.push_back(category);
  }
  Packs::EnsureDefaultCategories(categories);
}
inline bool MoveItem(nlohmann::json &document, const nlohmann::json &source,
                     const nlohmann::json &before = nullptr) {
  const auto from = FindItemIndex(document, source);
  const auto to =
      before.is_null()
          ? std::optional<std::size_t>{document.at("placeables").size()}
          : FindItemIndex(document, before);
  if (!from || !to)
    throw std::runtime_error("This item is no longer in the pack.");
  if (*from == *to || *from + 1 == *to)
    return false;
  auto &items = document.at("placeables");
  auto item = std::move(items[*from]);
  items.erase(items.begin() + *from);
  items.insert(items.begin() + (*to > *from ? *to - 1 : *to), std::move(item));
  return true;
}
inline void MoveCategory(nlohmann::json &document, const std::string &name,
                         const std::string &before) {
  if (Packs::CategoryKey(name) == "other")
    throw std::runtime_error(
        "Other always stays last and cannot be reordered.");
  auto &categories = document.at("categories");
  auto from = std::find(categories.begin(), categories.end(), name);
  if (from == categories.end())
    throw std::runtime_error("Category no longer exists.");
  if (name == before)
    return;
  if (!before.empty() && std::find(categories.begin(), categories.end(),
                                   before) == categories.end())
    throw std::runtime_error("Target category no longer exists.");
  categories.erase(from);
  const auto to = std::find(categories.begin(), categories.end(),
                            before.empty() ? "Other" : before);
  categories.insert(to, name);
}
inline void RemoveCategory(nlohmann::json &document, std::size_t index) {
  const auto removed =
      Lower(document["categories"].at(index).get<std::string>());
  if (Packs::IsDefaultCategory(removed))
    throw std::runtime_error("Default categories cannot be removed.");
  for (auto &item : document["placeables"])
    if (Lower(item.value("category", "")) == removed)
      item["category"] = "Other";
  document["categories"].erase(document["categories"].begin() + index);
  Packs::EnsureDefaultCategories(document["categories"]);
}
// Delete only the explicitly selected JSON; assets may be shared by packs.
inline void DeletePackFile(const std::filesystem::path &root,
                           const std::filesystem::path &path,
                           const std::string &confirmation) {
  if (confirmation != "DELETE")
    throw std::runtime_error("Type DELETE to confirm pack deletion.");
  if (path.empty() || Lower(path.extension().string()) != ".json" ||
      !std::filesystem::equivalent(path.parent_path(), root) ||
      std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path))
    throw std::runtime_error("Choose a pack JSON in the pack folder.");
  if (!std::filesystem::remove(path))
    throw std::runtime_error("The pack could not be deleted.");
}
inline std::string Validate(const nlohmann::json &document) {
  try {
    (void)Packs::SchemaVersion(document);
  } catch (const std::exception &error) {
    return error.what();
  }
  if (!document.is_object() || !document.contains("placeables") ||
      !document["placeables"].is_array())
    return "Pack must contain an item list.";
  if (!document.contains("categories") || !document["categories"].is_array())
    return "Categories must be an array.";
  if (document.contains("extendBasePack") &&
      !document["extendBasePack"].is_boolean())
    return "Extend base SkyBuild pack must be a toggle.";
  if (document.contains("packId") &&
      (!document["packId"].is_string() ||
       !Packs::Id::Parse(document["packId"].get<std::string>())))
    return "Invalid stable pack ID.";
  std::set<std::string> stableItems;
  for (const auto &item : document["placeables"])
    if (item.contains("itemId")) {
      const auto id = item["itemId"].is_string()
                          ? Packs::Id::Parse(item["itemId"].get<std::string>())
                          : std::nullopt;
      if (!id || !stableItems.insert(id->String()).second)
        return "Invalid or duplicate stable item ID.";
    }
  std::set<std::string> categories, identities;
  for (const auto &category : document["categories"]) {
    if (!category.is_string() || category.get<std::string>().empty() ||
        Lower(category.get<std::string>()) == "uncategorized" ||
        !categories.insert(Lower(category.get<std::string>())).second)
      return "Categories must have unique, nonempty names.";
  }
  for (const auto &item : document["placeables"]) {
    if (!item.is_object() || !item.contains("plugin") ||
        !item["plugin"].is_string() ||
        item["plugin"].get<std::string>().empty() || !item.contains("formId"))
      return "Each item needs a plugin and form ID.";
    const auto plugin = item["plugin"].get<std::string>();
    const auto pluginPath = std::filesystem::path(plugin);
    const auto extension = Lower(pluginPath.extension().string());
    if (pluginPath.has_parent_path() ||
        plugin.find_first_of("/\\:") != std::string::npos ||
        (extension != ".esp" && extension != ".esm" && extension != ".esl"))
      return "Plugin references must be .esp, .esm or .esl filenames.";
    const auto id =
        item["formId"].is_string()
            ? ParseThumbnailFormID(item["formId"].get<std::string>())
            : (item["formId"].is_number_unsigned()
                   ? std::optional{item["formId"].get<std::uint32_t>()}
                   : std::nullopt);
    if (!id || *id > 0xFFFFFF ||
        !identities
             .insert(Lower(item["plugin"].get<std::string>()) +
                     std::format(":{:06X}", *id))
             .second)
      return "Item IDs must be unique local form IDs.";
    try {
      (void)BuildCosts::Calculate(item);
    } catch (const std::exception &error) {
      return error.what();
    }
    const auto type = item.value("type", "");
    if (type != "none" && type != "basic" && type != "side" && type != "corner")
      return "Choose a valid snapping type.";
    if (!item.contains("occupy") || !item["occupy"].is_array())
      return "Choose the occupied slots.";
    std::set<std::string> flags;
    for (const auto &slot : item["occupy"]) {
      if (!slot.is_string())
        return "Occupancy must list slot names.";
      const auto name = slot.get<std::string>();
      if ((name != "center" && name != "bottom" && name != "top" &&
           name != "corners" && name != "sides") ||
          !flags.insert(name).second)
        return "Choose unique, valid occupancy slots.";
    }
    if (type != "basic" && !flags.empty())
      return "Only Basic uses occupancy toggles.";
    if (item.contains("alignToWall") && !item["alignToWall"].is_boolean())
      return "Align to wall must be a boolean.";
    if (item.contains("doorway") &&
        (!item["doorway"].is_boolean() ||
         (item["doorway"].get<bool>() && type != "side")))
      return "Doorway is only available for Side items.";

    if (!item.contains("category") || !item["category"].is_string() ||
        (!item["category"].get<std::string>().empty() &&
         !categories.contains(Lower(item["category"].get<std::string>()))))
      return "Choose an existing category.";
    if (item.contains("name") && !item["name"].is_string())
      return "Item names must be text.";
    if (item.contains("iconYaw") &&
        (!item["iconYaw"].is_number() ||
         !std::isfinite(item["iconYaw"].get<float>())))
      return "Icon rotation must be a finite number.";
    const float iconYaw = item.value("iconYaw", 0.F);
    if (iconYaw < 0 || iconYaw > 270 || std::fmod(iconYaw, 90.F) != 0)
      return "Icon rotation must be 0, 90, 180 or 270 degrees.";
    if (item.contains("iconLight") &&
        (!item["iconLight"].is_number() ||
         !std::isfinite(item["iconLight"].get<float>())))
      return "Icon light brightness must be a finite number.";
    const float iconLight = item.value("iconLight", 1.F);
    if (iconLight < .5F || iconLight > 2.F || std::fmod(iconLight, .5F) != 0)
      return "Icon light brightness must be 50%, 100%, 150% or 200%.";
    for (const auto field : {"freePlaceOffset", "rotationOffset"})
      if (item.contains(field))
        for (const auto key : {"x", "y", "z"}) {
          const auto &offset = item[field];
          if (!offset.is_object() || !offset.contains(key) ||
              !offset[key].is_number() ||
              !std::isfinite(offset[key].get<float>()))
            return "Position and rotation offsets must be finite XYZ values.";
        }
  }
  return {};
}
inline std::filesystem::path IconRelative(const std::string &pack,
                                          const std::string &plugin,
                                          std::uint32_t id, float yaw = 0,
                                          float light = 1) {
  return ThumbnailPaths::Relative(pack, plugin, id, yaw, light);
}
} // namespace SkyBuild::UI::PackDocument
