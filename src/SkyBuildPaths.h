#pragma once
#include <filesystem>
#include <optional>

namespace SkyBuild::Paths {
inline std::filesystem::path Root() {
  return std::filesystem::current_path() / "Data/SkyBuild";
}
inline std::optional<std::filesystem::path> DiagnosticsDirectory() {
  std::error_code error;
  const auto directory = Root() / "logs";
  std::filesystem::create_directories(directory, error);
  if (error)
    return std::nullopt;
  return directory;
}
} // namespace SkyBuild::Paths
