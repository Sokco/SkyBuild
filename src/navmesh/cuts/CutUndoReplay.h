#pragma once
#include "navmesh/generation/AuthoredTopology.h"
#include "navmesh/cuts/CutLifecycle.h"
#include "navmesh/cuts/CutMaterialization.h"
namespace SkyBuild::Navmesh {
struct CutSavedEdges {
  std::uint16_t triangle{};
  std::array<bool, 3> external{};
  std::array<std::uint16_t, 3> edges{};
  bool operator==(const CutSavedEdges &) const = default;
};
struct CutFaceLink {
  std::uint32_t sourceMesh{}, targetMesh{};
  std::uint16_t sourceTriangle{}, targetTriangle{};
  unsigned sourceEdge{}, targetEdge{};
  bool operator==(const CutFaceLink &) const = default;
};
struct CutUndoRecord {
  std::uint32_t originalVertices{}, addedVertices{};
  std::uint32_t originalTriangles{}, addedTriangles{};
  std::vector<std::uint16_t> affectedTriangles;
  std::vector<CutSavedEdges> restoredEdges;
  std::vector<CutFaceLink> links;
  std::vector<std::uint16_t> auxiliaryIndices;
  bool hasSavedObstacleData{};
  // Shared record state is evidence, not this mesh's materialization.
  // Zero means an offline fixture supplied no raw observation.
  std::uint32_t rawStatus{};
  std::uint64_t observedDataIdentity{}; // Diagnostic token; never persisted.
  // Diagnostic projection; publication integration awaits complete binding
  // graph and native task-lifecycle validation.
  CutMaterialization observedMaterialization{CutMaterialization::Unresolved};
  std::string materializationReason;
  CutLifecycle lifecycle{CutLifecycle::Applied};
  [[nodiscard]] bool Applied() const {
    return lifecycle == CutLifecycle::Applied;
  }
  bool operator==(const CutUndoRecord &other) const {
    return originalVertices == other.originalVertices &&
           addedVertices == other.addedVertices &&
           originalTriangles == other.originalTriangles &&
           addedTriangles == other.addedTriangles &&
           affectedTriangles == other.affectedTriangles &&
           restoredEdges == other.restoredEdges && links == other.links &&
           auxiliaryIndices == other.auxiliaryIndices &&
           hasSavedObstacleData == other.hasSavedObstacleData &&
           rawStatus == other.rawStatus && lifecycle == other.lifecycle;
  }
};
struct NativeCutSnapshot {
  std::map<std::uint32_t, std::vector<CutUndoRecord>> records;
  std::map<std::uint32_t, std::vector<std::uint16_t>> auxiliaryIndices;
  std::map<std::uint32_t, std::size_t> obstacleEntries;
  // Capture-local tokens only, never persistent cache/save data.
  std::map<std::uint32_t, std::vector<CutOwnershipWitness>> ownership;
  bool operator==(const NativeCutSnapshot &) const = default;
};
struct CutUndoReplay {
  std::map<std::uint32_t, AuthoredMesh> meshes;
  std::size_t restoredCuts{}, restoredEdges{}, redirectedPortals{};
  std::size_t discardedUndoneCuts{};
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
// Detached research replay of the traced local restoration fields. Rejects
// saved obstacle data and inter-mesh restoration chains not yet characterized.
// This is not a native undo invocation, an ownership clone, or a publishable
// replacement: info bounds, cut objects and native maps are not constructed.
[[nodiscard]] CutUndoReplay ReplayCapturedCutUndo(
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const std::map<std::uint32_t, std::vector<CutUndoRecord>> &cuts,
    const std::map<std::uint32_t, AuthoredMesh> *validatedSessionBasis =
        nullptr);
} // namespace SkyBuild::Navmesh
