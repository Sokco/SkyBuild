#pragma once
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace SkyBuild::BuildCosts {
enum class TransitionCost { None, Pay, Refund };
inline TransitionCost CostForTransition(bool existing, bool target) {
  if (existing == target)
    return TransitionCost::None;
  return target ? TransitionCost::Pay : TransitionCost::Refund;
}
struct Material {
  std::string plugin;
  std::uint32_t local{};
  int amount{};
  bool operator==(const Material &) const = default;
};
struct Requirement {
  Material material;
  int available{};
  bool Affordable() const { return available >= material.amount; }
};
inline std::vector<Material> ScaleCosts(const std::vector<Material> &materials,
                                        double multiplier) {
  auto result = materials;
  for (auto &material : result)
    material.amount = static_cast<int>(std::clamp(
        std::ceil(material.amount * multiplier), 1., double(INT32_MAX)));
  return result;
}
inline std::vector<Material>
ConfiguredCosts(const std::vector<Material> &materials, bool sandbox,
                double multiplier, bool alreadyScaled = false) {
  if (sandbox)
    return {};
  return alreadyScaled ? materials : ScaleCosts(materials, multiplier);
}
inline std::vector<Material> Decode(const nlohmann::json &values) {
  if (!values.is_array() || values.size() > 64)
    throw std::runtime_error(
        "Required materials must be a list of at most 64 entries.");
  std::vector<Material> result;
  std::set<std::string> identities;
  for (const auto &value : values) {
    if (!value.is_object() || !value.contains("plugin") ||
        !value["plugin"].is_string() || !value.contains("formId") ||
        !value.contains("amount") || !value["amount"].is_number_integer())
      throw std::runtime_error("Each required material needs a plugin, form ID "
                               "and whole-number amount.");
    Material entry{value["plugin"].get<std::string>()};
    auto lower = entry.plugin;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
      return char(std::tolower(c));
    });
    const auto extension = std::filesystem::path(lower).extension().string();
    if (entry.plugin.empty() ||
        entry.plugin.find_first_of("/\\:") != std::string::npos ||
        (extension != ".esm" && extension != ".esp" && extension != ".esl"))
      throw std::runtime_error(
          "Required materials must reference a plugin filename.");
    if (value["formId"].is_string()) {
      auto text = value["formId"].get<std::string>();
      if (text.starts_with("0x") || text.starts_with("0X"))
        text.erase(0, 2);
      const auto [end, error] = std::from_chars(
          text.data(), text.data() + text.size(), entry.local, 16);
      if (error != std::errc{} || end != text.data() + text.size() ||
          text.empty())
        throw std::runtime_error("Invalid material form ID.");
    } else if (value["formId"].is_number_integer() && value["formId"] >= 0 &&
               value["formId"] <= 0xFFFFFF)
      entry.local = value["formId"].get<std::uint32_t>();
    else
      throw std::runtime_error("Invalid material form ID.");
    if (!entry.local || entry.local > 0xFFFFFF ||
        !identities.insert(lower + std::format(":{:06X}", entry.local)).second)
      throw std::runtime_error(
          "Required materials need unique, valid local form IDs.");
    if (value["amount"] < 1 || value["amount"] > 1000000)
      throw std::runtime_error(
          "Material amounts must be between 1 and 1,000,000.");
    entry.amount = value["amount"].get<int>();
    result.push_back(std::move(entry));
  }
  return result;
}
inline nlohmann::json Encode(const std::vector<Material> &materials) {
  auto result = nlohmann::json::array();
  for (const auto &entry : materials)
    result.push_back({{"plugin", entry.plugin},
                      {"formId", std::format("0x{:06X}", entry.local)},
                      {"amount", entry.amount}});
  return result;
}
// Plan every source before mutating any inventory.
inline std::optional<std::vector<int>>
Allocate(int required, std::span<const int> available) {
  std::vector<int> amounts;
  for (const auto count : available) {
    const auto taken = std::min(required, std::max(0, count));
    amounts.push_back(taken);
    required -= taken;
  }
  return required == 0 ? std::optional{std::move(amounts)} : std::nullopt;
}
} // namespace SkyBuild::BuildCosts
