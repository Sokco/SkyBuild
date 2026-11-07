#pragma once
#include <cstdint>
#include <map>
#include <set>
namespace SkyBuild::Navmesh {
// Only classifies scheduler observations. This is not writer exclusion or
// permission to release memory; callers still validate native observation
// epochs.
inline bool CanInspectRetirementWork(std::uint32_t prepared,
                                     std::uint32_t dispatched) {
  return prepared == 0 && dispatched == 0;
}
struct RetirementEligibility {
  bool sealed{}, identityMatches{}, infoUnregistered{}, noLiveMembership{};
  bool noCutState{}, noDoorState{}, noUndoBorrowers{},
      noRegisteredCutMembership{};
  bool noDerivedWorkBorrowers{};
  std::uint32_t strongReferences{};
};
// Necessary ownership conditions only, NOT destruction authorization. Native
// destruction recycles IDs; stale ID consumers are not yet retired.
inline bool IsRetirementCandidate(const RetirementEligibility &v) {
  return v.sealed && v.identityMatches && v.infoUnregistered &&
         v.noLiveMembership && v.noCutState && v.noDoorState &&
         v.noUndoBorrowers && v.noRegisteredCutMembership &&
         v.noDerivedWorkBorrowers && v.strongReferences == 1;
}
// Retained geometry must not keep portal IDs into a reclaimed component.
// Source keys are opaque native tokens; IDs remain the portal lookup domain.
inline std::set<std::uint32_t> CloseRetirementCandidates(
    const std::map<std::uintptr_t, std::uint32_t> &eligible,
    const std::map<std::uintptr_t, std::set<std::uint32_t>> &portals) {
  std::set<std::uint32_t> result;
  for (const auto &[token, id] : eligible)
    result.insert(id);
  bool changed;
  do {
    changed = false;
    for (const auto &[source, targets] : portals) {
      const auto owner = eligible.find(source);
      if (owner != eligible.end() && result.contains(owner->second))
        continue;
      for (const auto target : targets)
        changed |= result.erase(target) != 0;
    }
  } while (changed);
  return result;
}
} // namespace SkyBuild::Navmesh
