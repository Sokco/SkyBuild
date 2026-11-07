#pragma once
#include "navmesh/cuts/CutReferenceGraph.h"
#include "navmesh/cuts/CutUndoReplay.h"
#include "navmesh/cuts/NativeCutData.h"
#include "navmesh/generation/AuthoredBasisStore.h"
#include "navmesh/generation/MeshPartition.h"
#include "navmesh/native/NativeFormDraft.h"
#include <algorithm>
#include <set>
namespace SkyBuild::Navmesh {
class NativePublication;
// Entirely detached graph: FF factory forms, remapped portal IDs, retained door
// inputs, exact native geometry and connected info populated through scoped
// private resolution, remapped native cuts and compressed bounds. Publication
// remains a separate transaction after preparation has passed.
class NativeGraphDraft {
public:
  [[nodiscard]] static std::unique_ptr<NativeGraphDraft>
  Create(const PartitionedMesh &graph,
         const RE::BSTSmartPointer<RE::BSPathingCell> &fallback,
         const std::map<std::uint32_t, AuthoredMesh> &originals,
         const NativeCutSnapshot &originalCuts,
         const AuthoredBasisSnapshot &recordLease, std::string &error);
  [[nodiscard]] std::string ValidationError(bool published = false) const;
  [[nodiscard]] std::vector<RE::FormID> IDs() const;
  [[nodiscard]] const std::set<RE::FormID> &DeferredTargets() const {
    return deferredTargets;
  }
  [[nodiscard]] std::size_t MeshCount() const { return forms.size(); }
  [[nodiscard]] bool HasDeferredBoundary(std::uint32_t a,
                                         std::uint32_t b) const {
    return deferredBoundaries.contains(std::minmax(a, b));
  }

private:
  friend class NativePublication;
  void ReleaseRetiredValidation();
  std::set<RE::FormID> deferredTargets;
  // Authored/cohort IDs only; no pointers and no speculative neighbor links.
  std::set<std::pair<std::uint32_t, std::uint32_t>> deferredBoundaries;
  std::vector<NativeCutDataSource> sourceCutData;
  // Journal retirement does not prove that older native work released these
  // inputs' bodies/groups. Keep their complete ownership with the graph.
  std::vector<NativeCutDataSource> retiredCutData;
  std::map<std::uint32_t, AuthoredMesh> originalTopology;
  NativeCutSnapshot originalCuts;
  AuthoredBasisSnapshot recordLease;
  void MaterializeCuts();
  NativeCutSnapshot expectedCuts;
  std::string freshCutPhase;
  std::string discoveryEvidence;
  std::string cutOperationEvidence;
  std::map<std::uint32_t, std::unique_ptr<NativeFormDraft>> forms;
  std::map<std::uint32_t, AuthoredMesh> expected;
  std::map<std::uint32_t, RE::BSTArray<RE::BSNavmeshTriangleDoorPortal>>
      expectedDoors;
  std::map<std::uint32_t, RE::BSTArray<RE::BSNavmeshClosedDoorInfo>>
      expectedClosedDoors;
  // Borrowed info pointers in these clones must release before the forms.
  std::vector<std::unique_ptr<NativeCutDataDraft>> cutInputs;
};
} // namespace SkyBuild::Navmesh
