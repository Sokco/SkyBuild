#pragma once
#include "ui/pack-editor/PackDocument.h"
#include <fstream>
#include <lz4frame.h>
#include <memory>
#include <unordered_map>
namespace SkyBuild::UI::PackAssets {
// SSE BSA v105 names and records are indexed once; asset contents are read only
// when a pack references them. Compare the effective game resource with these
// originals instead of assuming a vanilla path cannot have a mod override.
class OfficialAssets {
  struct Entry {
    std::shared_ptr<const std::filesystem::path> archive;
    std::uint32_t offset, size;
    bool compressed, prefix;
  };
  std::unordered_map<std::string, std::vector<Entry>> entries;
  static std::string Key(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return PackDocument::Lower(value);
  }
  template <class T> static T Read(std::istream &file) {
    T value{};
    if (!file.read(reinterpret_cast<char *>(&value), sizeof(value)))
      throw std::runtime_error("Truncated official archive.");
    return value;
  }
  void Index(const std::filesystem::path &path) {
    const auto archive = std::make_shared<const std::filesystem::path>(path);
    std::ifstream file(path, std::ios::binary);
    if (Read<std::uint32_t>(file) != 0x00415342 ||
        Read<std::uint32_t>(file) != 105)
      throw std::runtime_error("Unsupported official archive: " +
                               path.string());
    const auto offset = Read<std::uint32_t>(file),
               flags = Read<std::uint32_t>(file);
    const auto folders = Read<std::uint32_t>(file),
               count = Read<std::uint32_t>(file);
    Read<std::uint32_t>(file);
    const auto namesSize = Read<std::uint32_t>(file);
    Read<std::uint32_t>(file);
    if ((flags & 3) != 3 || folders > 1000000 || count > 4000000 ||
        namesSize > 128 * 1024 * 1024)
      throw std::runtime_error("Invalid official archive index.");
    file.seekg(offset);
    std::vector<std::uint32_t> counts;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < folders; ++i) {
      Read<std::uint64_t>(file);
      counts.push_back(Read<std::uint32_t>(file));
      sum += counts.back();
      Read<std::uint32_t>(file);
      Read<std::uint64_t>(file);
    }
    if (sum != count)
      throw std::runtime_error("Invalid archive file count.");
    std::vector<std::pair<std::string, Entry>> pending;
    pending.reserve(count);
    for (auto n : counts) {
      const auto length = Read<std::uint8_t>(file);
      std::string folder(length, '\0');
      if (!file.read(folder.data(), length))
        throw std::runtime_error("Truncated archive folder.");
      if (!folder.empty() && folder.back() == '\0')
        folder.pop_back();
      for (std::uint32_t i = 0; i < n; ++i) {
        Read<std::uint64_t>(file);
        const auto size = Read<std::uint32_t>(file),
                   at = Read<std::uint32_t>(file);
        pending.emplace_back(folder,
                             Entry{archive, at, size & 0x3fffffff,
                                   bool(flags & 4) != bool(size & 0x40000000),
                                   bool(flags & 0x100)});
      }
    }
    std::string names(namesSize, '\0');
    if (!file.read(names.data(), names.size()))
      throw std::runtime_error("Truncated archive names.");
    std::size_t at = 0;
    for (const auto &[folder, entry] : pending) {
      const auto end = names.find('\0', at);
      if (end == std::string::npos)
        throw std::runtime_error("Invalid archive filename.");
      const auto key = Key(folder + "/" + names.substr(at, end - at));
      if (key.starts_with("meshes/") || key.starts_with("textures/"))
        entries[key].push_back(entry);
      at = end + 1;
    }
  }
  static std::vector<char> Contents(const Entry &entry) {
    std::ifstream file(*entry.archive, std::ios::binary);
    file.seekg(entry.offset);
    std::uint32_t size = entry.size;
    if (entry.prefix) {
      const auto length = Read<std::uint8_t>(file);
      if (size < length + 1u)
        throw std::runtime_error("Invalid archive prefix.");
      file.seekg(length, std::ios::cur);
      size -= length + 1;
    }
    if (size > 128 * 1024 * 1024)
      throw std::runtime_error("Asset too large to verify.");
    std::uint32_t unpacked = size;
    if (entry.compressed) {
      if (size < 4)
        throw std::runtime_error("Invalid compressed asset.");
      unpacked = Read<std::uint32_t>(file);
      size -= 4;
      if (unpacked > 128 * 1024 * 1024)
        throw std::runtime_error("Asset too large to verify.");
    }
    std::vector<char> bytes(size);
    if (!file.read(bytes.data(), size))
      throw std::runtime_error("Truncated archive asset.");
    if (!entry.compressed)
      return bytes;
    LZ4F_dctx *context = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION)))
      throw std::runtime_error("Cannot decode official asset.");
    struct Scope {
      LZ4F_dctx *ctx;
      ~Scope() { LZ4F_freeDecompressionContext(ctx); }
    } scope{context};
    std::vector<char> output(unpacked);
    std::size_t source = 0, destination = 0, remaining = 1;
    while (remaining) {
      auto in = bytes.size() - source, out = output.size() - destination;
      remaining = LZ4F_decompress(context, output.data() + destination, &out,
                                  bytes.data() + source, &in, nullptr);
      if (LZ4F_isError(remaining) || (!in && !out))
        throw std::runtime_error("Invalid compressed official asset.");
      source += in;
      destination += out;
    }
    if (destination != output.size() || source != bytes.size())
      throw std::runtime_error("Invalid official asset size.");
    return output;
  }

public:
  explicit OfficialAssets(const std::filesystem::path &data) {
    for (const auto &file : std::filesystem::directory_iterator(data)) {
      const auto utf8 = file.path().filename().u8string();
      const auto name =
          PackDocument::Lower(std::string(utf8.begin(), utf8.end()));
      const bool official =
          name.starts_with("skyrim - ") || name.starts_with("update - ") ||
          name.starts_with("dawnguard - ") ||
          name.starts_with("hearthfires - ") ||
          name.starts_with("dragonborn - ") || name == "update.bsa" ||
          name == "dawnguard.bsa" || name == "hearthfires.bsa" ||
          name == "dragonborn.bsa" || name == "_resourcepack.bsa" ||
          name == "ccbgssse001-fish.bsa" || name == "ccbgssse025-advdsgs.bsa" ||
          name == "ccbgssse037-curios.bsa" ||
          name == "ccqdrsse001-survivalmode.bsa";
      if (official && file.is_regular_file() && name.ends_with(".bsa"))
        Index(file.path());
    }
    if (entries.empty())
      throw std::runtime_error("Official game archives were not found; asset "
                               "sources cannot be verified.");
  }
  bool Contains(const std::string &path) const {
    return entries.contains(Key(path));
  }
  bool Matches(const std::string &path,
               const std::vector<char> &effective) const {
    const auto found = entries.find(Key(path));
    if (found == entries.end())
      return false;
    for (const auto &entry : found->second)
      if (Contents(entry) == effective)
        return true;
    return false;
  }
};
} // namespace SkyBuild::UI::PackAssets
