#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
namespace SkyBuild::UI {
class PackZip {
  struct Entry {
    std::string name;
    std::uint32_t crc;
    std::uint64_t size, offset;
  };
  std::ofstream output;
  std::vector<Entry> entries;
  std::unordered_set<std::string> names;
  void Word(std::uint16_t value) {
    output.put(static_cast<char>(value));
    output.put(static_cast<char>(value >> 8));
  }
  void Dword(std::uint32_t value) {
    Word(static_cast<std::uint16_t>(value));
    Word(static_cast<std::uint16_t>(value >> 16));
  }
  void Qword(std::uint64_t value) {
    Dword(static_cast<std::uint32_t>(value));
    Dword(static_cast<std::uint32_t>(value >> 32));
  }
  std::uint64_t Position() {
    const auto at = output.tellp();
    if (at < 0)
      throw std::runtime_error("Archive write failed.");
    return static_cast<std::uint64_t>(at);
  }

public:
  explicit PackZip(const std::filesystem::path &path)
      : output(path, std::ios::binary | std::ios::trunc) {
    if (!output)
      throw std::runtime_error("Cannot create export archive.");
  }
  void Add(std::string name, std::uint64_t size,
           const std::function<bool(char *, std::uint32_t)> &read) {
    if (name.size() > 65535)
      throw std::runtime_error("Archive exceeds ZIP entry limits.");
    if (name.empty() || name.front() == '/' ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos ||
        name.find("../") != std::string::npos)
      throw std::runtime_error("Unsafe archive path.");
    if (!names.insert(name).second)
      throw std::runtime_error("Duplicate archive path: " + name);
    Entry entry{std::move(name), 0, size, Position()};
    const bool wide = size >= 0xFFFFFFFFULL;
    Dword(0x04034B50);
    Word(wide ? 45 : 20);
    Word(0x808);
    Word(0);
    Word(0);
    Word(0x21);
    Dword(0);
    Dword(wide ? 0xFFFFFFFFU : 0);
    Dword(wide ? 0xFFFFFFFFU : 0);
    Word(static_cast<std::uint16_t>(entry.name.size()));
    Word(wide ? 20 : 0);
    output << entry.name;
    if (wide) {
      Word(1);
      Word(16);
      Qword(size);
      Qword(size);
    }
    static const auto table = [] {
      std::array<std::uint32_t, 256> result{};
      for (std::uint32_t i = 0; i < 256; ++i) {
        auto crc = i;
        for (int j = 0; j < 8; ++j)
          crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        result[i] = crc;
      }
      return result;
    }();
    std::array<char, 65536> buffer{};
    std::uint32_t crc = 0xFFFFFFFFU;
    std::uint64_t remaining = size;
    while (remaining) {
      const auto count = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(remaining, buffer.size()));
      if (!read(buffer.data(), count))
        throw std::runtime_error("Cannot read " + entry.name);
      for (std::uint32_t i = 0; i < count; ++i)
        crc = table[(crc ^ static_cast<unsigned char>(buffer[i])) & 255] ^
              (crc >> 8);
      output.write(buffer.data(), count);
      remaining -= count;
    }
    entry.crc = crc ^ 0xFFFFFFFFU;
    Dword(0x08074B50);
    Dword(entry.crc);
    if (wide) {
      Qword(size);
      Qword(size);
    } else {
      Dword(static_cast<std::uint32_t>(size));
      Dword(static_cast<std::uint32_t>(size));
    }
    entries.push_back(std::move(entry));
    if (!output)
      throw std::runtime_error("Archive write failed.");
  }
  void Text(std::string name, std::string_view data) {
    std::size_t offset = 0;
    Add(std::move(name), static_cast<std::uint64_t>(data.size()),
        [&](char *buffer, std::uint32_t count) {
          std::memcpy(buffer, data.data() + offset, count);
          offset += count;
          return true;
        });
  }
  void File(std::string name, const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("Missing export file: " + path.string());
    const auto size = file.tellg();
    if (size < 0)
      throw std::runtime_error("Export file too large.");
    file.seekg(0);
    Add(std::move(name), static_cast<std::uint64_t>(size),
        [&](char *buffer, std::uint32_t count) {
          return static_cast<bool>(file.read(buffer, count));
        });
  }
  void Close() {
    const auto start = Position();
    for (const auto &entry : entries) {
      const bool wideSize = entry.size >= 0xFFFFFFFFULL,
                 wideOffset = entry.offset >= 0xFFFFFFFFULL;
      const auto extra = static_cast<std::uint16_t>(
          (wideSize || wideOffset ? 4 : 0) + (wideSize ? 16 : 0) +
          (wideOffset ? 8 : 0));
      Dword(0x02014B50);
      Word(45);
      Word(wideSize || wideOffset ? 45 : 20);
      Word(0x808);
      Word(0);
      Word(0);
      Word(0x21);
      Dword(entry.crc);
      Dword(wideSize ? 0xFFFFFFFFU : static_cast<std::uint32_t>(entry.size));
      Dword(wideSize ? 0xFFFFFFFFU : static_cast<std::uint32_t>(entry.size));
      Word(static_cast<std::uint16_t>(entry.name.size()));
      Word(extra);
      Word(0);
      Word(0);
      Word(0);
      Dword(0);
      Dword(wideOffset ? 0xFFFFFFFFU
                       : static_cast<std::uint32_t>(entry.offset));
      output << entry.name;
      if (extra) {
        Word(1);
        Word(extra - 4);
        if (wideSize) {
          Qword(entry.size);
          Qword(entry.size);
        }
        if (wideOffset)
          Qword(entry.offset);
      }
    }
    const auto size = Position() - start;
    if (entries.size() >= 65535 || size >= 0xFFFFFFFFULL ||
        start >= 0xFFFFFFFFULL) {
      const auto zip64 = Position();
      Dword(0x06064B50);
      Qword(44);
      Word(45);
      Word(45);
      Dword(0);
      Dword(0);
      Qword(entries.size());
      Qword(entries.size());
      Qword(size);
      Qword(start);
      Dword(0x07064B50);
      Dword(0);
      Qword(zip64);
      Dword(1);
    }
    Dword(0x06054B50);
    Word(0);
    Word(0);
    Word(static_cast<std::uint16_t>(
        std::min<std::size_t>(entries.size(), 65535)));
    Word(static_cast<std::uint16_t>(
        std::min<std::size_t>(entries.size(), 65535)));
    Dword(static_cast<std::uint32_t>(
        std::min<std::uint64_t>(size, 0xFFFFFFFFULL)));
    Dword(static_cast<std::uint32_t>(
        std::min<std::uint64_t>(start, 0xFFFFFFFFULL)));
    Word(0);
    output.close();
    if (!output)
      throw std::runtime_error("Could not finish export archive.");
  }
};
} // namespace SkyBuild::UI
