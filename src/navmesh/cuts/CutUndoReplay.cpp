#include "navmesh/cuts/CutUndoReplay.h"
#include <algorithm>
#include <set>
namespace SkyBuild::Navmesh {
CutUndoReplay ReplayCapturedCutUndo(
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const std::map<std::uint32_t, std::vector<CutUndoRecord>> &cuts,
    const std::map<std::uint32_t, AuthoredMesh> *validatedSessionBasis) {
  CutUndoReplay result;
  result.meshes = originals;
  auto fail = [&](std::string message) {
    result.meshes.clear();
    result.restoredCuts = result.restoredEdges = result.redirectedPortals = 0;
    result.discardedUndoneCuts = 0;
    result.error = std::move(message);
    return result;
  };
  const auto before = AuditAuthoredTopology(originals);
  if (!before.Valid())
    return fail("Cut replay requires a valid authored graph");
  using Face = std::pair<std::uint32_t, std::uint16_t>;
  std::map<Face, Face> redirected;
  for (const auto &[id, records] : cuts) {
    const auto found = result.meshes.find(id);
    if (found == result.meshes.end())
      return fail("Cut replay source mesh is missing");
    auto &mesh = found->second;
    for (auto iterator = records.rbegin(); iterator != records.rend();
         ++iterator) {
      const auto &record = *iterator;
      if (!DecodeCutLifecycle(static_cast<std::uint32_t>(record.lifecycle)))
        return fail("Unknown cut lifecycle in owned snapshot");
      if (record.Applied() && record.hasSavedObstacleData)
        return fail("Saved obstacle ownership requires native restoration");
      if (!record.Applied() &&
          mesh.vertices.size() == record.originalVertices &&
          mesh.triangles.size() == record.originalTriangles) {
        // Exact original dimensions prove there is no suffix left to discard.
        // Already compacted geometry needs no cleanup, and the undone state
        // forbids restoring historical payload a second time.
        ++result.discardedUndoneCuts;
        continue;
      }
      // Reverse order must unwind the actual append stack. Gaps or an unknown
      // later append would make truncation an unsupported assumption.
      if (static_cast<std::uint64_t>(record.originalVertices) +
                  record.addedVertices !=
              mesh.vertices.size() ||
          static_cast<std::uint64_t>(record.originalTriangles) +
                  record.addedTriangles !=
              mesh.triangles.size())
        return fail("Cut ranges do not match the native append stack");
      if (!record.Applied()) {
        // Native undo leaves appended geometry in the buffers but marks its
        // faces inactive. Only a verified inactive suffix can be discarded;
        // the restored original flags, edges and traversal stay untouched.
        for (std::size_t index = record.originalTriangles;
             index < mesh.triangles.size(); ++index)
          if (!(mesh.triangles[index].flags & 32U))
            return fail("Undone cut retains an active appended triangle");
        for (std::size_t index = 0; index < record.originalTriangles; ++index)
          if (!(mesh.triangles[index].flags & (8U | 32U)))
            for (const auto vertex : mesh.triangles[index].vertices)
              if (vertex >= record.originalVertices)
                return fail("Undone cut original face uses appended geometry");
        mesh.vertices.resize(record.originalVertices);
        mesh.triangles.resize(record.originalTriangles);
        ++result.discardedUndoneCuts;
        continue;
      }
      std::set<std::uint16_t> affected;
      for (const auto index : record.affectedTriangles) {
        if (index >= record.originalTriangles || !affected.insert(index).second)
          return fail("Invalid or repeated affected cut triangle");
        mesh.triangles[index].flags &= static_cast<std::uint16_t>(~32U);
      }
      std::set<std::uint16_t> restored;
      for (const auto &saved : record.restoredEdges) {
        if (saved.triangle >= record.originalTriangles ||
            !restored.insert(saved.triangle).second)
          return fail("Invalid or repeated saved edge triangle");
        auto &face = mesh.triangles[saved.triangle];
        face.flags &= static_cast<std::uint16_t>(~0x107U);
        for (unsigned side = 0; side < 3; ++side) {
          face.edges[side] = saved.edges[side];
          if (saved.external[side])
            face.flags |= static_cast<std::uint16_t>(1U << side);
        }
        // Traced 93311 uses a three-bit cyclic level, with seven as the value
        // reached when decrementing zero. Keep all other flag/traversal bits.
        const auto level = (face.flags >> 13U) & 7U;
        face.flags = static_cast<std::uint16_t>((face.flags & ~0xE000U) |
                                                (((level + 7U) & 7U) << 13U));
        ++result.restoredEdges;
      }
      for (const auto &link : record.links) {
        if (link.sourceMesh != id || link.targetMesh != id)
          return fail("Inter-mesh cut restoration chain is not yet supported");
        if (link.sourceTriangle >= record.originalTriangles ||
            link.targetTriangle < record.originalTriangles ||
            link.targetTriangle >= mesh.triangles.size() ||
            link.sourceEdge >= 3 || link.targetEdge >= 3)
          return fail("Invalid cut face redirection");
        const Face from{link.targetMesh, link.targetTriangle};
        const Face to{link.sourceMesh, link.sourceTriangle};
        const auto [entry, inserted] = redirected.emplace(from, to);
        if (!inserted && entry->second != to)
          return fail("Conflicting cut face redirections");
      }
      for (const auto index : record.auxiliaryIndices)
        if (index < record.originalVertices || index >= mesh.vertices.size())
          return fail(
              "Unsupported native auxiliary index outside appended vertices");
      mesh.vertices.resize(record.originalVertices);
      mesh.triangles.resize(record.originalTriangles);
      ++result.restoredCuts;
    }
    mesh.coverTriangles = BuildAuthoredCover(mesh.triangles);
    // Preserve the dependency marker: this replay does not rebuild native maps
    // or prove their lifetime/cleanup, even when portable topology is restored.
  }
  for (auto &[id, mesh] : result.meshes)
    for (auto &portal : mesh.portals) {
      if (portal.type)
        continue;
      std::set<Face> seen;
      Face target{portal.targetMesh, portal.targetTriangle};
      while (redirected.contains(target)) {
        if (!seen.insert(target).second)
          return fail("Cyclic cut portal redirection");
        target = redirected.at(target);
      }
      if (target != Face{portal.targetMesh, portal.targetTriangle}) {
        portal.targetMesh = target.first;
        portal.targetTriangle = target.second;
        ++result.redirectedPortals;
      }
    }
  // Native restoration also queues external edge repair for the publication
  // manager. A saved edge can point through an extra record whose destination
  // was changed by the cut. Resolve it from a unique explicit return link,
  // never proximity: authored cell-border vertices need not be identical.
  for (const auto &[id, records] : cuts)
    for (const auto &record : records)
      if (record.Applied())
        for (const auto &saved : record.restoredEdges) {
          auto &mesh = result.meshes.at(id);
          if (saved.triangle >= mesh.triangles.size())
            continue; // Face belonged to an earlier cut's discarded append.
          auto &face = mesh.triangles[saved.triangle];
          for (unsigned side = 0; side < 3; ++side) {
            if (!(face.flags & (1U << side)))
              continue;
            if (face.edges[side] >= mesh.portals.size())
              return fail("Restored external edge index is invalid");
            auto &portal = mesh.portals[face.edges[side]];
            if (portal.type)
              return fail(
                  "Restored special edge needs native auxiliary handling");
            const auto target = result.meshes.find(portal.targetMesh);
            if (target == result.meshes.end())
              continue; // Preserve the descriptor until its neighbor is loaded.
            std::vector<std::uint16_t> returns;
            for (std::size_t t = 0; t < target->second.triangles.size(); ++t) {
              const auto &other = target->second.triangles[t];
              if (other.flags & (8U | 32U))
                continue;
              for (unsigned edge = 0; edge < 3; ++edge) {
                if (!(other.flags & (1U << edge)) ||
                    other.edges[edge] >= target->second.portals.size())
                  continue;
                const auto &back = target->second.portals[other.edges[edge]];
                if (!back.type && back.targetMesh == id &&
                    back.targetTriangle == saved.triangle)
                  returns.push_back(static_cast<std::uint16_t>(t));
              }
            }
            if (returns.size() > 1)
              return fail("Restored portal has ambiguous return links");
            if (returns.empty())
              continue; // Preserve directed links; final audit rejects new
                        // ones.
            const Face destination{portal.targetMesh, returns.front()};
            if (portal.targetTriangle != destination.second) {
              bool shared = false;
              for (std::size_t t = 0; t < mesh.triangles.size(); ++t) {
                const auto &other = mesh.triangles[t];
                if (other.flags & (8U | 32U))
                  continue;
                for (unsigned edge = 0; edge < 3; ++edge)
                  shared |= (t != saved.triangle || edge != side) &&
                            (other.flags & (1U << edge)) &&
                            other.edges[edge] == face.edges[side];
              }
              if (shared) {
                if (mesh.portals.size() >= 65535)
                  return fail("Restored portal index budget exceeded");
                face.edges[side] =
                    static_cast<std::uint16_t>(mesh.portals.size());
                mesh.portals.push_back(
                    {0, destination.first, destination.second});
              } else {
                portal.targetTriangle = destination.second;
              }
              ++result.redirectedPortals;
            }
          }
        }
  auto after = AuditAuthoredTopology(result.meshes);
  bool lostExternal = false;
  if (validatedSessionBasis)
    for (const auto &[id, records] : cuts) {
      const auto reference = validatedSessionBasis->find(id);
      const auto current = result.meshes.find(id);
      if (reference == validatedSessionBasis->end() ||
          current == result.meshes.end())
        continue;
      std::set<std::uint16_t> touched;
      for (const auto &record : records) {
        touched.insert(record.affectedTriangles.begin(),
                       record.affectedTriangles.end());
        for (const auto &edge : record.restoredEdges)
          touched.insert(edge.triangle);
      }
      for (const auto triangle : touched) {
        if (triangle >= reference->second.triangles.size() ||
            triangle >= current->second.triangles.size())
          continue;
        const auto &a = reference->second.triangles[triangle];
        const auto &b = current->second.triangles[triangle];
        for (unsigned side = 0; side < 3; ++side)
          lostExternal |= (a.flags & (1U << side)) &&
                          !(b.flags & (1U << side)) && b.edges[side] == 0xFFFF;
      }
    }
  if (validatedSessionBasis && (!after.Valid() || lostExternal)) {
    // A streamed native cut can discard external flags and omit its incoming
    // redirection from undo data. Use only the uncut graph validated earlier
    // in THIS scene, never serialized originals or an earlier save's pointers.
    auto repaired = result.meshes;
    for (const auto &[id, records] : cuts) {
      if (records.empty())
        continue;
      const auto reference = validatedSessionBasis->find(id);
      const auto current = repaired.find(id);
      if (current == repaired.end())
        return fail("Streamed cut repair source is missing");
      if (reference == validatedSessionBasis->end())
        continue; // Newly attached cuts keep their freshly replayed metadata.
      const auto &a = reference->second;
      auto &b = current->second;
      if (a.vertices != b.vertices ||
          a.triangles.size() != b.triangles.size() ||
          a.coverTriangles != b.coverTriangles ||
          a.doorTriangles != b.doorTriangles ||
          a.closedDoorTriangles != b.closedDoorTriangles)
        return fail("Streamed cut original geometry/metadata changed");
      std::set<std::uint16_t> touched;
      for (const auto &record : records) {
        touched.insert(record.affectedTriangles.begin(),
                       record.affectedTriangles.end());
        for (const auto &edge : record.restoredEdges)
          touched.insert(edge.triangle);
      }
      for (std::size_t i = 0; i < a.triangles.size(); ++i) {
        const auto &x = a.triangles[i], &y = b.triangles[i];
        if (x.vertices != y.vertices || x.traversal != y.traversal ||
            (x.flags & ~7U) != (y.flags & ~7U))
          return fail("Streamed cut original triangle changed");
        for (unsigned edge = 0; edge < 3; ++edge) {
          const bool external = x.flags & (1U << edge);
          const bool liveExternal = y.flags & (1U << edge);
          if (external != liveExternal &&
              (!touched.contains(static_cast<std::uint16_t>(i)) || !external ||
               y.edges[edge] != 0xFFFF))
            return fail("Unaccounted streamed external-edge change");
          if (!external && x.edges[edge] != y.edges[edge])
            return fail("Streamed cut original local adjacency changed");
        }
      }
      b = a;
    }
    // Repair only incoming links whose destination was a discarded cut face.
    // A unique pair of retained original vertex indices must prove its origin.
    for (auto &[id, mesh] : repaired) {
      const auto reference = validatedSessionBasis->find(id);
      if (reference == validatedSessionBasis->end())
        continue;
      for (std::size_t t = 0; t < mesh.triangles.size(); ++t)
        for (unsigned edge = 0; edge < 3; ++edge) {
          const auto &face = mesh.triangles[t];
          if (!(face.flags & (1U << edge)) || face.flags & (8U | 32U))
            continue;
          if (face.edges[edge] >= mesh.portals.size())
            return fail("Streamed source portal index is invalid");
          auto &portal = mesh.portals[face.edges[edge]];
          const auto target = repaired.find(portal.targetMesh);
          if (portal.type || target == repaired.end() ||
              portal.targetTriangle < target->second.triangles.size())
            continue;
          const auto original = originals.find(portal.targetMesh);
          const auto cutRecords = cuts.find(portal.targetMesh);
          if (!validatedSessionBasis->contains(portal.targetMesh))
            return fail("Streamed incoming repair target lacks a validated "
                        "session original");
          if (cutRecords == cuts.end() || cutRecords->second.empty() ||
              t >= reference->second.triangles.size())
            return fail("Streamed portal target is not a known native cut");
          const auto &hintFace = reference->second.triangles[t];
          if (!(hintFace.flags & (1U << edge)))
            return fail("Streamed incoming portal was not originally external");
          if (hintFace.edges[edge] >= reference->second.portals.size())
            return fail("Validated source portal index is invalid");
          const auto &hint = reference->second.portals[hintFace.edges[edge]];
          if (hint.type || hint.targetMesh != portal.targetMesh ||
              hint.targetTriangle >= target->second.triangles.size() ||
              original == originals.end() ||
              portal.targetTriangle >= original->second.triangles.size())
            return fail(
                "Streamed incoming portal has no stable original target");
          const auto &cutFace =
              original->second.triangles[portal.targetTriangle];
          const auto &sourceFace =
              target->second.triangles[hint.targetTriangle];
          unsigned retained{};
          for (const auto vertex : cutFace.vertices)
            if (vertex < target->second.vertices.size()) {
              if (std::find(sourceFace.vertices.begin(),
                            sourceFace.vertices.end(),
                            vertex) == sourceFace.vertices.end())
                return fail("Streamed cut portal provenance changed");
              ++retained;
            }
          if (retained < 2)
            return fail(
                "Streamed cut portal has insufficient original provenance");
          std::set<std::uint16_t> candidates;
          for (const auto &record : cutRecords->second)
            for (const auto index : record.affectedTriangles)
              if (index < target->second.triangles.size()) {
                const auto &candidate = target->second.triangles[index];
                bool matches = true;
                for (const auto vertex : cutFace.vertices)
                  if (vertex < target->second.vertices.size() &&
                      std::find(candidate.vertices.begin(),
                                candidate.vertices.end(),
                                vertex) == candidate.vertices.end())
                    matches = false;
                if (matches)
                  candidates.insert(index);
              }
          if (candidates.size() != 1 ||
              !candidates.contains(hint.targetTriangle))
            return fail("Streamed cut portal origin is ambiguous");
          portal.targetTriangle = hint.targetTriangle;
          ++result.redirectedPortals;
        }
    }
    const auto repairedAudit = AuditAuthoredTopology(repaired);
    if (!repairedAudit.Valid())
      return fail("Streamed cut repair did not restore valid topology: " +
                  repairedAudit.firstError);
    result.meshes = std::move(repaired);
    after = repairedAudit;
  }
  if (!after.Valid() || after.asymmetricPortals > before.asymmetricPortals ||
      after.specialLinks != before.specialLinks)
    return fail(
        "Restored cut topology failed preservation: " + after.firstError +
        "; errors " + std::to_string(after.errors) + ", directed portals " +
        std::to_string(before.asymmetricPortals) + " -> " +
        std::to_string(after.asymmetricPortals) + ", special links " +
        std::to_string(before.specialLinks) + " -> " +
        std::to_string(after.specialLinks));
  return result;
}
} // namespace SkyBuild::Navmesh
