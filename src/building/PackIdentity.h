#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
namespace SkyBuild::Packs {
struct Id {
  std::array<std::uint8_t, 16> bytes{};
  bool operator==(const Id &) const = default;
  explicit operator bool() const { return *this != Id{}; }
  std::string String() const {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (auto byte : bytes) {
      result += hex[byte >> 4];
      result += hex[byte & 15];
    }
    return result;
  }
  static std::optional<Id> Parse(std::string_view text) {
    if (text.size() != 32)
      return {};
    Id result;
    const auto nibble = [](char c) {
      return c >= '0' && c <= '9'   ? c - '0'
             : c >= 'a' && c <= 'f' ? c - 'a' + 10
             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                    : -1;
    };
    for (unsigned i = 0; i < 16; ++i) {
      const int a = nibble(text[i * 2]), b = nibble(text[i * 2 + 1]);
      if (a < 0 || b < 0)
        return {};
      result.bytes[i] = static_cast<std::uint8_t>(a * 16 + b);
    }
    return result ? std::optional{result} : std::nullopt;
  }
  static Id New() {
    std::random_device random;
    Id result;
    for (unsigned i = 0; i < 16; i += 4) {
      auto value = random();
      for (unsigned j = 0; j < 4; ++j) {
        result.bytes[i + j] = static_cast<std::uint8_t>(value);
        value >>= 8;
      }
    }
    result.bytes[6] = (result.bytes[6] & 15) | 64;
    result.bytes[8] = (result.bytes[8] & 63) | 128;
    return result;
  }
};
struct Identity {
  Id pack, item;
  bool operator==(const Identity &) const = default;
  explicit operator bool() const { return bool(pack) && bool(item); }
};
static_assert(sizeof(Identity) == 32);
} // namespace SkyBuild::Packs
