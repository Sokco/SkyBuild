#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <vector>

namespace SkyBuild::Navmesh {
// Owned diagnostic text only. No engine access. Budgeted oldest-first retention
// and exact deduplication keep repeated failures from consuming unbounded disk.
inline std::filesystem::path
ArchiveFailure(const std::filesystem::path &directory, std::string_view payload,
               std::size_t maxFiles = 32,
               std::uintmax_t maxBytes = 128 * 1024 * 1024) {
  static std::mutex mutex;
  const std::lock_guard guard(mutex);
  if (!maxFiles || payload.size() > maxBytes)
    throw std::runtime_error("Navmesh failure exceeds archive budget");
  std::filesystem::create_directories(directory);
  std::vector<std::filesystem::directory_entry> files;
  std::uintmax_t bytes{};
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
        !entry.path().filename().string().starts_with("failure-"))
      continue;
    files.push_back(entry);
    const auto size = entry.file_size();
    bytes += size;
    if (size == payload.size()) {
      std::ifstream input(entry.path(), std::ios::binary);
      const std::string previous((std::istreambuf_iterator<char>(input)), {});
      if (!input.bad() && previous == payload)
        return entry.path();
    }
  }
  std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
    const auto at = a.last_write_time(), bt = b.last_write_time();
    return at == bt ? a.path() < b.path() : at < bt;
  });
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = directory / ("failure-" + std::to_string(stamp) + ".json");
  while (std::filesystem::exists(path))
    path = directory / ("failure-" + std::to_string(++stamp) + ".json");
  auto temporary = path;
  temporary += ".tmp";
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.close();
    if (output.fail())
      throw std::runtime_error("Could not write navmesh failure archive");
    std::filesystem::rename(temporary, path);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
  // Publish the new evidence before pruning any prior evidence.
  auto count = files.size() + 1;
  bytes += payload.size();
  for (const auto &entry : files) {
    if (count <= maxFiles && bytes <= maxBytes)
      break;
    const auto size = entry.file_size();
    if (std::filesystem::remove(entry.path())) {
      bytes -= size;
      --count;
    }
  }
  return path;
}
} // namespace SkyBuild::Navmesh
