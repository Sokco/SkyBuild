#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>
namespace SkyBuild::Navmesh {
// Producer must supply native discovery results against the replacement graph.
// An old affected-info list is not discovery, nor proof that a cut exists.
struct CutEpochInput {
  std::uint64_t identity{};
  std::set<std::uint32_t> oldMemberships, materializedCuts, newCandidates;
  bool operator==(const CutEpochInput &) const = default;
};
struct CutEpochPlan {
  std::map<std::uint64_t, CutEpochInput> inputs;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> retire, applyCandidates;
  std::set<std::uint32_t> requiredOldCohort;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
inline CutEpochPlan
PlanCutEpochs(std::span<const CutEpochInput> registered,
              const std::set<std::uint32_t> &oldCohort,
              const std::set<std::uint32_t> &replacementCohort,
              bool discoveryComplete) {
  CutEpochPlan plan;
  const auto reject = [&](const char *reason) {
    // No partial operation plan survives a coverage failure.
    plan.retire.clear();
    plan.applyCandidates.clear();
    plan.error = reason;
    return plan;
  };
  if (!discoveryComplete)
    return reject("Native input/candidate discovery is incomplete");
  if (registered.size() > 4096 || oldCohort.size() > 1024 ||
      replacementCohort.size() > 1024)
    return reject("Cut epoch cohort budget exceeded");
  for (const auto &input : registered) {
    if (!input.identity || input.oldMemberships.contains(0) ||
        input.materializedCuts.contains(0) || input.newCandidates.contains(0))
      return reject("Cut epoch input has an invalid identity");
    const auto [entry, inserted] = plan.inputs.emplace(input.identity, input);
    if (!inserted && entry->second != input)
      return reject("Canonical cut input has conflicting observations");
    for (const auto mesh : input.materializedCuts)
      if (!input.oldMemberships.contains(mesh))
        return reject("Materialized cut is absent from native memberships");
    plan.requiredOldCohort.insert(input.oldMemberships.begin(),
                                  input.oldMemberships.end());
    for (const auto mesh : input.newCandidates)
      if (!replacementCohort.contains(mesh))
        return reject("Native candidate escapes replacement cohort");
  }
  for (const auto mesh : plan.requiredOldCohort)
    if (!oldCohort.contains(mesh))
      return reject("Cut retirement requires an expanded old cohort");
  for (const auto &[identity, input] : plan.inputs) {
    for (const auto mesh : input.materializedCuts)
      plan.retire.emplace_back(identity, mesh);
    for (const auto mesh : input.newCandidates)
      plan.applyCandidates.emplace_back(identity, mesh);
  }
  return plan;
}
} // namespace SkyBuild::Navmesh
