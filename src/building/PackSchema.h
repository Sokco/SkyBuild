#pragma once
#include "building/AutomaticBuildCosts.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace SkyBuild::Packs {
inline constexpr int CurrentSchemaVersion = 1;
inline int SchemaVersion(const nlohmann::json &document) {
  if (!document.is_object())
    throw std::runtime_error("Pack must be a JSON object.");
  if (!document.contains("schemaVersion"))
    return 0;
  const auto &version = document.at("schemaVersion");
  if (!version.is_number_integer() || version < 0)
    throw std::runtime_error("Invalid pack schemaVersion.");
  if (version > CurrentSchemaVersion)
    throw std::runtime_error("This pack requires a newer version of SkyBuild.");
  return version.get<int>();
}
inline bool MigratePack(
    nlohmann::json &document,
    const std::function<void(nlohmann::json &)> &prepareLegacyCosts = {}) {
  const auto version = SchemaVersion(document);
  if (!document.contains("placeables") || !document["placeables"].is_array())
    throw std::runtime_error("Pack must contain an item list.");
  if (version == CurrentSchemaVersion)
    return false;
  // Convert a copy so a failure cannot leave a partially migrated pack.
  auto migrated = document;
  for (auto &item : migrated["placeables"]) {
    if (!item.is_object())
      throw std::runtime_error("Pack item must be a JSON object.");
    auto legacy = item;
    const auto method = legacy.value("costMethod", std::string{});
    if (prepareLegacyCosts && method != "manual" && method != "components")
      prepareLegacyCosts(legacy);
    item["requiredMaterials"] = BuildCosts::Encode(BuildCosts::CalculateLegacy(legacy));
    for (const auto field : {"pivot", "materialComponents", "autoDerivedMaterials",
                             "materialShell", "materials", "objectSize",
                             "costBounds", "costMethod"})
      item.erase(field);
    (void)BuildCosts::Calculate(item);
  }
  migrated["schemaVersion"] = CurrentSchemaVersion;
  document = std::move(migrated);
  return true;
}
inline void BackupLegacyPack(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path))
    return;
  std::ifstream input(path);
  const auto original = nlohmann::json::parse(input);
  if (SchemaVersion(original) == CurrentSchemaVersion)
    return;
  auto backup = path;
  backup += ".pre-schema-1.bak";
  if (!std::filesystem::exists(backup))
    std::filesystem::copy_file(path, backup);
}
} // namespace SkyBuild::Packs
