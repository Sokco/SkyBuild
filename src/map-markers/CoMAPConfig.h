#pragma once
#include <cctype>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace SkyBuild::MapMarkers {
// CoMAP's JsonCpp configuration format permits comments and trailing commas.
// Normalize those two extensions without changing any quoted strings.
inline nlohmann::json ParseCoMAPConfig(std::string_view text) {
  std::string clean;
  bool quoted{}, escaped{}, line{}, block{};
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (line) {
      if (c == '\n') {
        line = false;
        clean += '\n';
      }
      continue;
    }
    if (block) {
      if (c == '*' && next == '/') {
        block = false;
        ++i;
        clean += ' ';
      }
      continue;
    }
    if (!quoted && c == '/' && next == '/') {
      line = true;
      ++i;
      continue;
    }
    if (!quoted && c == '/' && next == '*') {
      block = true;
      ++i;
      continue;
    }
    clean += c;
    if (escaped)
      escaped = false;
    else if (quoted && c == '\\')
      escaped = true;
    else if (c == '"')
      quoted = !quoted;
  }
  if (block)
    throw std::runtime_error("Unterminated CoMAP config comment.");
  std::string normalized;
  quoted = escaped = false;
  for (std::size_t i = 0; i < clean.size(); ++i) {
    const char c = clean[i];
    if (!quoted && c == ',') {
      auto next = i + 1;
      while (next < clean.size() &&
             std::isspace(static_cast<unsigned char>(clean[next])))
        ++next;
      if (next < clean.size() && (clean[next] == '}' || clean[next] == ']'))
        continue;
    }
    normalized += c;
    if (escaped)
      escaped = false;
    else if (quoted && c == '\\')
      escaped = true;
    else if (c == '"')
      quoted = !quoted;
  }
  return nlohmann::json::parse(normalized);
}
} // namespace SkyBuild::MapMarkers
