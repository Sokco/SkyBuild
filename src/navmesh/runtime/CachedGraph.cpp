#include "navmesh/runtime/CachedGraph.h"
#include "navmesh/generation/MeshGeometry.h"
#include <cmath>
#include <limits>
namespace SkyBuild::Navmesh {
CachedGraph AssembleCachedGraph(
    const std::map<std::uint32_t, Mesh> &cells,
    const std::map<std::uint32_t, std::size_t> &sourceCounts,
    const std::map<std::uint32_t, BuildInput::Bounds> &domains) {
  CachedGraph result;
  // Reconcile the union of real edge endpoints on shared cell planes. A
  // fixed grid cannot match independently simplified contour end segments.
  struct Seam {
    std::uint32_t cell;
    Point a, b;
  };
  std::map<std::pair<unsigned, float>, std::vector<Seam>> planes;
  for (const auto &[cell, mesh] : cells) {
    const auto domain = domains.find(cell);
    if (domain == domains.end())
      continue;
    const auto &b = domain->second;
    for (const auto &face : mesh.triangles) {
      if (face.source.Authored())
        continue;
      for (unsigned side = 0; side < 3; ++side) {
        if (face.neighbors[side] >= 0)
          continue;
        const auto a = mesh.vertices.at(face.vertices[side]);
        const auto c = mesh.vertices.at(face.vertices[(side + 1) % 3]);
        if (a.x == c.x && (a.x == b.left || a.x == b.right))
          planes[{0, a.x}].push_back({cell, a, c});
        else if (a.y == c.y && (a.y == b.bottom || a.y == b.top))
          planes[{1, a.y}].push_back({cell, a, c});
      }
    }
  }
  std::map<std::uint32_t, std::vector<Point>> constraints;
  for (const auto &[plane, edges] : planes)
    for (const auto &edge : edges) {
      const auto &b = domains.at(edge.cell);
      const float first = plane.first == 0 ? edge.a.y : edge.a.x;
      const float last = plane.first == 0 ? edge.b.y : edge.b.x;
      if (first == last)
        continue;
      for (const auto &other : edges) {
        if (other.cell == edge.cell)
          continue;
        const auto &d = domains.at(other.cell);
        const bool adjacent =
            plane.first == 0
                ? ((b.left == d.right && plane.second == b.left) ||
                   (b.right == d.left && plane.second == b.right))
                : ((b.bottom == d.top && plane.second == b.bottom) ||
                   (b.top == d.bottom && plane.second == b.top));
        if (!adjacent)
          continue;
        for (auto point : {other.a, other.b}) {
          const float t =
              ((plane.first == 0 ? point.y : point.x) - first) / (last - first);
          if (t <= 0 || t >= 1)
            continue;
          const float z = edge.a.z + t * (edge.b.z - edge.a.z);
          // Separate terrain approximations can differ slightly in height.
          // Only reconcile surfaces within the existing traversable step.
          if (std::abs(z - point.z) > Geometry::MAX_STEP)
            continue;
          point.z = z;
          constraints[edge.cell].push_back(point);
        }
      }
    }
  std::map<std::uint32_t, Mesh> refined;
  for (const auto &[cell, points] : constraints) {
    const auto repaired = RefineGeneratedBoundaries(cells.at(cell), points);
    if (!repaired.Valid()) {
      result.ownership.error = "Cell seam refinement: " + repaired.error;
      return result;
    }
    refined.emplace(cell, repaired.mesh);
  }
  std::vector<std::uint32_t> triangleCells;
  for (const auto &[cell, originalMesh] : cells) {
    const auto found = refined.find(cell);
    const auto &mesh = found == refined.end() ? originalMesh : found->second;
    auto ownership = AssignConnectedOwners(mesh, sourceCounts);
    if (!ownership.Valid()) {
      result.ownership.error = ownership.error;
      return result;
    }
    if (!cell || result.mesh.vertices.size() + mesh.vertices.size() > 1000000 ||
        result.mesh.triangles.size() + mesh.triangles.size() > 1000000) {
      result.ownership.error = "Cached graph exceeds native staging limits";
      return result;
    }
    const auto vertexOffset =
        static_cast<std::uint32_t>(result.mesh.vertices.size());
    const auto triangleOffset =
        static_cast<std::uint32_t>(result.mesh.triangles.size());
    result.mesh.vertices.insert(result.mesh.vertices.end(),
                                mesh.vertices.begin(), mesh.vertices.end());
    for (auto triangle : mesh.triangles) {
      triangleCells.push_back(cell);
      for (auto &vertex : triangle.vertices)
        vertex += vertexOffset;
      for (auto &neighbor : triangle.neighbors)
        if (neighbor >= 0)
          neighbor += static_cast<std::int32_t>(triangleOffset);
      result.mesh.triangles.push_back(triangle);
    }
    result.ownership.owners.insert(result.ownership.owners.end(),
                                   ownership.owners.begin(),
                                   ownership.owners.end());
    for (auto component : ownership.newFormComponents) {
      for (auto &triangle : component)
        triangle += triangleOffset;
      result.ownership.newFormComponents.push_back(std::move(component));
      result.islandCells.push_back(cell);
    }
    result.cells.insert(cell);
  }
  // New walkable surfaces across an exterior cell boundary authorize their
  // own reciprocal connection, independent of the old ground-level portals.
  // XY is fixed by the shared grid/cell plane. Ray-hit Z can differ by float
  // rounding between cells; only the established .01 surface tolerance welds.
  using Position = std::pair<float, float>;
  using Key = std::pair<Position, Position>;
  struct Edge {
    std::uint32_t triangle, side, cell;
  };
  std::map<Key, std::vector<Edge>> boundaries;
  std::vector<std::uint32_t> canonical(result.mesh.vertices.size());
  for (std::uint32_t i = 0; i < canonical.size(); ++i)
    canonical[i] = i;
  const auto keyPoint = [](Point p) { return Position{p.x, p.y}; };
  for (std::uint32_t i = 0; i < result.mesh.triangles.size(); ++i) {
    auto &triangle = result.mesh.triangles[i];
    if (triangle.source.Authored())
      continue;
    const auto domain = domains.find(triangleCells[i]);
    if (domain == domains.end())
      continue;
    const auto &b = domain->second;
    for (unsigned side = 0; side < 3; ++side) {
      if (triangle.neighbors[side] >= 0)
        continue;
      const auto a = result.mesh.vertices[triangle.vertices[side]];
      const auto c = result.mesh.vertices[triangle.vertices[(side + 1) % 3]];
      if (!((a.x == c.x && (a.x == b.left || a.x == b.right)) ||
            (a.y == c.y && (a.y == b.bottom || a.y == b.top))))
        continue;
      const Key key{keyPoint(a), keyPoint(c)};
      const auto reverse = boundaries.find({key.second, key.first});
      if (reverse != boundaries.end()) {
        for (const auto other : reverse->second) {
          if (other.cell == triangleCells[i])
            continue;
          const auto &d = domains.at(other.cell);
          const bool adjacent =
              (a.x == c.x && ((b.left == d.right && a.x == b.left) ||
                              (b.right == d.left && a.x == b.right))) ||
              (a.y == c.y && ((b.bottom == d.top && a.y == b.bottom) ||
                              (b.top == d.bottom && a.y == b.top)));
          if (!adjacent)
            continue;
          auto &target = result.mesh.triangles[other.triangle];
          const auto first =
              result.mesh.vertices[target.vertices[(other.side + 1) % 3]];
          const auto second = result.mesh.vertices[target.vertices[other.side]];
          if (std::abs(a.z - first.z) > Geometry::MAX_STEP ||
              std::abs(c.z - second.z) > Geometry::MAX_STEP)
            continue;
          if (triangle.neighbors[side] >= 0 ||
              target.neighbors[other.side] >= 0) {
            result.ownership.error = "Ambiguous sampled cell boundary";
            return result;
          }
          triangle.neighbors[side] = static_cast<std::int32_t>(other.triangle);
          target.neighbors[other.side] = static_cast<std::int32_t>(i);
          // Native portals connect separate surfaces, including short steps.
          // Preserve each side's height instead of flattening one onto the
          // other. Exact surface matches can still share vertex indices.
          if (std::abs(a.z - first.z) <= Geometry::WELD_TOLERANCE &&
              std::abs(c.z - second.z) <= Geometry::WELD_TOLERANCE) {
            canonical[triangle.vertices[side]] =
                target.vertices[(other.side + 1) % 3];
            canonical[triangle.vertices[(side + 1) % 3]] =
                target.vertices[other.side];
          }
        }
      }
      boundaries[key].push_back({i, side, triangleCells[i]});
    }
  }
  for (auto &triangle : result.mesh.triangles)
    for (auto &vertex : triangle.vertices) {
      std::size_t steps{};
      while (canonical[vertex] != vertex) {
        vertex = canonical[vertex];
        if (++steps > canonical.size()) {
          result.ownership.error = "Cyclic cell seam weld";
          return result;
        }
      }
    }
  return result;
}
} // namespace SkyBuild::Navmesh
