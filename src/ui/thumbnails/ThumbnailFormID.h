#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace SkyBuild::UI {
// UI form IDs are hexadecimal, with an optional 0x prefix.
inline std::optional<std::uint32_t>
ParseThumbnailFormID(std::string_view value) {
  if (value.starts_with("0x") || value.starts_with("0X"))
    value.remove_prefix(2);
  std::uint32_t id = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), id, 16);
  if (error != std::errc{} || end != value.data() + value.size() || id == 0)
    return std::nullopt;
  return id;
}
} // namespace SkyBuild::UI
