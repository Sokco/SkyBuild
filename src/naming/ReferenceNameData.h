#pragma once
#include <cstdint>
#include <unordered_map>
#include <string>
#include <nlohmann/json.hpp>
#include <cctype>
#include <optional>
#include <stdexcept>
namespace SkyBuild::Naming {
inline std::optional<std::string> CleanObjectName(std::string name) {
  while (!name.empty() &&
         std::isspace(static_cast<unsigned char>(name.front())))
    name.erase(name.begin());
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.pop_back();
  if (name.empty() || name.size() > 128)
    return {};
  for (unsigned char c : name)
    if (c < 32 || c == 127)
      return {};
  return name;
}

inline constexpr std::uint32_t NamesSaveRecord = 0x4D414E52; // RNAM
using ReferenceNames = std::unordered_map<std::uint32_t, std::string>;
inline nlohmann::json EncodeNames(const ReferenceNames &names) {
  auto data = nlohmann::json::array();
  for (const auto &[id, name] : names)
    data.push_back({{"reference", id}, {"name", name}});
  return data;
}
inline ReferenceNames DecodeNames(const nlohmann::json &data) {
  if (!data.is_array() || data.size() > 65536)
    throw std::runtime_error("Invalid reference name list");
  ReferenceNames names;
  for (const auto &item : data) {
    const auto &id = item.at("reference");
    if (!id.is_number_integer() || id <= 0 || id > UINT32_MAX ||
        !item.at("name").is_string())
      throw std::runtime_error("Invalid reference name");
    const auto name = CleanObjectName(item.at("name").get<std::string>());
    if (!name || !names.emplace(id.get<std::uint32_t>(), *name).second)
      throw std::runtime_error("Invalid or duplicate reference name");
  }
  return names;
}
} // namespace SkyBuild::Naming
