#pragma once
#include "ui/pack-editor/PackDocument.h"
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>
namespace SkyBuild::UI::PackAssets {
inline std::string ModelKey(std::string model) {
  model = PackDocument::Lower(std::move(model));
  std::replace(model.begin(), model.end(), '\\', '/');
  while (model.starts_with("./"))
    model.erase(0, 2);
  if (model.starts_with("meshes/"))
    model.erase(0, 7);
  return model;
}
using AssetUsers = std::map<std::string, std::set<std::string>>;
using AssetDependencies = std::map<std::string, std::vector<std::string>>;
// Propagate every placeable through shared and nested mesh dependencies.
// Revisit nodes only when new users arrive, so cycles cannot loop indefinitely.
inline void PropagateUsers(AssetUsers &users,
                           const AssetDependencies &dependencies) {
  std::deque<std::string> pending;
  for (const auto &[path, names] : users)
    if (!names.empty())
      pending.push_back(path);
  while (!pending.empty()) {
    const auto path = std::move(pending.front());
    pending.pop_front();
    const auto found = dependencies.find(path);
    if (found == dependencies.end())
      continue;
    for (const auto &child : found->second) {
      auto &target = users[child];
      const auto before = target.size();
      target.insert(users[path].begin(), users[path].end());
      if (target.size() != before)
        pending.push_back(child);
    }
  }
}
inline std::string AssetPath(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  while (text.starts_with("./"))
    text.erase(0, 2);
  if (PackDocument::Lower(text).starts_with("data/"))
    text.erase(0, 5);
  const auto lower = PackDocument::Lower(text);
  const auto ext =
      PackDocument::Lower(std::filesystem::path(text).extension().string());
  if (ext == ".dds" && !lower.starts_with("textures/"))
    text = "textures/" + text;
  if ((ext == ".nif" || ext == ".tri" || ext == ".hkx") &&
      !lower.starts_with("meshes/"))
    text = "meshes/" + text;
  const std::filesystem::path path(text);
  if (path.is_absolute() || text.find(':') != std::string::npos ||
      text.find("..") != std::string::npos || text.empty() ||
      text.front() == '/')
    throw std::runtime_error(
        "Asset paths must be relative to Data, without '..'.");
  if (!PackDocument::Lower(text).starts_with("meshes/") &&
      !PackDocument::Lower(text).starts_with("textures/"))
    throw std::runtime_error("Choose a path under meshes or textures.");
  return text;
}
inline std::vector<std::string> MeshReferences(const std::vector<char> &data) {
  std::set<std::string> result;
  // Skyrim NIF texture paths are length-prefixed strings, including shader
  // texture sets.
  for (std::size_t i = 0; i + 4 < data.size(); ++i) {
    std::uint32_t length = 0;
    std::memcpy(&length, data.data() + i, 4);
    if (length < 5 || length > 1024 || i + 4 + length > data.size())
      continue;
    const std::string text(data.data() + i + 4, length);
    if (!std::ranges::all_of(
            text, [](unsigned char c) { return c >= 32 && c < 127; }))
      continue;
    const auto ext =
        PackDocument::Lower(std::filesystem::path(text).extension().string());
    if (ext != ".dds" && ext != ".nif" && ext != ".tri")
      continue;
    try {
      result.insert(AssetPath(text));
    } catch (...) {
    }
  }
  return {result.begin(), result.end()};
}
} // namespace SkyBuild::UI::PackAssets
