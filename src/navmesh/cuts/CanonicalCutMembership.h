#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace SkyBuild::Navmesh {
// Native discovery may reorder memberships without changing their identity.
// Reject duplicates rather than silently collapsing corrupt tracking lists.
template <class A, class B>
inline bool SameUniqueCutMembership(const A &a, const B &b) {
  if (a.size() != b.size())
    return false;
  using Token = typename A::value_type;
  std::set<Token> left(a.begin(), a.end()), right(b.begin(), b.end());
  return left.size() == a.size() && right.size() == b.size() && left == right;
}
using CutMemberships = std::map<std::uint64_t, std::set<std::uint64_t>>;
// An empty list alone may be a native transfer in progress. Retirement is
// authorized only after registry removal and closure of both retained graphs.
inline bool CanRetireCutInput(bool registered, bool referencedByLive,
                              bool referencedByOriginals,
                              bool membershipEmpty) {
  return !registered && !referencedByLive && !referencedByOriginals &&
         membershipEmpty;
}
// Journal detachment is not native reclamation. Removed inputs can still own
// live undo/saved-owner slots until Skyrim finishes removing their effects.
// Keep those inputs retained, but do not invent a load-transfer successor or
// restore memberships for an input absent from the original graph.
inline bool CanDetachCutJournal(bool registered, bool referencedByOriginals,
                                bool membershipEmpty) {
  return !registered && !referencedByOriginals && membershipEmpty;
}
// A runtime-only input may move after publication. Its registered identity and
// exact membership observation establish the new journal baseline; originals
// with retained triangle-indexed cut data still require the old strict check.
inline bool CanRefreshRuntimeCut(bool registered, bool referencedByOriginals,
                                 bool sameMembership) {
  return registered && !referencedByOriginals && sameMembership;
}
// An input first observed after publication has no before-image in the
// original cohort. Restore only its outside memberships, preserving native
// order. Never invent original memberships by translating generated infos.
inline bool
PrepareNewCutMembership(const std::vector<std::uint64_t> &current,
                        const std::set<std::uint64_t> &publishedCohort,
                        std::vector<std::uint64_t> &restored,
                        std::string &error) {
  error.clear();
  std::set<std::uint64_t> unique;
  std::vector<std::uint64_t> staged;
  staged.reserve(current.size());
  for (const auto info : current) {
    if (!info || !unique.insert(info).second) {
      error = "New native cut has null or duplicate navigation membership";
      return false;
    }
    if (!publishedCohort.contains(info))
      staged.push_back(info);
  }
  restored.swap(staged);
  return true;
}
// Owned tokens only. Native30476 rebuilds affectedInfos from DISCOVERY results,
// including candidates which materialize no undo record. Stable canonical
// inputs keep their out-of-cohort memberships; old cohort entries are replaced,
// never unioned with a previous generation's obsolete candidate set.
// This plans values; it supplies neither native slot addresses nor a boundary.
inline bool PrepareCanonicalCutMemberships(
    const CutMemberships &registered, const std::set<std::uint64_t> &oldCohort,
    const std::set<std::uint64_t> &newCohort,
    const CutMemberships &discoveredCandidates, CutMemberships &destination,
    std::string &error) {
  error.clear();
  if (oldCohort.contains(0) || newCohort.contains(0)) {
    error = "Null navigation info in cut membership cohort";
    return false;
  }
  for (const auto &[input, members] : registered) {
    if (!input || members.contains(0)) {
      error = "Invalid registered cut membership";
      return false;
    }
    const bool affected =
        std::any_of(members.begin(), members.end(),
                    [&](const auto info) { return oldCohort.contains(info); });
    if (affected && !discoveredCandidates.contains(input)) {
      error = "Missing rediscovery for a previously affected cutting input";
      return false;
    }
  }
  for (const auto &[input, members] : discoveredCandidates) {
    if (!registered.contains(input) ||
        !std::includes(newCohort.begin(), newCohort.end(), members.begin(),
                       members.end())) {
      error =
          "Cut discovery contains an unregistered input or out-of-cohort info";
      return false;
    }
  }
  auto staged = registered;
  for (const auto &[input, members] : discoveredCandidates) {
    auto &replacement = staged.at(input);
    for (const auto info : oldCohort)
      replacement.erase(info);
    replacement.insert(members.begin(), members.end());
  }
  destination.swap(staged);
  return true;
}
} // namespace SkyBuild::Navmesh
