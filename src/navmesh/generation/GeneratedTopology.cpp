#include "navmesh/generation/GeneratedTopology.h"
#include "navmesh/generation/MeshGeometry.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <tuple>
namespace SkyBuild::Navmesh {
namespace {
using Face = std::pair<std::uint32_t, std::uint16_t>;
struct Edge {
  Face face;
  unsigned side;
  Point a, b;
};
bool Near(Point a, Point b) {
  const auto dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz <= 0.0001f;
}
bool OnSegment(Point p, Point a, Point b) {
  const auto dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  const auto length = dx * dx + dy * dy + dz * dz;
  if (length <= 0.0001f)
    return false;
  const auto t =
      ((p.x - a.x) * dx + (p.y - a.y) * dy + (p.z - a.z) * dz) / length;
  return t >= -0.00001f && t <= 1.00001f &&
         Near(p, {a.x + t * dx, a.y + t * dy, a.z + t * dz});
}
std::optional<Point> PortalProjection(Point p, Point a, Point b) {
  const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
  const auto length = dx * dx + dy * dy;
  if (length <= .000001)
    return OnSegment(p, a, b) ? std::optional<Point>{p} : std::nullopt;
  const auto t = ((double(p.x) - a.x) * dx + (double(p.y) - a.y) * dy) / length;
  if (t < 0 || t > 1)
    return std::nullopt;
  const Point projected{static_cast<float>(a.x + t * dx),
                        static_cast<float>(a.y + t * dy),
                        static_cast<float>(a.z + t * (b.z - a.z))};
  if (std::hypot(p.x - projected.x, p.y - projected.y) >
          Geometry::WELD_TOLERANCE ||
      std::abs(p.z - projected.z) > Geometry::MAX_STEP)
    return std::nullopt;
  return projected;
}
std::pair<Point, Point> Endpoints(const AuthoredMesh &mesh,
                                  std::uint16_t triangle, unsigned side) {
  const auto &face = mesh.triangles.at(triangle);
  return {mesh.vertices.at(face.vertices[side]),
          mesh.vertices.at(face.vertices[(side + 1) % 3])};
}
} // namespace
std::optional<Point> ProjectDoorAnchor(const AuthoredMesh &mesh,
                                       std::uint16_t triangle, Point point) {
  if (triangle >= mesh.triangles.size() || !std::isfinite(point.x) ||
      !std::isfinite(point.y) || !std::isfinite(point.z))
    return std::nullopt;
  const auto &original = mesh.triangles[triangle];
  if (original.flags & (8U | 32U))
    return std::nullopt;
  const auto a = mesh.vertices.at(original.vertices[0]);
  const auto b = mesh.vertices.at(original.vertices[1]);
  const auto c = mesh.vertices.at(original.vertices[2]);
  const double bx = double(b.x) - a.x, by = double(b.y) - a.y;
  const double cx = double(c.x) - a.x, cy = double(c.y) - a.y;
  const double determinant = bx * cy - by * cx;
  if (std::abs(determinant) < .000001)
    return std::nullopt;
  const double px = double(point.x) - a.x, py = double(point.y) - a.y;
  const double u = (px * cy - py * cx) / determinant;
  const double v = (bx * py - by * px) / determinant;
  if (u < -.000001 || v < -.000001 || u + v > 1.000001)
    return std::nullopt;
  return Point{point.x, point.y,
               static_cast<float>(a.z + u * (b.z - a.z) + v * (c.z - a.z))};
}
bool ContainsDoorAnchor(const AuthoredMesh &mesh, std::uint16_t triangle,
                        Point point) {
  const auto projected = ProjectDoorAnchor(mesh, triangle, point);
  return projected && std::abs(projected->z - point.z) <= .01F;
}
PartitionedMesh PreserveGeneratedTopology(
    const Mesh &generated, const std::vector<std::uint32_t> &owners,
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const std::set<std::uint32_t> &replaced,
    const NativeDoorAnchors &doorAnchors, TopologyProfile *profile) {
  auto stageStart = std::chrono::steady_clock::now();
  if (profile)
    profile->stages.clear();
  auto mark = [&](const char *stage) {
    if (!profile)
      return;
    const auto now = std::chrono::steady_clock::now();
    profile->stages.emplace_back(
        stage,
        std::chrono::duration<double, std::milli>(now - stageStart).count());
    stageStart = now;
  };
  auto result = PartitionGeneratedMesh(generated, owners);
  if (!result.Valid())
    return result;
  auto fail = [&](std::string message) {
    result.meshes.clear();
    result.triangles.clear();
    result.triangleFragments.clear();
    result.deferredPortals.clear();
    result.doorRecordSources.clear();
    result.error = std::move(message);
    return result;
  };
  mark("partition generated geometry");
  const auto sourceAudit = AuditAuthoredTopology(originals);
  if (!sourceAudit.Valid())
    return fail("Authored graph is invalid");
  mark("validate source topology");
  auto changed = replaced;
  std::map<std::uint32_t, std::vector<std::size_t>> ownedTriangles;
  for (std::size_t i = 0; i < owners.size(); ++i)
    ownedTriangles[owners[i]].push_back(i);
  // Generation includes all cell baselines. Do not reindex or rebuild a native
  // mesh merely because it was included: retain every unchanged native record,
  // including inactive triangles and cut bookkeeping not in portable geometry.
  for (const auto id : replaced) {
    const auto found = originals.find(id);
    if (found == originals.end())
      return fail("Replacement source is missing");
    const auto &original = found->second;
    std::vector<bool> seen(original.triangles.size());
    bool identical = true;
    for (const auto i : ownedTriangles[id]) {
      const auto &face = generated.triangles[i];
      if (!face.source.Authored() || face.source.formID != id ||
          face.source.triangle >= original.triangles.size() ||
          seen[face.source.triangle]) {
        identical = false;
        break;
      }
      const auto &old = original.triangles[face.source.triangle];
      identical &= (old.flags & ~7U) == (face.flags & ~7U);
      for (unsigned side = 0; side < 3; ++side)
        identical &= Near(generated.vertices.at(face.vertices[side]),
                          original.vertices.at(old.vertices[side]));
      seen[face.source.triangle] = true;
    }
    for (std::size_t i = 0; i < seen.size(); ++i)
      if (!(original.triangles[i].flags & (8U | 32U)) && !seen[i])
        identical = false;
    if (!identical)
      continue;
    changed.erase(id);
    result.meshes[id] = original;
    for (const auto i : ownedTriangles[id])
      result.triangles[i].second =
          static_cast<std::uint16_t>(generated.triangles[i].source.triangle);
  }
  std::map<Face, std::vector<Face>> children;
  std::set<Face> sampledFaces;
  for (std::size_t i = 0; i < generated.triangles.size(); ++i)
    if (!generated.triangles[i].source.Authored())
      sampledFaces.insert(result.triangles[i]);
  for (const auto id : changed) {
    const auto found = originals.find(id);
    if (found == originals.end())
      return fail("Replacement source is missing");
    const auto &source = found->second;
    if (source.nativeDependencies ||
        std::any_of(source.triangles.begin(), source.triangles.end(),
                    [](const auto &face) { return (face.flags & 32U) != 0; }))
      return fail("Replacement requires native cut/overlap remapping");
  }
  for (std::size_t i = 0; i < generated.triangles.size(); ++i) {
    const auto &source = generated.triangles[i].source;
    const auto [owner, local] = result.triangles[i];
    if (originals.contains(owner) && !replaced.contains(owner))
      return fail("Generated owner would overwrite an unchanged mesh");
    if (!source.Authored())
      continue;
    const auto old = originals.find(source.formID);
    if (!replaced.contains(source.formID) || old == originals.end() ||
        source.triangle >= old->second.triangles.size() ||
        owner != source.formID)
      return fail("Invalid authored source ownership");
    const auto &original = old->second.triangles[source.triangle];
    if (original.flags & (8U | 32U))
      return fail("Generated source refers to inactive authored geometry");
    if (!changed.contains(owner))
      continue;
    auto &face = result.meshes.at(owner).triangles.at(local);
    face.traversal = original.traversal;
    // External flags describe indices rebuilt below; all other flags retain
    // their authored semantics instead of being inferred from sampled faces.
    face.flags =
        static_cast<std::uint16_t>((face.flags & 7U) | (original.flags & ~7U));
    children[{source.formID, static_cast<std::uint16_t>(source.triangle)}]
        .push_back({owner, local});
  }
  for (const auto &[id, mesh] : originals) {
    if (!changed.contains(id)) {
      if (result.meshes.contains(id) && !replaced.contains(id))
        return fail("Unchanged mesh ownership collision");
      result.meshes[id] = mesh;
      for (std::size_t i = 0; i < mesh.triangles.size(); ++i)
        children[{id, static_cast<std::uint16_t>(i)}] = {
            {id, static_cast<std::uint16_t>(i)}};
      continue;
    }
    // Remap door records after portal boundary refinement, which can split
    // these faces again. Preserve native record order and exactly one owner.
  }
  mark("retain originals and index provenance");
  // Index boundary faces once. Interior sampled faces cannot carry an
  // authored portal, so portal discovery need not visit every grid triangle.
  std::map<std::uint32_t, std::set<std::uint16_t>> boundaryFaces;
  for (const auto &[id, mesh] : result.meshes)
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
      const auto &face = mesh.triangles[i];
      if (!(face.flags & (8U | 32U)) &&
          ((face.flags & 7U) || std::find(face.edges.begin(), face.edges.end(),
                                          0xFFFF) != face.edges.end()))
        boundaryFaces[id].insert(static_cast<std::uint16_t>(i));
    }
  using EdgeKey = std::tuple<Face, float, float, float, float, float, float>;
  std::map<EdgeKey, std::vector<Edge>> edgeCache;
  auto edges = [&](Face original, Point a, Point b) {
    const EdgeKey key{original, a.x, a.y, a.z, b.x, b.y, b.z};
    if (const auto cached = edgeCache.find(key); cached != edgeCache.end())
      return cached->second;
    std::vector<Edge> output;
    auto candidates = children[original];
    // Sampled replacement faces have no authored triangle provenance. The
    // original portal segment still authorizes their connection: discover
    // surviving boundary edges on that segment instead of silently dropping
    // the link when its authored triangle was completely replaced.
    if (changed.contains(original.first)) {
      const auto found = result.meshes.find(original.first);
      if (found != result.meshes.end()) {
        std::set<Face> indexed(candidates.begin(), candidates.end());
        for (const auto index : boundaryFaces[original.first])
          indexed.insert({original.first, index});
        candidates.assign(indexed.begin(), indexed.end());
      }
    }
    for (const auto face : candidates) {
      const auto &mesh = result.meshes.at(face.first);
      if (mesh.triangles.at(face.second).flags & (8U | 32U))
        continue;
      for (unsigned side = 0; side < 3; ++side) {
        const auto [p, q] = Endpoints(mesh, face.second, side);
        const auto &triangle = mesh.triangles.at(face.second);
        const auto forward = (q.x - p.x) * (b.x - a.x) +
                             (q.y - p.y) * (b.y - a.y) +
                             (q.z - p.z) * (b.z - a.z);
        // Provenance identifies candidate faces, but does not make their
        // interior edges portal boundaries. Keep existing local connectivity.
        if (forward > 0 && PortalProjection(p, a, b) &&
            PortalProjection(q, a, b) &&
            ((triangle.flags & (1U << side)) || triangle.edges[side] == 0xFFFF))
          output.push_back({face, side, p, q});
      }
    }
    edgeCache.emplace(key, output);
    return output;
  };
  // Geometry welding alone cannot authorize connections between authored
  // faces. Restore the original directed connectivity; otherwise the builder
  // could turn a one-way portal into a reciprocal portal or join two touching
  // faces that the authored graph deliberately kept disconnected.
  for (std::size_t i = 0; i < generated.triangles.size(); ++i) {
    const auto &source = generated.triangles[i];
    const auto [owner, local] = result.triangles[i];
    if (!source.source.Authored() || !changed.contains(owner))
      continue;
    const auto &oldMesh = originals.at(source.source.formID);
    const auto &oldFace = oldMesh.triangles.at(source.source.triangle);
    auto &face = result.meshes.at(owner).triangles.at(local);
    for (unsigned side = 0; side < 3; ++side) {
      const auto neighbor = source.neighbors[side];
      if (neighbor < 0)
        continue;
      const auto &other = generated.triangles.at(neighbor).source;
      if (!other.Authored() || (other.formID == source.source.formID &&
                                other.triangle == source.source.triangle))
        continue;
      const auto a = generated.vertices.at(source.vertices[side]);
      const auto b = generated.vertices.at(source.vertices[(side + 1) % 3]);
      bool connected = false;
      for (unsigned oldSide = 0; oldSide < 3; ++oldSide) {
        const auto [p, q] = Endpoints(
            oldMesh, static_cast<std::uint16_t>(source.source.triangle),
            oldSide);
        if (!OnSegment(a, p, q) || !OnSegment(b, p, q))
          continue;
        if (oldFace.flags & (1U << oldSide)) {
          const auto &portal = oldMesh.portals.at(oldFace.edges[oldSide]);
          connected |= portal.type == 0 && portal.targetMesh == other.formID &&
                       portal.targetTriangle == other.triangle;
        } else {
          connected |= other.formID == source.source.formID &&
                       oldFace.edges[oldSide] == other.triangle;
        }
      }
      if (!connected) {
        face.edges[side] = 0xFFFF;
        face.flags &= static_cast<std::uint16_t>(~(1U << side));
      }
    }
  }
  // Match portal edge subdivisions without welding unrelated loaded meshes.
  // Split only boundary faces; preserve their other local/external edges.
  std::map<Face, std::vector<std::size_t>> fragmentOwners;
  bool fragmentsIndexed{};
  auto split = [&](Face original, const Edge &edge, Point point) -> bool {
    auto &mesh = result.meshes.at(edge.face.first);
    const auto index = edge.face.second;
    const auto old = mesh.triangles.at(index);
    const auto side = edge.side;
    if (Near(point, edge.a) || Near(point, edge.b) ||
        !OnSegment(point, edge.a, edge.b))
      return true;
    if (mesh.nativeDependencies || mesh.triangles.size() >= 65535 ||
        mesh.vertices.size() >= 65535 ||
        ((!(old.flags & (1U << side))) && old.edges[side] != 0xFFFF) ||
        (!changed.contains(edge.face.first) &&
         !doorAnchors.contains(edge.face.first) &&
         (std::find(mesh.doorTriangles.begin(), mesh.doorTriangles.end(),
                    index) != mesh.doorTriangles.end() ||
          std::find(mesh.closedDoorTriangles.begin(),
                    mesh.closedDoorTriangles.end(),
                    index) != mesh.closedDoorTriangles.end())))
      return false;
    const auto next = static_cast<std::uint16_t>(mesh.triangles.size());
    const auto vertex = static_cast<std::uint16_t>(mesh.vertices.size());
    const auto a = old.vertices[side], b = old.vertices[(side + 1) % 3],
               c = old.vertices[(side + 2) % 3];
    AuthoredTriangle first, second;
    first.flags = second.flags = static_cast<std::uint16_t>(old.flags & ~7U);
    first.traversal = second.traversal = old.traversal;
    first.vertices = {a, vertex, c};
    second.vertices = {vertex, b, c};
    first.edges = {0xFFFF, next, old.edges[(side + 2) % 3]};
    second.edges = {0xFFFF, old.edges[(side + 1) % 3], index};
    if (old.flags & (1U << ((side + 2) % 3)))
      first.flags |= 4U;
    if (old.flags & (1U << ((side + 1) % 3)))
      second.flags |= 2U;
    // The inherited B->C edge moved to the second triangle.
    if (!(second.flags & 2U) && second.edges[1] != 0xFFFF) {
      auto &neighbor = mesh.triangles.at(second.edges[1]);
      unsigned matches{};
      for (unsigned neighborSide = 0; neighborSide < 3; ++neighborSide)
        if (!(neighbor.flags & (1U << neighborSide)) &&
            neighbor.edges[neighborSide] == index &&
            neighbor.vertices[neighborSide] == c &&
            neighbor.vertices[(neighborSide + 1) % 3] == b) {
          neighbor.edges[neighborSide] = next;
          ++matches;
        }
      if (matches != 1)
        return false;
    }
    // Retain the original face's cyclic orientation instead of rotating the
    // opaque traversal word onto different edge slots.
    auto orient = [&](AuthoredTriangle &face) {
      const auto rotated = face;
      face.flags &= static_cast<std::uint16_t>(~7U);
      for (unsigned slot = 0; slot < 3; ++slot) {
        const auto destination = (side + slot) % 3;
        face.vertices[destination] = rotated.vertices[slot];
        face.edges[destination] = rotated.edges[slot];
        if (rotated.flags & (1U << slot))
          face.flags |= static_cast<std::uint16_t>(1U << destination);
      }
    };
    orient(first);
    orient(second);
    if (!fragmentsIndexed) {
      if (result.triangleFragments.empty())
        for (const auto face : result.triangles)
          result.triangleFragments.push_back({face});
      for (std::size_t i = 0; i < result.triangleFragments.size(); ++i)
        for (const auto face : result.triangleFragments[i])
          fragmentOwners[face].push_back(i);
      fragmentsIndexed = true;
    }
    const Face newFace{edge.face.first, next};
    if (sampledFaces.contains(edge.face))
      sampledFaces.insert(newFace);
    const auto owners = fragmentOwners.find(edge.face);
    if (owners != fragmentOwners.end()) {
      fragmentOwners[newFace] = owners->second;
      for (const auto owner : owners->second)
        result.triangleFragments[owner].push_back(newFace);
    }
    mesh.vertices.push_back(point);
    mesh.triangles[index] = first;
    mesh.triangles.push_back(second);
    boundaryFaces[edge.face.first].insert(index);
    boundaryFaces[edge.face.first].insert(next);
    // Only queries using the split face changed. Keep discoveries for every
    // other portal, even when refinement restarts its verification pass.
    for (auto cached = edgeCache.begin(); cached != edgeCache.end();) {
      const auto &[source, ax, ay, az, bx, by, bz] = cached->first;
      const Point queryA{ax, ay, az}, queryB{bx, by, bz};
      // An empty query may gain an edge when a longer edge is subdivided.
      // Invalidate overlapping segments too, not just existing matches.
      const bool overlap = source.first == edge.face.first &&
                           (OnSegment(queryA, edge.a, edge.b) ||
                            OnSegment(queryB, edge.a, edge.b) ||
                            OnSegment(edge.a, queryA, queryB) ||
                            OnSegment(edge.b, queryA, queryB));
      if (overlap || std::any_of(cached->second.begin(), cached->second.end(),
                                 [&](const Edge &candidate) {
                                   return candidate.face == edge.face;
                                 }))
        cached = edgeCache.erase(cached);
      else
        ++cached;
    }
    children[original].push_back({edge.face.first, next});
    changed.insert(edge.face.first);
    return true;
  };
  std::size_t boundarySplits{};
  auto parameter = [](Point point, Point a, Point b) {
    const auto x = b.x - a.x, y = b.y - a.y, z = b.z - a.z;
    if (x * x + y * y > .000001F)
      return ((point.x - a.x) * x + (point.y - a.y) * y) / (x * x + y * y);
    return ((point.x - a.x) * x + (point.y - a.y) * y + (point.z - a.z) * z) /
           (x * x + y * y + z * z);
  };
  // Native portal endpoints can differ along the same seam. Normalizing each
  // edge independently shifts their generated fragments to different physical
  // locations. Use a common coordinate on overlapping collinear XY segments;
  // separated authored connections retain their original parameter mapping.
  auto sharedLine = [&](Point a, Point b, Point c, Point d) {
    const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
    const auto length = std::hypot(dx, dy);
    if (length <= Geometry::WELD_TOLERANCE)
      return false;
    for (auto p : {c, d})
      if (std::abs(dx * (double(p.y) - a.y) - dy * (double(p.x) - a.x)) /
              length >
          Geometry::WELD_TOLERANCE)
        return false;
    const auto tc = parameter(c, a, b), td = parameter(d, a, b);
    return std::max(0.0F, std::min(tc, td)) < std::min(1.0F, std::max(tc, td));
  };
  // Near/refinement uses .01 world units. Matching fractions must use the
  // same spatial tolerance rather than a fixed fraction of every edge.
  const auto parameterTolerance = [](Point a, Point b) {
    const double x = double(b.x) - a.x, y = double(b.y) - a.y,
                 z = double(b.z) - a.z;
    const auto length =
        std::sqrt(x * x + y * y > .000001 ? x * x + y * y : z * z);
    return std::max(0.00001, length > 0 ? .01 / length : 0.00001);
  };
  auto returnSide =
      [&](Face from, unsigned side,
          const AuthoredPortal &portal) -> std::optional<unsigned> {
    const auto destination = originals.find(portal.targetMesh);
    if (destination == originals.end())
      return std::nullopt;
    const auto &target = destination->second;
    const auto &face = target.triangles.at(portal.targetTriangle);
    for (unsigned edge = 0; edge < 3; ++edge)
      if (face.flags & (1U << edge)) {
        const auto &back = target.portals.at(face.edges[edge]);
        if (!back.type && back.targetMesh == from.first &&
            back.targetTriangle == from.second)
          return edge;
      }
    const auto [a, b] = Endpoints(originals.at(from.first), from.second, side);
    for (unsigned edge = 0; edge < 3; ++edge) {
      const auto [c, d] = Endpoints(target, portal.targetTriangle, edge);
      if (Near(a, d) && Near(b, c))
        return edge;
    }
    return std::nullopt;
  };
  mark("boundary indexing and authored connectivity");
  // Prepare every shared boundary before editing any triangle. Each pair
  // contributes its endpoints in the opposite edge's parameter space. The
  // union is deterministic and independent of native mesh enumeration order.
  struct BoundaryPlan {
    Face original;
    Point a, b;
    std::vector<Point> points;
  };
  std::map<EdgeKey, BoundaryPlan> boundaryPlans;
  auto hasSampledEdge = [&](const std::vector<Edge> &edges) {
    return std::any_of(edges.begin(), edges.end(), [&](const Edge &edge) {
      return sampledFaces.contains(edge.face);
    });
  };
  auto plan = [&](Face original, Point a, Point b,
                  const std::vector<Edge> &opposite, Point c, Point d,
                  bool physicalSeam) {
    const EdgeKey key{original, a.x, a.y, a.z, b.x, b.y, b.z};
    auto [found, inserted] =
        boundaryPlans.try_emplace(key, BoundaryPlan{original, a, b, {}});
    for (const auto &edge : opposite)
      for (const auto endpoint : {edge.a, edge.b}) {
        const auto t = physicalSeam ? parameter(endpoint, a, b)
                                    : 1.0f - parameter(endpoint, c, d);
        found->second.points.push_back({a.x + t * (b.x - a.x),
                                        a.y + t * (b.y - a.y),
                                        a.z + t * (b.z - a.z)});
      }
  };
  for (const auto &[id, source] : originals)
    for (std::size_t index = 0; index < source.triangles.size(); ++index) {
      const auto &face = source.triangles[index];
      if (face.flags & (8U | 32U))
        continue;
      for (unsigned side = 0; side < 3; ++side) {
        if (!(face.flags & (1U << side)))
          continue;
        const auto &portal = source.portals.at(face.edges[side]);
        if (portal.type ||
            (!changed.contains(id) && !changed.contains(portal.targetMesh)))
          continue;
        const Face from{id, static_cast<std::uint16_t>(index)},
            to{portal.targetMesh, portal.targetTriangle};
        const auto targetSide = returnSide(from, side, portal);
        if (!targetSide)
          continue;
        const auto [a, b] = Endpoints(source, from.second, side);
        const auto [c, d] =
            Endpoints(originals.at(to.first), to.second, *targetSide);
        const auto left = edges(from, a, b), right = edges(to, c, d);
        const bool physicalSeam =
            sharedLine(a, b, c, d) &&
            (hasSampledEdge(left) || hasSampledEdge(right));
        plan(from, a, b, right, c, d, physicalSeam);
        plan(to, c, d, left, a, b, physicalSeam);
      }
    }
  mark("collect shared boundary constraints");
  // Apply the plan locally. A split cannot introduce an unplanned endpoint:
  // every cut point came from a boundary present when the plan was captured.
  // There is no graph-wide restart after each subdivision.
  for (auto &[key, boundary] : boundaryPlans) {
    auto &points = boundary.points;
    std::sort(points.begin(), points.end(), [&](Point a, Point b) {
      return parameter(a, boundary.a, boundary.b) <
             parameter(b, boundary.a, boundary.b);
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](Point a, Point b) { return Near(a, b); }),
                 points.end());
    for (const auto point : points) {
      const auto candidates = edges(boundary.original, boundary.a, boundary.b);
      for (const auto &edge : candidates) {
        const auto projected = PortalProjection(point, edge.a, edge.b);
        if (!projected || Near(*projected, edge.a) || Near(*projected, edge.b))
          continue;
        if (++boundarySplits > 65535 ||
            !split(boundary.original, edge, *projected))
          return fail("Portal boundary subdivision requires unsupported "
                      "local/door mapping");
      }
    }
  }
  mark("apply boundary subdivision plan");
  // Rebuild only affected ordinary links. Unchanged records retain exact
  // identity, including directed/special links outside the edited graph.
  std::map<std::pair<std::uint32_t, std::uint16_t>, Face> incomingTargets;
  for (const auto &[id, mesh] : originals)
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
      const auto &triangle = mesh.triangles[index];
      if (triangle.flags & (8U | 32U))
        continue;
      for (unsigned side = 0; side < 3; ++side) {
        if (!(triangle.flags & (1U << side)))
          continue;
        const auto &portal = mesh.portals.at(triangle.edges[side]);
        if (!originals.contains(portal.targetMesh)) {
          if (portal.targetMesh == 0 || portal.targetMesh >= 0xFF000000 ||
              portal.type > 2)
            return fail("Deferred portal needs a stable authored destination");
          const Face original{id, static_cast<std::uint16_t>(index)};
          const auto [a, b] = Endpoints(mesh, original.second, side);
          const auto fragments = edges(original, a, b);
          for (const auto &fragment : fragments) {
            auto &output = result.meshes.at(fragment.face.first)
                               .triangles.at(fragment.face.second);
            output.flags &= static_cast<std::uint16_t>(~(1U << fragment.side));
            output.edges[fragment.side] = 0xFFFF;
            result.deferredPortals.push_back(
                {fragment.face.first, fragment.face.second, fragment.side,
                 portal, fragment.a, fragment.b});
          }
          // Compact the now-unused native portal records during finalization.
          changed.insert(id);
          continue;
        }
        if (!changed.contains(id) && !changed.contains(portal.targetMesh))
          continue;
        if (portal.type) {
          const auto &from = children[{id, static_cast<std::uint16_t>(index)}];
          const auto &to = children[{portal.targetMesh, portal.targetTriangle}];
          auto unchangedFace = [&](Face old, const std::vector<Face> &mapped) {
            if (mapped.size() != 1 || mapped.front() != old)
              return false;
            const auto &before = originals.at(old.first);
            const auto &after = result.meshes.at(old.first);
            for (unsigned vertex = 0; vertex < 3; ++vertex)
              if (!Near(before.vertices.at(
                            before.triangles.at(old.second).vertices[vertex]),
                        after.vertices.at(
                            after.triangles.at(old.second).vertices[vertex])))
                return false;
            return true;
          };
          if ((portal.type != 1 && portal.type != 2) ||
              !unchangedFace({id, static_cast<std::uint16_t>(index)}, from) ||
              !unchangedFace({portal.targetMesh, portal.targetTriangle}, to))
            return fail(
                "Changed special portal requires native auxiliary mapping");
          auto &output = result.meshes.at(id);
          auto &face = output.triangles.at(static_cast<std::uint16_t>(index));
          // Partitioning rebuilds ordinary adjacency only. Restore an authored
          // special link only after both endpoint faces retain their identity.
          if (!(face.flags & (1U << side)) && face.edges[side] == 0xFFFF) {
            if (output.portals.size() >= 65535)
              return fail("Native partition exceeds portal index limits");
            face.edges[side] = static_cast<std::uint16_t>(output.portals.size());
            face.flags |= static_cast<std::uint16_t>(1U << side);
            output.portals.push_back(portal);
          }
          if (!(face.flags & (1U << side)) ||
              output.portals.at(face.edges[side]) != portal)
            return fail(
                "Special portal record changed during boundary refinement");
          continue;
        }
        const auto [a, b] =
            Endpoints(mesh, static_cast<std::uint16_t>(index), side);
        const auto sourceEdges =
            edges({id, static_cast<std::uint16_t>(index)}, a, b);
        const auto targetSide =
            returnSide({id, static_cast<std::uint16_t>(index)}, side, portal);
        const auto targetEndpoints =
            targetSide ? Endpoints(originals.at(portal.targetMesh),
                                   portal.targetTriangle, *targetSide)
                       : std::pair{a, b};
        const auto [c, d] = targetEndpoints;
        const auto tolerance =
            std::max(parameterTolerance(a, b), parameterTolerance(c, d));
        auto targetEdges =
            targetSide ? edges({portal.targetMesh, portal.targetTriangle}, c, d)
                       : std::vector<Edge>{};
        const bool physicalSeam =
            targetSide && sharedLine(a, b, c, d) &&
            (hasSampledEdge(sourceEdges) || hasSampledEdge(targetEdges));
        auto targetParameter = [&](Point p) {
          return physicalSeam ? parameter(p, a, b) : 1.0F - parameter(p, c, d);
        };
        if (!targetSide) {
          const auto &mapped =
              children[{portal.targetMesh, portal.targetTriangle}];
          if (mapped.size() != 1)
            return fail("Directed portal without a return edge requires an "
                        "unchanged destination");
          const auto &oldTarget = originals.at(portal.targetMesh);
          const auto &newTarget = result.meshes.at(mapped.front().first);
          for (unsigned vertex = 0; vertex < 3; ++vertex)
            if (!Near(oldTarget.vertices.at(
                          oldTarget.triangles.at(portal.targetTriangle)
                              .vertices[vertex]),
                      newTarget.vertices.at(
                          newTarget.triangles.at(mapped.front().second)
                              .vertices[vertex])))
              return fail("Directed portal destination geometry changed "
                          "without a return edge");
          targetEdges.push_back({mapped.front(), 0, b, a});
        }
        for (const auto &edge : sourceEdges) {
          const Edge *destination = nullptr;
          for (const auto &target : targetEdges)
            if (!targetSide ||
                (std::abs(parameter(edge.a, a, b) -
                          targetParameter(target.b)) <= tolerance &&
                 std::abs(parameter(edge.b, a, b) -
                          targetParameter(target.a)) <= tolerance)) {
              if (destination)
                return fail("Ambiguous generated portal destination");
              destination = &target;
            }
          if (!destination) {
            // Clipping can remove the destination's whole boundary fragment.
            // Refinement above already aligned surviving endpoints. Keep this
            // fragment closed rather than retaining a stale native portal.
            for (const auto &target : targetEdges)
              if (std::max(parameter(edge.a, a, b), targetParameter(target.b)) +
                      tolerance <
                  std::min(parameter(edge.b, a, b), targetParameter(target.a)))

                return fail(std::format(
                    "Portal partial overlap {:08X}/{} -> {:08X}/{}: source "
                    "[{:.9f}, {:.9f}], target [{:.9f}, {:.9f}]",
                    id, index, portal.targetMesh, portal.targetTriangle,
                    parameter(edge.a, a, b), parameter(edge.b, a, b),
                    targetParameter(target.b), targetParameter(target.a)));
            auto &face = result.meshes.at(edge.face.first)
                             .triangles.at(edge.face.second);
            if (mesh.nativeDependencies)
              return fail(
                  "Closing an incoming portal requires native cut bookkeeping");
            face.edges[edge.side] = 0xFFFF;
            face.flags &= static_cast<std::uint16_t>(~(1U << edge.side));
            changed.insert(edge.face.first);
            continue;
          }
          auto &output = result.meshes.at(edge.face.first);
          auto &face = output.triangles.at(edge.face.second);
          // An unchanged source already holds this external edge. Replace its
          // target in place, retaining any native records indexed by portal ID.
          if (!changed.contains(id)) {
            const auto record = face.edges[edge.side];
            const auto [assignment, inserted] = incomingTargets.emplace(
                std::pair{id, record}, destination->face);
            if (!inserted && assignment->second != destination->face)
              return fail(
                  "Shared incoming portal requires incompatible targets");
            if (mesh.nativeDependencies &&
                (portal.targetMesh != destination->face.first ||
                 portal.targetTriangle != destination->face.second))
              return fail(
                  "Incoming portal remap requires native cut bookkeeping");
            output.portals.at(record) = {0, destination->face.first,
                                         destination->face.second};
          } else {
            if (face.edges[edge.side] != 0xFFFF) {
              if (face.flags & (1U << edge.side)) {
                auto &existing = output.portals.at(face.edges[edge.side]);
                if (!existing.type &&
                    existing.targetMesh == destination->face.first &&
                    existing.targetTriangle == destination->face.second)
                  continue;
                bool refinedDestination = false;
                if (!existing.type) {
                  const auto fragments = fragmentOwners.find(destination->face);
                  if (fragments != fragmentOwners.end())
                    for (const auto source : fragments->second)
                      refinedDestination |=
                          result.triangles.at(source) ==
                          Face{existing.targetMesh, existing.targetTriangle};
                }
                // The partition's original target index becomes stale when
                // another boundary splits that face. Only remap to a recorded
                // descendant; matching geometry alone cannot authorize it.
                if (!existing.type &&
                    (refinedDestination ||
                     (existing.targetMesh == portal.targetMesh &&
                      existing.targetTriangle == portal.targetTriangle))) {
                  const auto [assignment, inserted] = incomingTargets.emplace(
                      std::pair{edge.face.first, face.edges[edge.side]},
                      destination->face);
                  if (!inserted && assignment->second != destination->face)
                    return fail(
                        "Shared portal requires incompatible remapped targets");
                  existing = {0, destination->face.first,
                              destination->face.second};
                  continue;
                }
              }

              return fail(std::format(
                  "Authored portal conflicts with generated adjacency: "
                  "{:08X}/{} side {} -> {:08X}/{}, existing edge {} flags {} "
                  "target {:08X}/{} type {}",
                  edge.face.first, edge.face.second, edge.side,
                  destination->face.first, destination->face.second,
                  face.edges[edge.side], face.flags,
                  (face.flags & (1U << edge.side))
                      ? output.portals.at(face.edges[edge.side]).targetMesh
                      : 0,
                  (face.flags & (1U << edge.side))
                      ? output.portals.at(face.edges[edge.side]).targetTriangle
                      : 0,
                  (face.flags & (1U << edge.side))
                      ? output.portals.at(face.edges[edge.side]).type
                      : 0));
            }
            if (output.portals.size() >= 65535)
              return fail("Generated authored portal index limit exceeded");
            face.flags |= static_cast<std::uint16_t>(1U << edge.side);
            face.edges[edge.side] =
                static_cast<std::uint16_t>(output.portals.size());
            output.portals.push_back(
                {0, destination->face.first, destination->face.second});
          }
        }
      }
    }
  mark("construct portal records");
  for (const auto id : changed) {
    const auto &original = originals.at(id);
    std::string doorError;
    if (result.meshes.contains(id)) {
      result.meshes.at(id).doorTriangles.clear();
      result.meshes.at(id).closedDoorTriangles.clear();
    }
    auto remapDoors = [&](const std::vector<std::uint16_t> &indices,
                          bool closed) -> bool {
      for (std::size_t record = 0; record < indices.size(); ++record) {
        const auto old = indices[record];
        auto reject = [&](std::string_view reason) {
          doorError =
              std::format("{} door record {} (authored triangle {}): {}",
                          closed ? "Closed" : "Open", record, old, reason);
          return false;
        };
        const auto &mapped = children[{id, old}];
        std::optional<Face> selected;
        // An exactly retained triangle needs no additional native metadata.
        if (mapped.size() == 1) {
          const auto candidate = mapped.front();
          const auto &output = result.meshes.at(candidate.first);
          const auto &before = original.triangles.at(old);
          const auto &after = output.triangles.at(candidate.second);
          bool same = true;
          for (unsigned vertex = 0; vertex < 3; ++vertex)
            same &= Near(original.vertices.at(before.vertices[vertex]),
                         output.vertices.at(after.vertices[vertex]));
          if (same)
            selected = candidate;
        }
        if (!selected) {
          const auto anchors = doorAnchors.find(id);
          if (anchors != doorAnchors.end()) {
            const auto &unbound = closed ? anchors->second.unboundClosedDoors
                                         : anchors->second.unboundDoors;
            if (std::ranges::find(unbound, record) != unbound.end()) {
              // No door reference/access point exists to choose one child.
              // Retain the authored attachment region, never an arbitrary
              // neighboring/generated face. Native arrays support repeated
              // records for the same PathingDoor.
              if (mapped.empty())
                return reject("unbound door has no surviving authored surface");
              for (const auto &candidate : mapped) {
                if (candidate.first != id)
                  return reject("unbound door changed owning mesh");
                const auto &output = result.meshes.at(id);
                for (auto vertex :
                     output.triangles.at(candidate.second).vertices)
                  if (!ContainsDoorAnchor(original, old,
                                          output.vertices.at(vertex))) {
                    const auto point = output.vertices.at(vertex);
                    bool boundary = false;
                    for (unsigned side = 0; side < 3; ++side) {
                      const auto [a, b] = Endpoints(original, old, side);
                      boundary |= OnSegment(point, a, b);
                    }
                    if (!boundary)
                      return reject(
                          "unbound door descendant left its authored surface");
                  }
              }
              for (const auto &candidate : mapped) {
                auto &output = result.meshes.at(id);
                auto &faces =
                    closed ? output.closedDoorTriangles : output.doorTriangles;
                auto &sources = closed
                                    ? result.doorRecordSources[id].closedDoors
                                    : result.doorRecordSources[id].doors;
                if (faces.size() >= 65535)
                  return reject("door record limit exceeded");
                faces.push_back(candidate.second);
                sources.push_back(static_cast<std::uint16_t>(record));
              }
              continue;
            }
          }
          if (anchors == doorAnchors.end())
            return reject("native access location unavailable");
          const auto &points =
              closed ? anchors->second.closedDoors : anchors->second.doors;
          if (record >= points.size() || !points[record])
            return reject("native access location missing or ambiguous");
          if (!ContainsDoorAnchor(original, old, *points[record]))
            return reject(
                "access location does not match the original surface");
          const auto output = result.meshes.find(id);
          if (output == result.meshes.end())
            return reject("owning mesh was entirely removed");
          // Keep the access XY in the same native mesh. Recast's voxel surface
          // can be slightly above the original collision plane; choose the
          // nearest traversable height, never an arbitrary overlapping layer.
          float nearest = Geometry::MAX_STEP + 1;
          for (std::size_t triangle = 0;
               triangle < output->second.triangles.size(); ++triangle) {
            const Face candidate{id, static_cast<std::uint16_t>(triangle)};
            const auto projected = ProjectDoorAnchor(
                output->second, candidate.second, *points[record]);
            const auto distance =
                projected ? std::abs(projected->z - points[record]->z)
                          : Geometry::MAX_STEP + 1;
            if (distance <= Geometry::MAX_STEP) {
              // On a shared edge either incident child owns the same access
              // point. Stable ordering avoids duplicating the native record.
              if (!selected || distance < nearest ||
                  (distance == nearest && candidate < *selected)) {
                selected = candidate;
                nearest = distance;
              }
            }
          }
        }
        if (!selected)
          return reject("no replacement surface at the door access location");
        auto &output = result.meshes.at(selected->first);
        (closed ? output.closedDoorTriangles : output.doorTriangles)
            .push_back(selected->second);
        auto &sources =
            closed ? result.doorRecordSources[selected->first].closedDoors
                   : result.doorRecordSources[selected->first].doors;
        sources.push_back(static_cast<std::uint16_t>(record));
      }
      return true;
    };
    if (!remapDoors(original.doorTriangles, false) ||
        !remapDoors(original.closedDoorTriangles, true))
      return fail(std::format(
          "Door access point could not be preserved in mesh {:08X}: {}", id,
          doorError));
  }
  // Removed generated links must not leave unused portal records for native
  // info builders to mistake for adjacent meshes. Unchanged native arrays keep
  // their exact indices; changed meshes have no supported auxiliary dependents.
  for (const auto id : changed) {
    const auto found = result.meshes.find(id);
    if (found == result.meshes.end())
      continue;
    auto &mesh = found->second;
    mesh.coverTriangles = BuildAuthoredCover(mesh.triangles);
    const auto old = std::move(mesh.portals);
    std::map<std::uint16_t, std::uint16_t> indices;
    mesh.portals.clear();
    for (auto &face : mesh.triangles)
      for (unsigned side = 0; side < 3; ++side)
        if (face.flags & (1U << side)) {
          const auto record = face.edges[side];
          auto [mapped, inserted] = indices.emplace(
              record, static_cast<std::uint16_t>(mesh.portals.size()));
          if (inserted)
            mesh.portals.push_back(old.at(record));
          face.edges[side] = mapped->second;
        }
  }
  mark("remap doors and finalize metadata");
  const auto audit = AuditAuthoredTopology(result.meshes);
  mark("validate completed topology");
  if (!audit.Valid() || audit.unloadedTargets)
    return fail("Rebuilt authored topology failed validation: " +
                audit.firstError);
  return result;
}
} // namespace SkyBuild::Navmesh
