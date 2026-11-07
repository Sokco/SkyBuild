#include "navmesh/cuts/CutPortalRepair.h"
#include <optional>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <tuple>
namespace SkyBuild::Navmesh {
namespace {
using Edge = std::tuple<std::uint32_t, std::uint16_t, unsigned>;
bool Active(const AuthoredTriangle &face) { return !(face.flags & (8U | 32U)); }
} // namespace
CutPortalRepair RepairCutPortals(
    const std::map<std::uint32_t, AuthoredMesh> &before,
    const std::map<std::uint32_t, AuthoredMesh> &cut,
    const std::map<std::uint32_t, std::vector<CutUndoRecord>> &records) {
  CutPortalRepair result;
  result.meshes = cut;
  auto fail = [&](std::string reason) {
    result.error = std::move(reason);
    return result;
  };
  const auto originalAudit = AuditAuthoredTopology(before);
  if (!originalAudit.Valid() || originalAudit.unloadedTargets ||
      before.size() != cut.size())
    return fail("Invalid cut repair input graph");
  std::map<Edge, std::set<Edge>> replacements;
  for (const auto &[owner, stack] : records) {
    if (!cut.contains(owner))
      return fail("Unknown native cut owner");
    for (const auto &record : stack)
      if (record.Applied())
        for (const auto &link : record.links) {
          if (!cut.contains(link.sourceMesh) ||
              !cut.contains(link.targetMesh) || link.sourceEdge >= 3 ||
              link.targetEdge >= 3 ||
              link.sourceTriangle >= cut.at(link.sourceMesh).triangles.size() ||
              link.targetTriangle >= cut.at(link.targetMesh).triangles.size())
            return fail("Invalid native cut replacement edge");
          const Edge source{link.sourceMesh, link.sourceTriangle,
                            link.sourceEdge};
          const Edge target{link.targetMesh, link.targetTriangle,
                            link.targetEdge};
          if (source == target)
            continue;
          replacements[source].insert(target);
        }
  }
  auto resolve = [&](Edge original) {
    std::set<Edge> leaves, path;
    std::function<void(Edge)> visit = [&](Edge edge) {
      if (!path.insert(edge).second)
        throw std::runtime_error("Cyclic native cut replacement edges");
      if (const auto next = replacements.find(edge);
          next != replacements.end()) {
        for (const auto &target : next->second)
          visit(target);
      } else {
        const auto &[id, triangle, side] = edge;
        if (!cut.contains(id) || triangle >= cut.at(id).triangles.size())
          throw std::runtime_error("Missing cut repair face");
        const auto &face = cut.at(id).triangles[triangle];
        if (Active(face) && (edge != original || (face.flags & (1U << side)))) {
          if (!(face.flags & (1U << side)) && face.edges[side] != 0xFFFF)
            throw std::runtime_error(
                "Cut replacement edge already has local connectivity");
          leaves.insert(edge);
        }
      }
      path.erase(edge);
    };
    visit(original);
    return leaves;
  };
  // Explicit cut lineage limits candidates to one authored portal. Project its
  // surviving segments onto the original edge's dominant world axis: native
  // portal endpoints can differ slightly in height and extent across cells.
  // No proximity search or arbitrary choice between overlapping returns.
  const auto interval = [&](const Edge &edge, unsigned axis,
                            const auto &graph) {
    const auto &[id, triangle, side] = edge;
    const auto &mesh = graph.at(id);
    const auto &face = mesh.triangles.at(triangle);
    const auto coordinate = [axis](const Point &p) {
      return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
    };
    const auto a = coordinate(mesh.vertices.at(face.vertices[side]));
    const auto b = coordinate(mesh.vertices.at(face.vertices[(side + 1) % 3]));
    return std::pair{std::min(a, b), std::max(a, b)};
  };
  std::map<Edge, std::optional<std::pair<std::uint32_t, std::uint16_t>>>
      assignments;
  std::set<Edge> accounted;
  std::map<Edge, AuthoredPortal> prototypes;
  std::size_t nativeClosedReturns{}, directedSplits{};
  try {
    for (const auto &[id, mesh] : before) {
      if (!cut.contains(id))
        return fail("Cut repair dropped a mesh");
      for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        const auto &face = mesh.triangles[index];
        if (!Active(face))
          continue;
        for (unsigned side = 0; side < 3; ++side) {
          if (!(face.flags & (1U << side)))
            continue;
          const Edge original{id, static_cast<std::uint16_t>(index), side};
          const auto sources = resolve(original);
          const auto &portal = mesh.portals.at(face.edges[side]);
          if (portal.type) {
            if (sources.size() != 1 || *sources.begin() != original ||
                !Active(cut.at(portal.targetMesh)
                            .triangles.at(portal.targetTriangle)))
              return fail("Native cut modified a special connection");
            const auto &current = cut.at(id).triangles.at(index);
            if (!(current.flags & (1U << side)) ||
                cut.at(id).portals.at(current.edges[side]) != portal)
              return fail("Native cut changed a special connection record");
            accounted.insert(original);
            continue;
          }
          if (sources.empty())
            continue;
          const auto &targetMesh = before.at(portal.targetMesh);
          const auto &targetFace =
              targetMesh.triangles.at(portal.targetTriangle);
          std::vector<Edge> returns;
          for (unsigned other = 0; other < 3; ++other) {
            if (!(targetFace.flags & (1U << other)))
              continue;
            const auto &back = targetMesh.portals.at(targetFace.edges[other]);
            if (!back.type && back.targetMesh == id &&
                back.targetTriangle == index)
              returns.emplace_back(portal.targetMesh, portal.targetTriangle,
                                   other);
          }
          if (returns.size() > 1)
            return fail("Ambiguous authored portal return edge");
          std::set<Edge> targets;
          if (!returns.empty())
            targets = resolve(returns.front());
          unsigned axis{};
          float extent{};
          for (unsigned candidate = 0; candidate < 3; ++candidate) {
            const auto span = interval(original, candidate, before);
            if (span.second - span.first > extent) {
              extent = span.second - span.first;
              axis = candidate;
            }
          }
          if (returns.empty())
            directedSplits += sources.size() - 1;
          for (const auto &source : sources) {
            accounted.insert(source);
            const auto [prototype, fresh] = prototypes.emplace(source, portal);
            if (!fresh && prototype->second != portal)
              return fail("Conflicting cut edge provenance");
            std::optional<std::pair<std::uint32_t, std::uint16_t>> destination;
            if (!returns.empty()) {
              std::vector<Edge> matches;
              for (const auto &target : targets) {
                if (sources.size() == 1 && targets.size() == 1) {
                  matches.push_back(target);
                } else {
                  const auto x = interval(source, axis, cut);
                  const auto y = interval(target, axis, cut);
                  if (std::min(x.second, y.second) > std::max(x.first, y.first))
                    matches.push_back(target);
                }
              }
              if (matches.size() > 1)
                return fail(
                    "Native cut portal segments require further subdivision");
              if (!matches.empty())
                destination = {std::get<0>(matches.front()),
                               std::get<1>(matches.front())};
              else if (targets.empty() &&
                       !replacements.contains(returns.front()) &&
                       Active(cut.at(portal.targetMesh)
                                  .triangles.at(portal.targetTriangle))) {
                destination = {portal.targetMesh, portal.targetTriangle};
                ++nativeClosedReturns;
              }
            } else {
              if (!Active(cut.at(portal.targetMesh)
                              .triangles.at(portal.targetTriangle)))
                return fail(
                    "Directed cut destination needs explicit remapping");
              destination = {portal.targetMesh, portal.targetTriangle};
            }
            const auto [assignment, inserted] =
                assignments.emplace(source, destination);
            if (!inserted && assignment->second != destination)
              return fail("Conflicting native cut portal destinations");
          }
        }
      }
    }
    // Reject new or ambiguous external connections not accounted for by an
    // authored edge and its explicit replacement chain.
    for (const auto &[id, mesh] : cut)
      for (std::size_t index = 0; index < mesh.triangles.size(); ++index)
        if (Active(mesh.triangles[index]))
          for (unsigned side = 0; side < 3; ++side)
            if ((mesh.triangles[index].flags & (1U << side)) &&
                !accounted.contains(
                    {id, static_cast<std::uint16_t>(index), side}))
              return fail("Unaccounted external edge after native cut");
    // Saved native edges retain indices into the original portal array. Never
    // rewrite those records: redirect the active face to an appended record,
    // as Skyrim's edge-result consumer does. Aliased old indices may therefore
    // split into distinct destinations without corrupting native undo data.
    for (const auto &[edge, target] : assignments) {
      const auto &[id, triangle, side] = edge;
      auto &mesh = result.meshes.at(id);
      auto &face = mesh.triangles.at(triangle);
      auto record = face.edges[side];
      if (!target) {
        face.flags &= ~(1U << side);
        face.edges[side] = 0xFFFF;
        ++result.closed;
        continue;
      }
      const bool external = face.flags & (1U << side);
      auto portal = external ? mesh.portals.at(record) : prototypes.at(edge);
      if (portal.type)
        return fail("Ordinary cut replacement became a special link");
      if (!external || portal.targetMesh != target->first ||
          portal.targetTriangle != target->second) {
        portal.targetMesh = target->first;
        portal.targetTriangle = target->second;
        if (mesh.portals.size() >= 65535)
          return fail("Cut portal record budget exceeded");
        record = static_cast<std::uint16_t>(mesh.portals.size());
        mesh.portals.push_back(portal);
        face.edges[side] = record;
        face.flags |= 1U << side;
        ++result.redirected;
      }
    }
    const auto audit = AuditAuthoredTopology(result.meshes);
    if (!audit.Valid() || audit.unloadedTargets ||
        audit.asymmetricPortals > originalAudit.asymmetricPortals +
                                      nativeClosedReturns + directedSplits)
      return fail("Repaired cut graph failed connectivity audit: " +
                  audit.firstError);
  } catch (const std::exception &exception) {
    return fail(exception.what());
  }
  return result;
}
} // namespace SkyBuild::Navmesh
