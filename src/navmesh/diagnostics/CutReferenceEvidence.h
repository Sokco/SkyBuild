#pragma once
#include "navmesh/cuts/CutReferenceGraph.h"
#include <format>
#include <nlohmann/json.hpp>
namespace SkyBuild::Navmesh {
// Owned reference evidence shared with offline transaction replay. Pointer
// tokens are opaque diagnostics, never persistent navigation identifiers.
inline nlohmann::json EncodeCutReferenceEvidence(
    const CutReferenceGraph &graph,
    const std::map<std::uint64_t, std::uint64_t> &mapping,
    const std::map<std::uint64_t, std::uint64_t> &infoMapping = {}) {
  using Json = nlohmann::json;
  Json result{{"schema", 1},
              {"kind", "cut-reference-plan"},
              {"nativePublicationAuthorized", false},
              {"coverageErrors", Json::array()},
              {"referenceSlots", graph.references.size()},
              {"originalReferences", Json::array()},
              {"originalMemberships", Json::object()},
              {"dataSubstitutions", Json::object()},
              {"infoSubstitutions", Json::object()},
              {"convertedReferences", Json::array()},
              {"convertedMemberships", Json::object()},
              {"nativeWriterExclusion", false}};
  const auto key = [](std::uint64_t id) { return std::format("0x{:x}", id); };
  const auto references = [](const CutReferenceGraph &source) {
    auto refs = Json::array();
    for (const auto &ref : source.references) {
      const char *kind = ref.kind == CutReferenceKind::UndoInput ? "undo"
                         : ref.kind == CutReferenceKind::TriangleOwner
                             ? "current-owner"
                             : "saved-owner";
      refs.push_back({ref.mesh, kind, ref.record, ref.element, ref.data});
    }
    return refs;
  };
  result["originalReferences"] = references(graph);
  for (const auto &[id, infos] : graph.affectedInfos)
    result["originalMemberships"][key(id)] = infos;
  for (const auto &[from, to] : mapping)
    result["dataSubstitutions"][key(from)] = key(to);
  for (const auto &[from, to] : infoMapping)
    result["infoSubstitutions"][key(from)] = key(to);
  CutReferenceGraph converted;
  std::string error;
  if (!RemapCutReferences(graph, mapping, converted, error, infoMapping))
    result["coverageErrors"].push_back(error);
  else {
    result["convertedReferences"] = references(converted);
    for (const auto &[id, infos] : converted.affectedInfos)
      result["convertedMemberships"][key(id)] = infos;
  }
  return result;
}
} // namespace SkyBuild::Navmesh
