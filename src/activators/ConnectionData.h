#pragma once
#include "naming/ReferenceNameData.h"
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace SkyBuild::Activators {
inline constexpr std::uint32_t SaveRecord = 0x4B4E4C41; // ALNK
inline constexpr std::size_t MaxConnections = 4096,
                             MaxRecordBytes = 4 * 1024 * 1024;
using Naming::CleanObjectName;

enum class Target : unsigned { Door, Container, Usable };
enum class Action : unsigned {
  OpenDoor,
  CloseDoor,
  ToggleDoor,
  LockDoor,
  UnlockDoor,
  ToggleLock,
  OpenContainer,
  Deposit,
  Retrieve,
  Activate
};
enum class Filter : unsigned {
  All,
  Ingredients,
  Food,
  Potions,
  Poisons,
  SmithingMaterials,
  Weapons,
  Armor,
  Ammunition,
  Books,
  SoulGems,
  Valuables,
  SpecificItem
};
struct Connection {
  std::uint32_t source{}, target{};
  Target kind{Target::Door};
  Action action{Action::ToggleDoor};
  Filter filter{Filter::All};
  std::uint32_t item{};
  int limit{}; // Zero transfers all matching items; otherwise a total item cap.
  bool enabled{true};
  bool operator==(const Connection &) const = default;
};
inline bool SupportsAction(Target kind, Action action) {
  if (action == Action::Activate)
    return unsigned(kind) <= unsigned(Target::Usable);
  if (kind == Target::Door)
    return action <= Action::ToggleLock;
  if (kind == Target::Container)
    return action >= Action::OpenContainer && action <= Action::Retrieve;
  return false;
}
inline bool UsesStorageFilter(Action action) {
  return action == Action::Deposit || action == Action::Retrieve;
}
inline bool Valid(const Connection &c) {
  return c.source && c.target && c.source != c.target &&
         unsigned(c.kind) <= unsigned(Target::Usable) &&
         unsigned(c.action) <= unsigned(Action::Activate) &&
         unsigned(c.filter) <= unsigned(Filter::SpecificItem) &&
         SupportsAction(c.kind, c.action) &&
         (!UsesStorageFilter(c.action) || c.filter != Filter::SpecificItem ||
          c.item) &&
         c.limit >= 0 && c.limit <= 100000;
}
inline std::uint64_t ConnectionKey(const Connection &c) {
  return (std::uint64_t(c.source) << 32) | c.target;
}
// Follow reachable edges without repeatedly scanning the complete list.
inline bool CreatesCycle(const std::vector<Connection> &connections,
                         const Connection &candidate) {
  if (!candidate.enabled)
    return false;
  std::map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
  for (const auto &c : connections)
    if (c.enabled && ConnectionKey(c) != ConnectionKey(candidate))
      outgoing[c.source].push_back(c.target);
  std::set<std::uint32_t> visited;
  std::vector<std::uint32_t> pending{candidate.target};
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (id == candidate.source)
      return true;
    if (!visited.insert(id).second)
      continue;
    if (const auto found = outgoing.find(id); found != outgoing.end())
      pending.insert(pending.end(), found->second.begin(), found->second.end());
  }
  return false;
}
inline bool HasActivationCycle(const std::vector<Connection> &connections) {
  std::map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
  std::map<std::uint32_t, std::size_t> incoming;
  for (const auto &c : connections)
    if (c.enabled) {
      outgoing[c.source].push_back(c.target);
      incoming.try_emplace(c.source, 0);
      ++incoming[c.target];
    }
  std::vector<std::uint32_t> pending;
  for (const auto &[id, count] : incoming)
    if (!count)
      pending.push_back(id);
  std::size_t processed = 0;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    ++processed;
    if (const auto found = outgoing.find(id); found != outgoing.end())
      for (const auto target : found->second)
        if (!--incoming[target])
          pending.push_back(target);
  }
  return processed != incoming.size();
}
inline nlohmann::json Encode(const std::vector<Connection> &connections) {
  auto data = nlohmann::json::array();
  for (const auto &c : connections)
    data.push_back({{"source", c.source},
                    {"target", c.target},
                    {"kind", unsigned(c.kind)},
                    {"action", unsigned(c.action)},
                    {"filter", unsigned(c.filter)},
                    {"item", c.item},
                    {"limit", c.limit},
                    {"enabled", c.enabled}});
  return data;
}
inline std::vector<Connection> Decode(const nlohmann::json &data) {
  if (!data.is_array() || data.size() > MaxConnections)
    throw std::runtime_error("Invalid activator connection list");
  std::vector<Connection> result;
  std::set<std::uint64_t> connections;
  const auto id = [](const nlohmann::json &value) {
    if (!value.is_number_integer() || value < 0 || value > UINT32_MAX)
      throw std::runtime_error("Invalid activator connection form ID");
    return value.get<std::uint32_t>();
  };
  for (const auto &entry : data) {
    Connection c;
    c.source = id(entry.at("source"));
    c.target = id(entry.at("target"));
    c.kind = static_cast<Target>(id(entry.at("kind")));
    c.action = static_cast<Action>(id(entry.at("action")));
    c.filter = static_cast<Filter>(id(entry.at("filter")));
    c.item = id(entry.at("item"));
    const auto limit = id(entry.at("limit"));
    if (limit > 100000)
      throw std::runtime_error("Invalid connection transfer limit");
    c.limit = static_cast<int>(limit);
    c.enabled = entry.at("enabled").get<bool>();
    if (!Valid(c) || !connections.insert(ConnectionKey(c)).second)
      throw std::runtime_error("Invalid or duplicate activator connection");
    result.push_back(c);
  }
  if (HasActivationCycle(result))
    throw std::runtime_error("Activation connections contain a cycle.");
  return result;
}
} // namespace SkyBuild::Activators
