#pragma once
#include "building/BuildCostData.h"
#include <array>
#include <map>
#include <cmath>
#include <string_view>
namespace SkyBuild::BuildCosts {
struct StandardMaterial {
  const char *key;
  const char *label;
  const char *plugin;
  std::uint32_t local;
  int units;
};
inline constexpr std::array<StandardMaterial, 18> StandardMaterials{
    {{"wood", "Wood", "Skyrim.esm", 0x6F993, 2},
     {"stone", "Stone", "HearthFires.esm", 0x306C, 2},
     {"iron", "Iron", "Skyrim.esm", 0x5ACE4, 1},
     {"steel", "Steel", "Skyrim.esm", 0x5ACE5, 1},
     {"leather", "Leather", "Skyrim.esm", 0xDB5D2, 1},
     {"leatherStrips", "Leather Strips", "Skyrim.esm", 0x800E4, 4},
     {"clay", "Clay", "HearthFires.esm", 0x3043, 2},
     {"linenWrap", "Linen Wrap", "Skyrim.esm", 0x34CD6, 1},
     {"nails", "Nails", "HearthFires.esm", 0x300F, 1},
     {"corundum", "Corundum", "Skyrim.esm", 0x5AD93, 1},
     {"dwarven", "Dwarven Metal", "Skyrim.esm", 0xDB8A2, 1},
     {"orichalcum", "Orichalcum", "Skyrim.esm", 0x5AD99, 1},
     {"moonstone", "Moonstone", "Skyrim.esm", 0x5AD9F, 1},
     {"malachite", "Malachite", "Skyrim.esm", 0x5ADA1, 1},
     {"quicksilver", "Quicksilver", "Skyrim.esm", 0x5ADA0, 1},
     {"ebony", "Ebony", "Skyrim.esm", 0x5AD9D, 1},
     {"silver", "Silver", "Skyrim.esm", 0x5ACE3, 1},
     {"gold", "Gold", "Skyrim.esm", 0x5AD9E, 1}}};
inline constexpr std::array<const char *, 7> CostSizes{
    "Auto", "Extra Small", "Small", "Medium", "Large", "Extra Large", "Huge"};
inline constexpr std::array<double, 7> CostEdges{0, 16, 32, 64, 128, 256, 512};
inline int CostSizeIndex(std::string_view size) {
  for (int i = 0; i < 7; ++i)
    if (size == CostSizes[i])
      return i;
  throw std::runtime_error("Choose a valid object size.");
}
inline double CostVolume(const nlohmann::json &item) {
  const auto size =
      CostSizeIndex(item.value("objectSize", std::string("Medium")));
  if (size)
    return CostEdges[size] * CostEdges[size] * CostEdges[size];
  const auto bounds =
      item.value("costBounds", nlohmann::json::array({64, 64, 64}));
  if (!bounds.is_array() || bounds.size() != 3)
    throw std::runtime_error("Invalid automatic cost bounds.");
  double volume = 1;
  for (const auto &entry : bounds) {
    if (!entry.is_number())
      throw std::runtime_error("Invalid automatic cost bounds.");
    const auto dimension = entry.get<double>();
    if (!std::isfinite(dimension) || dimension < 0)
      throw std::runtime_error("Invalid automatic cost bounds.");
    volume *= std::max(1., dimension);
  }
  return volume;
}
struct ShellWeights {
  std::uint64_t total{};
  std::map<std::string, std::uint64_t> materials;
};
inline ShellWeights ReadShellWeights(const nlohmann::json &item) {
  const auto &shell = item.at("materialShell");
  if (!shell.is_object() || shell.value("voxelSize", 0.) != 1. ||
      !shell.contains("textures") || !shell["textures"].is_array() ||
      shell["textures"].empty() || shell["textures"].size() > 4096)
    throw std::runtime_error("Invalid one-unit material shell.");
  ShellWeights result;
  std::set<std::string> textures;
  for (const auto &entry : shell["textures"]) {
    const auto texture = entry.at("texture").get<std::string>();
    const auto key = entry.at("material").get<std::string>();
    const auto &voxels = entry.at("voxels");
    if (!voxels.is_number_integer() || voxels <= 0 || voxels > 4000000 ||
        !textures.insert(texture).second)
      throw std::runtime_error("Invalid shell texture voxel counts.");
    const auto count = voxels.get<std::uint64_t>();
    result.total += count;
    if (result.total > 4000000)
      throw std::runtime_error("Material shell exceeds four million voxels.");
    if (key != "none") {
      if (std::ranges::find(StandardMaterials, key, &StandardMaterial::key) ==
          StandardMaterials.end())
        throw std::runtime_error("Choose a valid material for each texture.");
      result.materials[key] += count;
    }
  }
  return result;
}
inline int CostAmount(const StandardMaterial &material,
                      const nlohmann::json &item) {
  if ((item.contains("materialShell") && !item["materialShell"].is_null())) {
    const auto weights = ReadShellWeights(item);
    const auto found = weights.materials.find(material.key);
    if (found == weights.materials.end())
      return 0;
    const auto size =
        CostSizeIndex(item.value("objectSize", std::string("Auto")));
    const double surface =
        size ? 6. * CostEdges[size] * CostEdges[size] : double(weights.total);
    return static_cast<int>(std::clamp(
        std::ceil(material.units * 2. * surface * double(found->second) /
                  double(weights.total) / (6. * 64. * 64.)),
        1., 1000000.));
  }
  // Normalize volume to a 64-unit cube and divide it among selected materials.
  const auto choices = item.value("materials", nlohmann::json::array());
  const double shareCount =
      static_cast<double>(std::max<std::size_t>(1, choices.size()));
  return static_cast<int>(
      std::clamp(std::ceil(material.units * 2. * CostVolume(item) /
                           (64. * 64. * 64. * shareCount)),
                 1., 1000000.));
}
inline std::vector<std::string> SuggestCostMaterials(std::string text) {
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return char(std::tolower(c)); });
  const auto has = [&](std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(
        words, [&](auto word) { return text.find(word) != std::string::npos; });
  };
  std::set<std::string> selected;
  if (has({"wood", "timber", "plank", "bark", "trunk", "branch", "pine",
           "birch", "aspen"}))
    selected.insert("wood");
  auto stoneText = text;
  for (auto at = stoneText.find("moonstone"); at != std::string::npos;
       at = stoneText.find("moonstone"))
    stoneText.replace(at, 9, " ");
  if (stoneText.find("stone") != std::string::npos ||
      has({"rock", "brick", "cobble", "marble", "granite", "masonry",
           "plaster"}))
    selected.insert("stone");
  // Specific alloys take priority over generic metal texture naming.
  for (const auto *key :
       {"steel", "corundum", "orichalcum", "moonstone", "malachite",
        "quicksilver", "ebony", "silver", "gold"})
    if (has({key}) &&
        (std::string_view(key) != "silver" || !has({"quicksilver"})))
      selected.insert(key);
  if (has({"dwarven", "dwemer", "dwemetal"}))
    selected.insert("dwarven");
  if (has({"iron"}))
    selected.insert("iron");
  if (has({"metal"}) &&
      !has({"iron", "steel", "dwarven", "dwemer", "dwemetal", "corundum",
            "orichalcum", "moonstone", "malachite", "quicksilver", "ebony",
            "silver", "gold"}))
    selected.insert("iron");
  if (has({"leatherstrip", "leather_strip", "leather strip"}))
    selected.insert("leatherStrips");
  else if (has({"leather", "hide", "pelt", "banner"}))
    selected.insert("leather");
  if (has({"clay", "terracotta"}))
    selected.insert("clay");
  if (selected.empty())
    selected.insert("wood");
  std::vector<std::string> result;
  for (const auto &material : StandardMaterials)
    if (selected.contains(material.key))
      result.emplace_back(material.key);
  return result;
}
inline std::vector<Material> CalculateComponentCosts(const nlohmann::json &analysis) {
  const bool projected = analysis.contains("woodBudget");
  const double budget = analysis.value("woodBudget", 0.);
  if (projected && (!std::isfinite(budget) || budget < 1. || budget > 1000000.))
    throw std::runtime_error("Invalid automatic material budget.");
  double total = 0.;
  std::map<std::string, double> weights;
  for (const auto &row : analysis.at("textures")) {
    const auto material = row.at("material").get<std::string>();
    const double weight =
        row.at(projected ? "footprint" : "volume").get<double>();
    if (!std::isfinite(weight) || weight < 0.)
      throw std::runtime_error("Invalid material weight.");
    total += weight;
    if (material != "none")
      weights[material] += weight;
  }
  std::vector<Material> derived;
  for (const auto &[key, weight] : weights) {
    const auto found =
        std::ranges::find(StandardMaterials, key, &StandardMaterial::key);
    if (found == StandardMaterials.end())
      throw std::runtime_error("Invalid component material.");
    if (weight == 0.)
      continue;
    const double quantity =
        projected ? budget * (weight / total) * (found->units / 2.)
                  : found->units * 2. * weight / (64. * 64. * 64.);
    const int amount =
        static_cast<int>(std::clamp(std::ceil(quantity), 1., 1000000.));
    derived.push_back({found->plugin, found->local, amount});
  }
  return derived;
}
// Retained only to migrate packs written before schema version 1.
inline std::vector<Material> CalculateLegacy(const nlohmann::json &item) {
  if (item.value("costMethod", std::string{}) == "components" ||
      item.value("costMethod", std::string{}) == "manual")
    return Decode(item.value("requiredMaterials", nlohmann::json::array()));
  if (item.value("costMethod", std::string{}) == "componentsPending")
    throw std::runtime_error("Analyze materials with Auto first.");
  if (item.value("costMethod", std::string{}) == "shell" &&
      !(item.contains("materialShell") && !item["materialShell"].is_null()))
    throw std::runtime_error(
        "Analyze the triangle shell before using automatic costs.");
  if ((item.contains("materialShell") && !item["materialShell"].is_null()))
    (void)ReadShellWeights(item);
  else
    (void)CostVolume(item);
  auto result =
      Decode(item.value("requiredMaterials", nlohmann::json::array()));
  auto choices = item.value("materials", nlohmann::json::array());
  if ((item.contains("materialShell") && !item["materialShell"].is_null())) {
    choices = nlohmann::json::array();
    for (const auto &[key, count] : ReadShellWeights(item).materials)
      choices.push_back(key);
  }
  if (!choices.is_array() || choices.size() > StandardMaterials.size())
    throw std::runtime_error("Choose a valid material list.");
  std::set<std::string> seen;
  for (const auto &choice : choices) {
    if (!choice.is_string())
      throw std::runtime_error("Invalid automatic material.");
    const auto key = choice.get<std::string>();
    const auto found =
        std::ranges::find(StandardMaterials, key, &StandardMaterial::key);
    if (found == StandardMaterials.end() || !seen.insert(key).second)
      throw std::runtime_error("Invalid or duplicate automatic material.");
    const int amount = CostAmount(*found, item);
    const auto existing =
        std::ranges::find_if(result, [&](const auto &material) {
          auto plugin = material.plugin;
          std::ranges::transform(plugin, plugin.begin(), [](unsigned char c) {
            return char(std::tolower(c));
          });
          auto standard = std::string(found->plugin);
          std::ranges::transform(
              standard, standard.begin(),
              [](unsigned char c) { return char(std::tolower(c)); });
          return material.local == found->local && plugin == standard;
        });
    if (existing == result.end())
      result.push_back({found->plugin, found->local, amount});
    else {
      if (existing->amount > 1000000 - amount)
        throw std::runtime_error("Combined material amount exceeds 1,000,000.");
      existing->amount += amount;
    }
  }
  return result;
}
inline std::vector<Material> Calculate(const nlohmann::json &item) {
  return Decode(item.value("requiredMaterials", nlohmann::json::array()));
}
} // namespace SkyBuild::BuildCosts
