#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace SkyBuild::Navmesh {
// Native infos contain borrowed mesh pointers. Before removing native-created
// successors from a cell list, acquire actual ownership; a retired info's
// association alone cannot keep that allocation alive for cached borrowers.
template <class Forms>
void RetainPublicationDepartures(Forms &retained, const Forms &live,
                                 const Forms &published) {
  for (const auto &form : live)
    if (std::find(published.begin(), published.end(), form) ==
            published.end() &&
        std::find(retained.begin(), retained.end(), form) == retained.end())
      retained.push_back(form);
}
// Retired storage may outlive publication. It must never be admitted as live
// navigation merely because its allocation still belongs to this plugin.
inline bool MatchesDormantRetiredForm(const void *form, const void *lookup,
                                      const void *binding,
                                      const void *registeredInfo,
                                      bool retainedByRetiredGraph,
                                      bool ownedByCurrentGraph, bool sameCell) {
  return form && lookup == form && !binding && !registeredInfo &&
         retainedByRetiredGraph && !ownedByCurrentGraph && sameCell;
}
// The caller must independently prove retained ID, cell and journal ownership.
// A retained original can have its reciprocal info binding, or be unbound while
// native form transfer is completed. A binding to any other info is invalid.
template <class Mesh, class Info>
[[nodiscard]] bool
MatchesRetainedOriginalBinding(const Mesh *mesh, const Info *info,
                               const Mesh *infoOwner, const Info *binding) {
  return mesh && info && infoOwner == mesh && (!binding || binding == info);
}
// IDs alone do not prove a survived publication: the live registry and both
// directions of the form/info binding must still identify this owned cohort.
template <class Mesh, class Info>
bool MatchesPublicationIdentity(std::uint32_t expectedID, std::uint32_t formID,
                                std::uint32_t infoID, const Mesh *form,
                                const Mesh *infoMesh, const Info *ownedInfo,
                                const Info *registryInfo,
                                const Info *formInfo) {
  return expectedID && form && ownedInfo && expectedID == formID &&
         expectedID == infoID && form == infoMesh &&
         ownedInfo == registryInfo && ownedInfo == formInfo;
}
// Live cell lists are collections, not index-addressed route or triangle data.
// Accept reordering only: duplicate counts and pointer identities must match.
template <class Array>
bool SamePublicationMembership(const Array &actual, const Array &expected) {
  return actual.size() == expected.size() &&
         std::is_permutation(actual.begin(), actual.end(), expected.begin(),
                             expected.end());
}
// Validate a native form transfer against the complete prior membership.
// Reordering is allowed; missing, foreign and duplicated forms are rejected.
template <class Array, class Map, class Pointer>
bool MatchesTransferredPublication(const Array &actual, const Array &expected,
                                   const Map &transfers, Pointer pointer) {
  using Value = typename Map::key_type;
  std::vector<Value> live, planned;
  for (const auto &form : actual)
    live.push_back(pointer(form));
  for (const auto &form : expected) {
    const auto prior = pointer(form);
    const auto replacement = transfers.find(prior);
    planned.push_back(replacement == transfers.end() ? prior
                                                     : replacement->second);
  }
  return SamePublicationMembership(live, planned);
}
template <class Set>
bool MatchesPublicationWithDormantOriginals(const Set &actual,
                                            const Set &published,
                                            const Set &originals) {
  Set filtered;
  Set seen;
  for (const auto &value : actual) {
    if (std::find(originals.begin(), originals.end(), value) !=
        originals.end()) {
      if (std::find(seen.begin(), seen.end(), value) != seen.end())
        return false;
      seen.push_back(value);
    } else
      filtered.push_back(value);
  }
  return SamePublicationMembership(filtered, published);
}
template <class Set>
Set PublicationObsoleteInfos(const Set &previous, const Set &departing,
                             const Set &arriving) {
  Set result = previous;
  result.insert(departing.begin(), departing.end());
  for (const auto &info : arriving)
    result.erase(info);
  return result;
}
// Retain only transaction-owned changes; unchanged world entries do not
// belong in the publication/rollback journal.
template <class Original, class Replacement, class Journal>
void BuildPublicationJournal(const Original &original,
                             const Replacement &replacement, Journal &removed,
                             Journal &added) {
  removed.clear();
  added.clear();
  for (const auto &entry : original) {
    const auto next = replacement.find(entry.first);
    if (next == replacement.end() || next->second != entry.second)
      removed.emplace(entry.first, entry.second);
  }
  for (const auto &entry : replacement) {
    const auto prior = original.find(entry.first);
    if (prior == original.end() || prior->second != entry.second)
      added.emplace(entry.first, entry.second);
  }
}
// Engine scatter-table range copies first count by traversing the sparse
// source. Its size is already known; reserve it directly and visit once.
// The destination uses its normal allocator and typed element construction.
template <class Map> void CopyPublicationMap(Map &output, const Map &source) {
  if (std::addressof(output) == std::addressof(source))
    return;
  output.clear();
  if constexpr (requires { output.reserve(source.size()); })
    output.reserve(source.size());
  for (const auto &entry : source)
    output.emplace(entry.first, entry.second);
}
template <class Map, class Journal>
void RestorePublicationEntries(Map &output, const Map &live,
                               const Journal &original,
                               const Journal &published) {
  CopyPublicationMap(output, live);
  for (const auto &entry : published) {
    const auto prior = original.find(entry.first);
    if (prior == original.end() || prior->second != entry.second)
      output.erase(entry.first);
  }
  for (const auto &entry : original) {
    const auto replacement = published.find(entry.first);
    if (replacement == published.end() || replacement->second != entry.second)
      output.emplace(entry.first, entry.second);
  }
}
} // namespace SkyBuild::Navmesh
