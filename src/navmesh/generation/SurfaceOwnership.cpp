#include "navmesh/generation/SurfaceOwnership.h"
#include <cmath>
#include <limits>
#include <queue>
#include <tuple>
namespace SkyBuild::Navmesh {
SurfaceOwnership AssignConnectedOwners(
    const Mesh &mesh,
    const std::map<std::uint32_t, std::size_t> &sourceCounts) {
  SurfaceOwnership result;
  auto fail = [&](const char *error) {
    result.owners.clear();
    result.newFormComponents.clear();
    result.error = error;
    return result;
  };
  result.owners.resize(mesh.triangles.size());
  std::vector<Point> centers;
  std::vector<double> distances(mesh.triangles.size(),
                                std::numeric_limits<double>::infinity());
  using Entry = std::tuple<double, std::uint32_t, std::size_t>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
    const auto &triangle = mesh.triangles[i];
    Point center{};
    for (const auto vertex : triangle.vertices) {
      if (vertex >= mesh.vertices.size())
        return fail("Invalid ownership vertex index");
      const auto &point = mesh.vertices[vertex];
      if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z))
        return fail("Non-finite ownership vertex");
      center.x += point.x / 3;
      center.y += point.y / 3;
      center.z += point.z / 3;
    }
    centers.push_back(center);
    for (const auto neighbor : triangle.neighbors)
      if (neighbor < -1 ||
          (neighbor >= 0 &&
           static_cast<std::size_t>(neighbor) >= mesh.triangles.size()))
        return fail("Invalid ownership adjacency");
    for (unsigned edge = 0; edge < 3; ++edge) {
      const auto neighbor = triangle.neighbors[edge];
      if (neighbor < 0)
        continue;
      const auto &other = mesh.triangles[neighbor];
      unsigned reverseEdges{};
      for (unsigned reverse = 0; reverse < 3; ++reverse)
        reverseEdges +=
            other.neighbors[reverse] == static_cast<std::int32_t>(i) &&
            other.vertices[reverse] == triangle.vertices[(edge + 1) % 3] &&
            other.vertices[(reverse + 1) % 3] == triangle.vertices[edge];
      if (reverseEdges != 1)
        return fail("Ownership adjacency is not uniquely reciprocal");
    }
    if (triangle.source.Authored()) {
      const auto source = sourceCounts.find(triangle.source.formID);
      if (!triangle.source.formID || source == sourceCounts.end() ||
          triangle.source.triangle >= source->second)
        return fail("Unknown authored triangle provenance");
      result.owners[i] = triangle.source.formID;
      distances[i] = 0;
      queue.emplace(0, triangle.source.formID, i);
    }
  }
  while (!queue.empty()) {
    const auto [distance, owner, index] = queue.top();
    queue.pop();
    if (distance != distances[index] || owner != result.owners[index])
      continue;
    for (const auto neighbor : mesh.triangles[index].neighbors) {
      if (neighbor < 0 || mesh.triangles[neighbor].source.Authored())
        continue;
      const double x =
          static_cast<double>(centers[index].x) - centers[neighbor].x;
      const double y =
          static_cast<double>(centers[index].y) - centers[neighbor].y;
      const double z =
          static_cast<double>(centers[index].z) - centers[neighbor].z;
      const auto next =
          distance + std::max(.001, std::sqrt(x * x + y * y + z * z));
      if (next < distances[neighbor] ||
          (next == distances[neighbor] && owner < result.owners[neighbor])) {
        distances[neighbor] = next;
        result.owners[neighbor] = owner;
        queue.emplace(next, owner, neighbor);
      }
    }
  }
  std::vector<bool> seen(mesh.triangles.size());
  for (std::uint32_t i = 0; i < mesh.triangles.size(); ++i) {
    if (result.owners[i] || seen[i])
      continue;
    std::vector<std::uint32_t> pending{i}, component;
    seen[i] = true;
    while (!pending.empty()) {
      const auto index = pending.back();
      pending.pop_back();
      component.push_back(index);
      for (const auto neighbor : mesh.triangles[index].neighbors)
        if (neighbor >= 0 && !result.owners[neighbor] && !seen[neighbor]) {
          seen[neighbor] = true;
          pending.push_back(neighbor);
        }
    }
    result.newFormComponents.push_back(std::move(component));
  }
  return result;
}
} // namespace SkyBuild::Navmesh
