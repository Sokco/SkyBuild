#include "models/VertexBounds.h"
#include "building/AutomaticBuildCosts.h"
#include "building/PlaceableRegistry.h"
#include "building/BuildingManager.h"
#include "building/PackIdentityJson.h"
#include "building/PackCategories.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_set>

namespace SkyBuild::PlaceableRegistry {
namespace {
std::vector<Definition> definitions{};
std::unordered_set<std::string> packIdentities;

std::string ToLower(std::string a_value) {
  std::ranges::transform(a_value, a_value.begin(),
                         [](unsigned char a_character) {
                           return static_cast<char>(std::tolower(a_character));
                         });
  return a_value;
}

std::optional<RE::FormID> ParseFormID(const nlohmann::json &a_value,
                                      std::string_view a_pluginName) {
  std::uint64_t value = 0;
  if (a_value.is_number_unsigned()) {
    value = a_value.get<std::uint64_t>();
  } else if (a_value.is_string()) {
    auto text = a_value.get<std::string>();
    if (text.starts_with("0x") || text.starts_with("0X")) {
      text.erase(0, 2);
    }
    if (text.empty()) {
      return std::nullopt;
    }
    std::size_t parsedCharacters = 0;
    try {
      value = std::stoull(text, &parsedCharacters, 16);
    } catch (const std::exception &) {
      return std::nullopt;
    }
    if (parsedCharacters != text.size()) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }

  if (value == 0 || value > 0xFFFFFFFF) {
    return std::nullopt;
  }

  if (value > 0x00FFFFFF) {
    const auto dataHandler = RE::TESDataHandler::GetSingleton();
    const auto plugin =
        dataHandler ? dataHandler->LookupModByName(a_pluginName) : nullptr;
    const bool isLightPlugin =
        plugin ? plugin->IsLight() : (value & 0xFF000000) == 0xFE000000;
    const auto fullFormID = value;
    value &= isLightPlugin ? 0x00000FFF : 0x00FFFFFF;
    spdlog::info(
        "Normalized full FormID {:08X} from '{}' to local FormID {:06X}.",
        fullFormID, a_pluginName, value);
  }

  if (value == 0) {
    return std::nullopt;
  }
  return static_cast<RE::FormID>(value);
}

std::optional<PlacementPreview::PlaceableType>
ParseType(const nlohmann::json &value) {
  if (!value.is_string())
    return std::nullopt;
  const auto type = ToLower(value.get<std::string>());
  if (type == "none")
    return Grid::SnapMode::kNone;
  if (type == "basic")
    return Grid::SnapMode::kBasic;
  if (type == "side")
    return Grid::SnapMode::kSide;
  if (type == "corner")
    return Grid::SnapMode::kCorner;
  return std::nullopt;
}
std::optional<Grid::Settings> ParseGrid(const nlohmann::json &entry,
                                        Grid::SnapMode mode) {
  Grid::Settings settings{0};
  if (entry.contains("doorway") &&
      (!entry["doorway"].is_boolean() ||
       (entry["doorway"].get<bool>() && mode != Grid::SnapMode::kSide)))
    return std::nullopt;
  if (entry.value("doorway", false))
    settings.sideRole = Grid::SideRole::kDoorway;
  if (mode != Grid::SnapMode::kBasic) {
    if (entry.contains("occupy") &&
        (!entry["occupy"].is_array() || !entry["occupy"].empty()))
      return std::nullopt;
    return settings;
  }
  if (!entry.contains("occupy") || !entry["occupy"].is_array())
    return std::nullopt;
  for (const auto &value : entry["occupy"]) {
    if (!value.is_string())
      return std::nullopt;
    const auto slot = ToLower(value.get<std::string>());
    if (slot == "center")
      settings.occupy |= Grid::kCenter;
    else if (slot == "bottom")
      settings.occupy |= Grid::kBottom;
    else if (slot == "top")
      settings.occupy |= Grid::kTop;
    else if (slot == "corners")
      settings.occupy |= Grid::kCorners;
    else if (slot == "sides")
      settings.occupy |= Grid::kSides;
    else
      return std::nullopt;
  }
  return settings;
}

std::optional<RE::NiPoint3> ParseOffset(const nlohmann::json &a_value) {
  if (!a_value.is_object()) {
    return std::nullopt;
  }

  for (const auto component : {"x", "y", "z"}) {
    if (!a_value.contains(component) || !a_value.at(component).is_number()) {
      return std::nullopt;
    }
  }
  return RE::NiPoint3{a_value.at("x").get<float>(),
                      a_value.at("y").get<float>(),
                      a_value.at("z").get<float>()};
}

void LoadFile(const std::filesystem::path &a_path, std::string_view a_packName,
              std::unordered_set<std::string> &a_seenForms) {
  std::ifstream stream{a_path};
  if (!stream) {
    spdlog::error("Could not open placeable definition file '{}'.",
                  a_path.string());
    return;
  }

  try {
    auto document = nlohmann::json::parse(stream);
    if (!document.is_object() || !document.contains("placeables") ||
        !document.at("placeables").is_array()) {
      spdlog::error(
          "Placeable definition file '{}' must contain a 'placeables' array.",
          a_path.string());
      return;
    }

    if (document.contains("extendBasePack") &&
        !document["extendBasePack"].is_boolean()) {
      spdlog::error("Invalid extendBasePack toggle in '{}'.", a_path.string());
      return;
    }
    if (Packs::EnsureIdentities(document)) {
      stream.close();
      auto backup = a_path;
      backup += ".pre-ids.bak";
      if (!std::filesystem::exists(backup))
        std::filesystem::copy_file(a_path, backup);
      auto temporary = a_path;
      temporary += ".tmp";
      std::ofstream output(temporary, std::ios::binary);
      output << document.dump(2) << '\n';
      output.close();
      if (!output || !MoveFileExW(temporary.c_str(), a_path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Could not persist stable pack IDs");
    }
    const auto packId =
        *Packs::Id::Parse(document.at("packId").get<std::string>());
    if (!packIdentities.insert(packId.String()).second)
      throw std::runtime_error("Duplicate stable pack ID");
    const bool extendBasePack = document.value("extendBasePack", false);
    auto categoryOrder = document.value("categories", nlohmann::json::array());
    Packs::EnsureDefaultCategories(categoryOrder);
    const auto firstDefinition = definitions.size();
    bool boundsChanged = false;
    std::size_t entryIndex = 0;
    for (auto &entry : document.at("placeables")) {
      const auto logInvalid = [&]() {
        spdlog::error("Ignoring invalid placeable entry {} in '{}'.",
                      entryIndex, a_path.string());
      };

      if (!entry.is_object() || !entry.contains("plugin") ||
          !entry.at("plugin").is_string() || !entry.contains("formId") ||
          !entry.contains("type")) {
        logInvalid();
        ++entryIndex;
        continue;
      }

      Definition definition{};
      definition.identity = {
          packId, *Packs::Id::Parse(entry.at("itemId").get<std::string>())};
      definition.packName = a_packName;
      definition.extendBasePack = extendBasePack;
      definition.pluginName = entry.at("plugin").get<std::string>();
      const auto localFormID =
          ParseFormID(entry.at("formId"), definition.pluginName);
      const auto type = ParseType(entry.at("type"));
      if (definition.pluginName.empty() || !localFormID || !type) {
        logInvalid();
        ++entryIndex;
        continue;
      }

      definition.localFormID = *localFormID;
      if (auto *handler = RE::TESDataHandler::GetSingleton())
        boundsChanged |= Models::EnsureVertexBounds(
            entry, handler->LookupForm<RE::TESBoundObject>(
                       *localFormID, definition.pluginName));
      definition.selectionBounds = Models::DecodeVertexBounds(entry);
      try {
        auto costItem = entry;
        if (entry.value("objectSize", std::string("Medium")) == "Auto")
          if (auto *handler = RE::TESDataHandler::GetSingleton())
            if (const auto *object = handler->LookupForm<RE::TESBoundObject>(
                    *localFormID, definition.pluginName)) {
              const auto &bounds = object->boundData;
              costItem["costBounds"] = nlohmann::json::array(
                  {float(bounds.boundMax.x) - bounds.boundMin.x,
                   float(bounds.boundMax.y) - bounds.boundMin.y,
                   float(bounds.boundMax.z) - bounds.boundMin.z});
            }
        definition.requiredMaterials = BuildCosts::Calculate(costItem);
      } catch (const std::exception &error) {
        spdlog::error("Invalid build cost in '{}': {}", a_path.string(),
                      error.what());
        ++entryIndex;
        continue;
      }

      definition.type = *type;
      const auto grid = ParseGrid(entry, *type);
      if (!grid) {
        logInvalid();
        ++entryIndex;
        continue;
      }
      definition.grid = *grid;
      if (entry.contains("alignToWall") && !entry["alignToWall"].is_boolean()) {
        logInvalid();
        ++entryIndex;
        continue;
      }
      definition.alignToWall = entry.value("alignToWall", false);
      definition.iconYaw = entry.value("iconYaw", 0.F);
      definition.iconLight =
          std::clamp(entry.value("iconLight", 1.F), .5F, 2.F);
      definition.category = "Other";
      if (entry.contains("category")) {
        if (!entry.at("category").is_string()) {
          spdlog::warn("Skipping placeable with a non-string category.");
          continue;
        }
        const auto category = entry.at("category").get<std::string>();
        auto categoryKey = category;
        std::transform(
            categoryKey.begin(), categoryKey.end(), categoryKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        definition.category = category.empty() || categoryKey == "uncategorized"
                                  ? "Other"
                                  : category;
      }
      if (entry.contains("name")) {
        if (!entry.at("name").is_string()) {
          logInvalid();
          ++entryIndex;
          continue;
        }
        definition.name = entry.at("name").get<std::string>();
      }
      if (entry.contains("freePlaceOffset")) {
        const auto offset = ParseOffset(entry.at("freePlaceOffset"));
        if (!offset) {
          logInvalid();
          ++entryIndex;
          continue;
        }
        definition.freePlaceOffset = *offset;
      }

      if (entry.contains("rotationOffset")) {
        const auto offset = ParseOffset(entry.at("rotationOffset"));
        if (!offset || !std::isfinite(offset->x) || !std::isfinite(offset->y) ||
            !std::isfinite(offset->z)) {
          logInvalid();
          ++entryIndex;
          continue;
        }
        definition.rotationOffset =
            *offset * (std::numbers::pi_v<float> / 180.F);
      }
      const auto identity = ToLower(definition.pluginName) + ':' +
                            std::format("{:06X}", definition.localFormID);
      if (!a_seenForms.insert(identity).second) {
        spdlog::warn("Ignoring duplicate placeable {} from '{}'.", identity,
                     a_path.string());
        ++entryIndex;
        continue;
      }

      definitions.push_back(std::move(definition));
      ++entryIndex;
    }
    if (boundsChanged) {
      stream.close();
      auto temporary = a_path;
      temporary += ".bounds.tmp";
      std::ofstream output(temporary, std::ios::binary);
      output << document.dump(2) << '\n';
      output.close();
      if (!output || !MoveFileExW(temporary.c_str(), a_path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING))
        spdlog::warn("Could not persist vertex bounds in '{}'",
                     a_path.string());
    }
    const auto rank = [&](const std::string &category) {
      const auto key = Packs::CategoryKey(category);
      for (std::size_t i = 0; i < categoryOrder.size(); ++i)
        if (categoryOrder[i].is_string() &&
            Packs::CategoryKey(categoryOrder[i].get<std::string>()) == key)
          return i;
      return categoryOrder.size() + (category.empty() ? 1 : 0);
    };
    std::stable_sort(definitions.begin() + firstDefinition, definitions.end(),
                     [&](const auto &a, const auto &b) {
                       return rank(a.category) < rank(b.category);
                     });
  } catch (const std::exception &exception) {
    spdlog::error("Could not parse placeable definition file '{}': {}",
                  a_path.string(), exception.what());
  }
}
} // namespace

bool Load() {
  definitions.clear();
  packIdentities.clear();
  const auto directory =
      std::filesystem::current_path() / "Data" / "SkyBuild" / "packs";
  std::error_code error{};
  if (!std::filesystem::is_directory(directory, error)) {
    spdlog::error("Placeable pack folder '{}' does not exist.",
                  directory.string());
    return false;
  }

  std::vector<std::filesystem::path> files{};
  for (std::filesystem::directory_iterator iterator{directory, error}, end;
       iterator != end && !error; ++iterator) {
    if (iterator->is_regular_file() &&
        ToLower(iterator->path().extension().string()) == ".json") {
      files.push_back(iterator->path());
    }
  }
  if (error) {
    spdlog::error("Could not enumerate placeable pack folder '{}': {}",
                  directory.string(), error.message());
    return false;
  }

  std::ranges::sort(files);
  for (const auto &file : files) {
    std::unordered_set<std::string> seenForms{};
    LoadFile(file, file.stem().string(), seenForms);
  }

  std::size_t resolvedCount = 0;
  for (auto &definition : definitions) {
    if (Resolve(definition)) {
      ++resolvedCount;
    }
  }

  spdlog::info("Loaded {} placeable definition(s) from {} pack file(s); "
               "resolved {} form(s).",
               definitions.size(), files.size(), resolvedCount);
  BuildingManager::RefreshPackSettings();
  return resolvedCount > 0;
}

const Definition *Find(Packs::Identity identity) {
  if (!identity)
    return nullptr;
  const auto it = std::ranges::find_if(
      definitions, [&](const auto &d) { return d.identity == identity; });
  return it == definitions.end() ? nullptr : &*it;
}
const Definition *FindUnique(RE::TESBoundObject *object) {
  const Definition *match = nullptr;
  if (!object)
    return nullptr;
  for (auto &d : definitions)
    if (Resolve(d) == object) {
      if (match)
        return nullptr; // Old saves cannot disambiguate two packs using the
                        // same base.
      match = &d;
    }
  return match;
}
std::vector<Definition> &GetDefinitions() { return definitions; }

RE::TESBoundObject *Resolve(Definition &a_definition) {
  if (a_definition.resolvedFormID) {
    return RE::TESForm::LookupByID<RE::TESBoundObject>(
        a_definition.resolvedFormID);
  }

  const auto dataHandler = RE::TESDataHandler::GetSingleton();
  const auto form = dataHandler
                        ? dataHandler->LookupForm(a_definition.localFormID,
                                                  a_definition.pluginName)
                        : nullptr;
  const auto object = form ? form->As<RE::TESBoundObject>() : nullptr;
  if (!object) {
    spdlog::warn("Could not resolve placeable local FormID {:06X} from '{}'.",
                 a_definition.localFormID, a_definition.pluginName);
    return nullptr;
  }

  a_definition.resolvedFormID = object->GetFormID();
  if (object->As<RE::TESObjectDOOR>()) {
    a_definition.type = Grid::SnapMode::kSide;
    a_definition.grid = {0, Grid::SideRole::kDoor};
  }
  if (a_definition.name.empty()) {
    a_definition.name = object->GetName();
  }
  return object;
}
} // namespace SkyBuild::PlaceableRegistry
