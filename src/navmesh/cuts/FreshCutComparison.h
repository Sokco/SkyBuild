#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
#include <set>
namespace SkyBuild::Navmesh {
// Compare private rebuilds across fresh input allocation. Tokens may change;
// topology, lifecycle evidence and ownership relationships must not change.
// This is owned evidence comparison, never an authorization to publish.
inline bool FreshCutResultsMatch(
    const NativeCutSnapshot &before, const NativeCutSnapshot &after,
    const std::map<std::uint64_t, std::uint64_t> &inputRenaming) {
  std::set<std::uint64_t> destinations;
  for (const auto &[source, destination] : inputRenaming)
    if (!source || !destination || source == destination ||
        !destinations.insert(destination).second)
      return false;
  auto renamed = before;
  auto rename = [&](std::uint64_t &identity) {
    const auto found = inputRenaming.find(identity);
    if (found == inputRenaming.end())
      return false;
    identity = found->second;
    return true;
  };
  for (auto &[id, records] : renamed.records)
    for (auto &record : records)
      if (!rename(record.observedDataIdentity))
        return false;
  for (auto &[id, owners] : renamed.ownership)
    for (auto &owner : owners)
      if (!rename(owner.dataIdentity))
        return false;
  if (renamed != after)
    return false;
  // CutUndoRecord equality intentionally ignores observation-only tokens.
  // Compare them explicitly here so a wrong rebuilt owner cannot be accepted.
  for (const auto &[id, records] : renamed.records) {
    const auto &actual = after.records.at(id);
    for (std::size_t index = 0; index < records.size(); ++index)
      if (records[index].observedDataIdentity !=
              actual[index].observedDataIdentity ||
          records[index].observedMaterialization !=
              actual[index].observedMaterialization)
        return false;
  }
  return true;
}
} // namespace SkyBuild::Navmesh
