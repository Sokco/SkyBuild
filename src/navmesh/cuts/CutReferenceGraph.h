#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace SkyBuild::Navmesh {
// Owned offline identities only. These are not native addresses or co-save IDs.
// A native adapter must enumerate every reference under a verified lifetime
// boundary before this model can authorize any engine mutation.
enum class CutReferenceKind { UndoInput, TriangleOwner, SavedTriangleOwner };
struct CutReference {
  std::uint64_t mesh{}, record{}, element{}, data{};
  CutReferenceKind kind{};
  bool operator==(const CutReference &) const = default;
};
struct CutReferenceGraph {
  std::set<std::uint64_t> data;
  std::vector<CutReference> references;
  std::map<std::uint64_t, std::set<std::uint64_t>> affectedInfos;
  bool complete{};
  bool operator==(const CutReferenceGraph &) const = default;
  // MSVC's map move constructor may allocate its sentinel. Exchange native
  // model buffers through container swap, never a whole-graph move constructor.
  friend void swap(CutReferenceGraph &a, CutReferenceGraph &b) noexcept {
    static_assert(noexcept(a.data.swap(b.data)));
    static_assert(noexcept(a.references.swap(b.references)));
    static_assert(noexcept(a.affectedInfos.swap(b.affectedInfos)));
    a.data.swap(b.data);
    a.references.swap(b.references);
    a.affectedInfos.swap(b.affectedInfos);
    std::swap(a.complete, b.complete);
  }
};
inline std::string ValidateCutReferenceGraph(const CutReferenceGraph &graph) {
  if (!graph.complete)
    return "Cut reference coverage is incomplete";
  if (graph.data.contains(0))
    return "Null cut identity";
  std::set<
      std::tuple<std::uint64_t, CutReferenceKind, std::uint64_t, std::uint64_t>>
      slots;
  for (const auto &ref : graph.references) {
    if (!ref.mesh || !ref.data || !graph.data.contains(ref.data))
      return "Unresolved cut reference";
    if (!slots.emplace(ref.mesh, ref.kind, ref.record, ref.element).second)
      return "Duplicate cut reference slot";
  }
  for (const auto &[data, infos] : graph.affectedInfos) {
    if (!graph.data.contains(data) || infos.contains(0))
      return "Unresolved affected-info membership";
  }
  for (const auto data : graph.data)
    if (!graph.affectedInfos.contains(data))
      return "Missing affected-info membership capture";
  return {};
}
// Alpha conversion of a COMPLETE detached graph. No borrowed slot addresses
// survive this operation. Many private clones may map to one retained identity;
// their membership sets must be unioned rather than overwritten.
inline bool RemapCutReferences(
    const CutReferenceGraph &source,
    const std::map<std::uint64_t, std::uint64_t> &mapping,
    CutReferenceGraph &destination, std::string &error,
    const std::map<std::uint64_t, std::uint64_t> &infoMapping = {}) {
  error = ValidateCutReferenceGraph(source);
  if (!error.empty())
    return false;
  for (const auto &[from, to] : mapping) {
    if (!source.data.contains(from) || !to) {
      error = "Invalid cut identity substitution";
      return false;
    }
  }
  for (const auto &[from, to] : infoMapping)
    if (!from || !to) {
      error = "Null navigation info substitution";
      return false;
    }
  const auto target = [&](std::uint64_t id) {
    const auto found = mapping.find(id);
    return found == mapping.end() ? id : found->second;
  };
  CutReferenceGraph staged;
  staged.complete = true;
  for (auto id : source.data)
    staged.data.insert(target(id));
  staged.references = source.references;
  for (auto &ref : staged.references)
    ref.data = target(ref.data);
  for (const auto &[id, infos] : source.affectedInfos) {
    auto &members = staged.affectedInfos[target(id)];
    for (auto info : infos) {
      const auto replacement = infoMapping.find(info);
      members.insert(replacement == infoMapping.end() ? info
                                                      : replacement->second);
    }
  }
  error = ValidateCutReferenceGraph(staged);
  if (!error.empty())
    return false;
  swap(destination, staged);
  return true;
}
} // namespace SkyBuild::Navmesh
