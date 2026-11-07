#pragma once
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/generation/GeneratedTopology.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace SkyBuild::Navmesh {
// Owned worker inputs only; overwrite one diagnostic instead of accumulating
// captures. This represents the complete batch after cached-cell assembly.
inline void
SaveTopologySnapshot(const Mesh &mesh, const std::vector<std::uint32_t> &owners,
                     const std::map<std::uint32_t, AuthoredMesh> &originals,
                     const std::set<std::uint32_t> &replaced,
                     const NativeDoorAnchors &anchors, std::string_view error,
                     const std::filesystem::path &path) {
  if (!Diagnostics::Enabled())
    return;
  using nlohmann::json;
  json data{{"schema", 1},
            {"error", error},
            {"owners", owners},
            {"replaced", replaced}};
  data["vertices"] = data["triangles"] = data["originals"] = data["anchors"] =
      json::array();
  for (const auto &p : mesh.vertices)
    data["vertices"].push_back({p.x, p.y, p.z});
  for (const auto &t : mesh.triangles)
    data["triangles"].push_back({{"vertices", t.vertices},
                                 {"neighbors", t.neighbors},
                                 {"flags", t.flags},
                                 {"sourceMesh", t.source.formID},
                                 {"sourceTriangle", t.source.triangle}});
  for (const auto &[id, m] : originals) {
    json item{{"id", id},
              {"nativeDependencies", m.nativeDependencies},
              {"coverTriangles", m.coverTriangles},
              {"doorTriangles", m.doorTriangles},
              {"closedDoorTriangles", m.closedDoorTriangles},
              {"vertices", json::array()},
              {"triangles", json::array()},
              {"portals", json::array()}};
    for (const auto &p : m.vertices)
      item["vertices"].push_back({p.x, p.y, p.z});
    for (const auto &t : m.triangles)
      item["triangles"].push_back({{"vertices", t.vertices},
                                   {"edges", t.edges},
                                   {"flags", t.flags},
                                   {"traversal", t.traversal}});
    for (const auto &p : m.portals)
      item["portals"].push_back(
          {p.type, p.targetMesh, p.targetTriangle, p.auxiliary});
    data["originals"].push_back(std::move(item));
  }
  for (const auto &[id, a] : anchors) {
    json item{
        {"id", id}, {"doors", json::array()}, {"closedDoors", json::array()}};
    auto append = [&](const auto &points, const char *key) {
      for (const auto &p : points)
        item[key].push_back(p ? json{p->x, p->y, p->z} : json(nullptr));
    };
    append(a.doors, "doors");
    append(a.closedDoors, "closedDoors");
    if (!a.unboundDoors.empty())
      item["unboundDoors"] = a.unboundDoors;
    if (!a.unboundClosedDoors.empty())
      item["unboundClosedDoors"] = a.unboundClosedDoors;
    data["anchors"].push_back(std::move(item));
  }
  std::ofstream stream(path, std::ios::trunc);
  stream << data.dump();
  stream.close();
  if (stream.fail())
    throw std::runtime_error("Could not save complete topology snapshot");
}
} // namespace SkyBuild::Navmesh
