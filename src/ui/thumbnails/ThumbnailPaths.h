#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace SkyBuild::UI::ThumbnailPaths {
// Relative to the UI root; generated files are separate from distributable
// assets. Bump this version when renderer changes require existing thumbnails
// to regenerate.
inline constexpr auto CacheRoot = "../Cache/Thumbnails/v1/";
inline std::filesystem::path Relative(std::string_view pack,
                                      std::string_view plugin,
                                      std::uint32_t local, float yaw = 0,
                                      float light = 1) {
  auto stem = std::string(plugin);
  for (auto &c : stem)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      c = '_';
  if (pack == ".hover" || pack == ".resource-search")
    return std::filesystem::path("thumbnails") / pack /
           std::format("{}-{:06X}.png", stem, local);
  const bool resource = pack == ".resources";
  return std::filesystem::path(CacheRoot) / (resource ? "resources" : "items") /
         std::format("{}-{:06X}-y{}-l{}.png", stem, local,
                     resource ? 0 : int(std::lround(yaw)),
                     resource ? 100 : int(std::lround(light * 100)));
}
} // namespace SkyBuild::UI::ThumbnailPaths
