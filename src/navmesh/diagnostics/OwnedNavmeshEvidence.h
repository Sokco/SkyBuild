#pragma once
#include "navmesh/cuts/CutUndoReplay.h"
#include <nlohmann/json.hpp>
namespace SkyBuild::Navmesh {
// Shared owned-value encoding for runtime failures and durable private native
// checkpoints. These tokens are diagnostic observations, never save data.
inline nlohmann::json
EncodeAuthoredEvidence(const std::map<std::uint32_t, AuthoredMesh> &topology) {
  auto authored = nlohmann::json::array();
  for (const auto &[id, mesh] : topology) {
    nlohmann::json entry{{"id", id},
                         {"nativeDependencies", mesh.nativeDependencies},
                         {"coverTriangles", mesh.coverTriangles},
                         {"doorTriangles", mesh.doorTriangles},
                         {"closedDoorTriangles", mesh.closedDoorTriangles}};
    entry["vertices"] = nlohmann::json::array();
    entry["triangles"] = nlohmann::json::array();
    entry["portals"] = nlohmann::json::array();
    for (const auto &vertex : mesh.vertices)
      entry["vertices"].push_back({vertex.x, vertex.y, vertex.z});
    for (const auto &triangle : mesh.triangles)
      entry["triangles"].push_back({{"vertices", triangle.vertices},
                                    {"edges", triangle.edges},
                                    {"flags", triangle.flags},
                                    {"traversal", triangle.traversal}});
    for (const auto &portal : mesh.portals)
      entry["portals"].push_back({portal.type, portal.targetMesh,
                                  portal.targetTriangle, portal.auxiliary});
    authored.push_back(std::move(entry));
  }
  return authored;
}
inline nlohmann::json EncodeCutEvidence(const NativeCutSnapshot &cuts) {
  auto cutEntries = nlohmann::json::array();
  for (const auto &[id, records] : cuts.records) {
    nlohmann::json entry{{"id", id},
                         {"auxiliaryIndices", cuts.auxiliaryIndices.at(id)},
                         {"obstacleEntries", cuts.obstacleEntries.at(id)}};
    entry["undo"] = nlohmann::json::array();
    entry["ownership"] = nlohmann::json::array();
    if (const auto found = cuts.ownership.find(id);
        found != cuts.ownership.end())
      for (const auto &witness : found->second)
        entry["ownership"].push_back({witness.triangle, witness.dataIdentity});
    for (const auto &record : records) {
      nlohmann::json decoded{
          {"lifecycle", static_cast<std::uint32_t>(record.lifecycle)},
          {"rawStatus", record.rawStatus},
          {"observedDataIdentity", record.observedDataIdentity},
          {"observedMaterialization",
           static_cast<unsigned>(record.observedMaterialization)},
          {"materializationReason", record.materializationReason},
          {"originalVertices", record.originalVertices},
          {"addedVertices", record.addedVertices},
          {"originalTriangles", record.originalTriangles},
          {"addedTriangles", record.addedTriangles},
          {"affectedTriangles", record.affectedTriangles},
          {"auxiliaryIndices", record.auxiliaryIndices},
          {"hasSavedObstacleData", record.hasSavedObstacleData}};
      decoded["restoredEdges"] = nlohmann::json::array();
      decoded["links"] = nlohmann::json::array();
      for (const auto &edge : record.restoredEdges)
        decoded["restoredEdges"].push_back({{"triangle", edge.triangle},
                                            {"external", edge.external},
                                            {"edges", edge.edges}});
      for (const auto &link : record.links)
        decoded["links"].push_back({{"sourceMesh", link.sourceMesh},
                                    {"targetMesh", link.targetMesh},
                                    {"sourceTriangle", link.sourceTriangle},
                                    {"targetTriangle", link.targetTriangle},
                                    {"sourceEdge", link.sourceEdge},
                                    {"targetEdge", link.targetEdge}});
      entry["undo"].push_back(std::move(decoded));
    }
    cutEntries.push_back(std::move(entry));
  }
  return cutEntries;
}
} // namespace SkyBuild::Navmesh
