#pragma once
#include "models/ModelAsset.h"
#include "ui/thumbnails/ThumbnailRenderer.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>

namespace SkyBuild::Models {
using VertexBounds = UI::ThumbnailRenderer::Bounds;
// Game-thread cache: one CPU-only read per model, never a GPU readback.
inline std::optional<VertexBounds> GetVertexBounds(RE::TESBoundObject *object,
                                                   bool refresh = false) {
  const auto model = WorldModel(object);
  if (!model || !model->GetModel() || !*model->GetModel())
    return {};
  static std::unordered_map<std::string, std::optional<VertexBounds>> cache;
  const std::string path = model->GetModel();
  if (const auto found = cache.find(path); !refresh && found != cache.end())
    return found->second;
  std::optional<VertexBounds> result;
  try {
    result = UI::ThumbnailRenderer::ReadBounds(path, [](std::string resource) {
      std::replace(resource.begin(), resource.end(), '/', '\\');
      RE::BSResourceNiBinaryStream stream(resource);
      UI::ThumbnailRenderer::Bytes bytes;
      if (stream.good() && stream.stream) {
        bytes.resize(stream.stream->totalSize);
        if (!bytes.empty() &&
            !stream.read(reinterpret_cast<char *>(bytes.data()),
                         static_cast<std::uint32_t>(bytes.size())))
          bytes.clear();
      }
      return bytes;
    });
  } catch (const std::exception &error) {
    spdlog::warn("Cannot derive selection bounds for '{}': {}", path,
                 error.what());
  }
  if (result || !refresh)
    cache.insert_or_assign(path, result);
  return result;
}
inline std::optional<VertexBounds>
DecodeVertexBounds(const nlohmann::json &item) {
  try {
    const auto &value = item.at("selectionBounds");
    VertexBounds result{value.at("min").get<std::array<float, 3>>(),
                        value.at("max").get<std::array<float, 3>>()};
    for (int i = 0; i < 3; ++i)
      if (!std::isfinite(result.min[i]) || !std::isfinite(result.max[i]) ||
          result.min[i] > result.max[i])
        return {};
    return result;
  } catch (...) {
    return {};
  }
}
inline bool EnsureVertexBounds(nlohmann::json &item,
                               RE::TESBoundObject *object) {
  if (DecodeVertexBounds(item))
    return false;
  if (const auto bounds = GetVertexBounds(object)) {
    item["selectionBounds"] = {{"min", bounds->min}, {"max", bounds->max}};
    return true;
  }
  return false;
}
} // namespace SkyBuild::Models
