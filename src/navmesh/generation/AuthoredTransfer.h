#pragma once
#include "navmesh/generation/AuthoredBasisStore.h"
#include "navmesh/generation/AuthoredSourceCompatibility.h"
namespace SkyBuild::Navmesh {
// Ownership reconciliation follows native full-form transfers. Both retained
// and current forms may contain different cut epochs. Identity is established
// by the same pinned authored surface, not by reversing either epoch's shared
// undo objects. This does NOT validate live bindings, cut owners or cell lists;
// the native transaction checks those independently before changing journals.
inline std::string
ValidateAuthoredTransfer(const AuthoredBasisSnapshot &records,
                         const std::map<std::uint32_t, AuthoredMesh> &before,
                         const std::map<std::uint32_t, AuthoredMesh> &after) {
  if (!records.Complete() || before.empty() || before.size() != after.size())
    return "Authored transfer cohort is incomplete";
  for (const auto &[id, prior] : before) {
    const auto source = records.records.find(id);
    const auto current = after.find(id);
    if (source == records.records.end() || !source->second ||
        current == after.end())
      return "Authored transfer source identity is missing: " +
             std::to_string(id);
    for (const auto *view : {&prior, &current->second}) {
      const auto prefix =
          CompareAuthoredGeometryPrefix(source->second->mesh, *view);
      if (!prefix.Matches())
        return "Authored transfer surface " + std::to_string(id) + ": " +
               prefix.error;
    }
  }
  return {};
}
} // namespace SkyBuild::Navmesh
