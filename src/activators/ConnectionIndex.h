#pragma once
#include "activators/ConnectionData.h"
#include <span>
#include <unordered_map>
#include <utility>

namespace SkyBuild::Activators {
// Publish data and its index together so event readers always see one version.
struct ConnectionSnapshot {
  std::vector<Connection> connections;
  explicit ConnectionSnapshot(std::vector<Connection> values = {})
      : connections(std::move(values)) {
    for (std::size_t i = 0; i < connections.size(); ++i)
      if (connections[i].enabled)
        outgoing[connections[i].source].push_back(i);
  }
  std::span<const std::size_t> Outgoing(std::uint32_t source) const {
    const auto found = outgoing.find(source);
    return found == outgoing.end()
               ? std::span<const std::size_t>{}
               : std::span<const std::size_t>{found->second};
  }

private:
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> outgoing;
};
} // namespace SkyBuild::Activators
