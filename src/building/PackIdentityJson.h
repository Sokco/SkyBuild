#pragma once
#include "building/PackIdentity.h"
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
namespace SkyBuild::Packs {
// Assign only absent IDs. Existing/duplicate IDs are never silently replaced.
inline bool EnsureIdentities(nlohmann::json &document) {
  bool changed = false;
  if (!document.contains("packId")) {
    document["packId"] = Id::New().String();
    changed = true;
  }
  if (!document["packId"].is_string() ||
      !Id::Parse(document["packId"].get<std::string>()))
    throw std::runtime_error("Invalid stable pack ID");
  std::set<std::string> seen;
  for (auto &item : document.at("placeables")) {
    if (!item.contains("itemId")) {
      item["itemId"] = Id::New().String();
      changed = true;
    }
    const auto id = item["itemId"].is_string()
                        ? Id::Parse(item["itemId"].get<std::string>())
                        : std::nullopt;
    if (!id || !seen.insert(id->String()).second)
      throw std::runtime_error("Invalid or duplicate stable item ID");
  }
  return changed;
}
} // namespace SkyBuild::Packs
