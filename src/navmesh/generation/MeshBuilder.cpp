#include "navmesh/generation/MeshBuilder.h"
#include "navmesh/generation/MeshGeometry.h"
#include "navmesh/generation/XYBoundsIndex.h"
#include "navmesh/generation/recast/RecastBuilder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
namespace SkyBuild::Navmesh {
using namespace Geometry;
namespace {
// Clipping a coarse baseline beside a sampled grid leaves vertices in the
// middle of baseline edges. Split those edges before building adjacency so
// matching surfaces share complete edges rather than disconnected T junctions.
bool Stitch(Writer &writer, const std::function<bool()> &stop) {
  const auto count = writer.mesh.vertices.size();
  std::vector<bool> authored(count);
  std::map<std::pair<std::uint32_t, std::uint32_t>, unsigned> edgeUses;
  for (const auto &face : writer.mesh.triangles) {
    for (unsigned side = 0; side < 3; ++side) {
      const auto a = face.vertices[side], b = face.vertices[(side + 1) % 3];
      authored[a] = authored[a] || face.source.Authored();
      ++edgeUses[{std::min(a, b), std::max(a, b)}];
    }
  }
  std::vector<XYBounds> pointBounds;
  pointBounds.reserve(count);
  for (const auto &point : writer.mesh.vertices)
    pointBounds.push_back({point.x, point.y, point.x, point.y});
  const XYBoundsIndex vertexIndex(std::move(pointBounds));
  std::vector<std::uint32_t> candidates;
  auto triangles = std::move(writer.mesh.triangles);
  for (const auto &triangle : triangles) {
    if (stop())
      return false;
    std::array<std::vector<std::pair<float, std::uint32_t>>, 3> edgeSplits;
    for (unsigned side = 0; side < 3; ++side) {
      const auto ai = triangle.vertices[side];
      const auto bi = triangle.vertices[(side + 1) % 3];
      const auto a = writer.mesh.vertices[ai], b = writer.mesh.vertices[bi];
      const Point delta{b.x - a.x, b.y - a.y, b.z - a.z};
      const float length2 =
          delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
      const float xyLength2 = delta.x * delta.x + delta.y * delta.y;
      const bool sampledBoundary =
          !triangle.source.Authored() &&
          edgeUses.at({std::min(ai, bi), std::max(ai, bi)}) == 1;
      std::vector<std::pair<float, std::uint32_t>> splits;
      const XYBounds bounds{std::min(a.x, b.x) - WELD_TOLERANCE,
                            std::min(a.y, b.y) - WELD_TOLERANCE,
                            std::max(a.x, b.x) + WELD_TOLERANCE,
                            std::max(a.y, b.y) + WELD_TOLERANCE};
      if (!vertexIndex.Query(bounds, candidates, stop))
        return false;
      for (const auto id : candidates) {
        const auto p = writer.mesh.vertices[id];
        if (stop())
          return false;
        if (std::find(triangle.vertices.begin(), triangle.vertices.end(), id) !=
                triangle.vertices.end() ||
            length2 <= 0)
          continue;
        // Authored faces are immutable support. A tolerance-near point on
        // the outside of an acute corner must not expand a face into its
        // neighbor; being close to its edge line is not sufficient incidence.
        if (triangle.source.Authored()) {
          bool inside = true;
          for (unsigned edge = 0; edge < 3; ++edge) {
            const auto first = writer.mesh.vertices[triangle.vertices[edge]];
            const auto last =
                writer.mesh.vertices[triangle.vertices[(edge + 1) % 3]];
            inside &=
                (double(last.x) - first.x) * (double(p.y) - first.y) -
                    (double(last.y) - first.y) * (double(p.x) - first.x) >=
                0;
          }
          if (!inside)
            continue;
        }
        const float t = ((p.x - a.x) * delta.x + (p.y - a.y) * delta.y +
                         (p.z - a.z) * delta.z) /
                        length2;
        if (t <= 0 || t >= 1)
          continue;
        const auto q = Mix(a, b, t);
        const float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
        // Generated detail meshes can contain very thin triangles. The weld
        // tolerance is too broad for inserting points into their shared edges:
        // a nearby point on the opposite side can fold the resulting fan.
        const float splitTolerance =
            !triangle.source.Authored() && (!sampledBoundary || !authored[id])
                ? .0001F
                : WELD_TOLERANCE;
        if (dx * dx + dy * dy + dz * dz <= splitTolerance * splitTolerance)
          splits.emplace_back(t, id);
        else if (sampledBoundary &&
                 (authored[id] || writer.boundaryConstraints.contains(id)) &&
                 xyLength2 > .000001F) {
          const float xyT =
              ((p.x - a.x) * delta.x + (p.y - a.y) * delta.y) / xyLength2;
          const auto projected = Mix(a, b, xyT);
          const float x = p.x - projected.x, y = p.y - projected.y;
          if (xyT > 0 && xyT < 1 &&
              x * x + y * y <= WELD_TOLERANCE * WELD_TOLERANCE &&
              std::abs(p.z - projected.z) <= MAX_STEP)
            splits.emplace_back(xyT, id);
        }
      }
      std::ranges::sort(splits);
      if (!triangle.source.Authored()) {
        const auto c = writer.mesh.vertices[triangle.vertices[(side + 2) % 3]];
        const Point center{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3,
                           (a.z + b.z + c.z) / 3};
        const auto validWedge = [&](Point first, Point last) {
          return Walkable(center, first, last, writer.validatedWalkability);
        };
        // A nearby boundary vertex can make a thin fan wedge too steep.
        // Decline that split instead of silently dropping part of the face.
        std::vector<std::pair<float, std::uint32_t>> accepted;
        Point previous = a;
        for (const auto &split : splits) {
          const auto p = writer.mesh.vertices[split.second];
          if (validWedge(previous, p) && validWedge(p, b)) {
            accepted.push_back(split);
            previous = p;
          }
        }
        splits = std::move(accepted);
      }
      edgeSplits[side] = std::move(splits);
    }
    // Near an acute corner, the tolerance tube of two edges can overlap.
    // A split vertex belongs to just one edge of a face: choose the closest
    // eligible projection before assembling its cyclic boundary. Inserting it
    // twice creates a self-touching ring and reversed, overlapping fan wedges.
    std::map<std::uint32_t, std::pair<double, unsigned>> splitOwners;
    for (unsigned side = 0; side < 3; ++side) {
      const auto a = writer.mesh.vertices[triangle.vertices[side]];
      const auto b = writer.mesh.vertices[triangle.vertices[(side + 1) % 3]];
      for (const auto &[t, id] : edgeSplits[side]) {
        const auto p = writer.mesh.vertices[id];
        const auto q = Mix(a, b, t);
        const double dx = double(p.x) - q.x, dy = double(p.y) - q.y,
                     dz = double(p.z) - q.z;
        const auto candidate = std::pair{dx * dx + dy * dy + dz * dz, side};
        const auto [entry, inserted] = splitOwners.emplace(id, candidate);
        if (!inserted && candidate < entry->second)
          entry->second = candidate;
      }
    }
    std::vector<std::uint32_t> boundary;
    for (unsigned side = 0; side < 3; ++side) {
      boundary.push_back(triangle.vertices[side]);
      for (const auto &[t, id] : edgeSplits[side])
        if (splitOwners.at(id).second == side)
          boundary.push_back(id);
    }
    if (boundary.size() == 3) {
      writer.mesh.triangles.push_back(triangle);
      continue;
    }
    const auto a = writer.mesh.vertices[triangle.vertices[0]],
               b = writer.mesh.vertices[triangle.vertices[1]],
               c = writer.mesh.vertices[triangle.vertices[2]];
    const Point center{(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3,
                       (a.z + b.z + c.z) / 3};
    for (std::size_t i = 0; i < boundary.size(); ++i)
      writer.TriangleAt(
          center, writer.mesh.vertices[boundary[i]],
          writer.mesh.vertices[boundary[(i + 1) % boundary.size()]],
          triangle.flags, !triangle.source.Authored(), triangle.source);
    if (writer.invalid)
      return true;
  }
  return true;
}
// Collision and authored navmesh heights need not agree exactly. Conform only
// sampled boundary vertices on an authored boundary's XY segment, within the
// existing step limit. Never alter authored vertices or join separate layers.
bool Conform(Writer &writer, const std::function<bool()> &stop) {
  const auto &triangles = writer.mesh.triangles;
  auto &vertices = writer.mesh.vertices;
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>>
      edges;
  std::vector<bool> authored(vertices.size());
  std::vector<std::vector<std::uint32_t>> incident(vertices.size());
  for (std::uint32_t i = 0; i < triangles.size(); ++i) {
    const auto &triangle = triangles[i];
    for (unsigned edge = 0; edge < 3; ++edge) {
      const auto a = triangle.vertices[edge];
      const auto b = triangle.vertices[(edge + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}].push_back(i);
      incident[a].push_back(i);
      authored[a] = authored[a] || triangle.source.Authored();
    }
  }
  std::vector<std::pair<Point, Point>> boundaries;
  std::set<std::uint32_t> sampled;
  for (const auto &[edge, faces] : edges) {
    if (faces.size() != 1)
      continue;
    if (triangles[faces.front()].source.Authored())
      boundaries.emplace_back(vertices[edge.first], vertices[edge.second]);
    else {
      sampled.insert(edge.first);
      sampled.insert(edge.second);
    }
  }
  std::vector<XYBounds> boundaryBounds;
  boundaryBounds.reserve(boundaries.size());
  for (const auto &[a, b] : boundaries)
    boundaryBounds.push_back({std::min(a.x, b.x), std::min(a.y, b.y),
                              std::max(a.x, b.x), std::max(a.y, b.y)});
  const XYBoundsIndex boundaryIndex(std::move(boundaryBounds));
  std::vector<std::uint32_t> candidates;
  for (const auto index : sampled) {
    if (stop())
      return false;
    if (authored[index])
      continue;
    const auto original = vertices[index];
    Point candidate{};
    float best = MAX_STEP + 1;
    bool ambiguous = false;
    if (!boundaryIndex.Query(
            {original.x - WELD_TOLERANCE, original.y - WELD_TOLERANCE,
             original.x + WELD_TOLERANCE, original.y + WELD_TOLERANCE},
            candidates, stop))
      return false;
    for (const auto boundary : candidates) {
      const auto &[a, b] = boundaries[boundary];
      const auto dx = b.x - a.x, dy = b.y - a.y;
      const auto length = dx * dx + dy * dy;
      if (length < .000001F)
        continue;
      const auto t = std::clamp(
          ((original.x - a.x) * dx + (original.y - a.y) * dy) / length, 0.0F,
          1.0F);
      const auto point = Mix(a, b, t);
      const auto x = point.x - original.x, y = point.y - original.y;
      const auto distance = std::abs(point.z - original.z);
      if (x * x + y * y > WELD_TOLERANCE * WELD_TOLERANCE ||
          distance > MAX_STEP)
        continue;
      if (distance + WELD_TOLERANCE < best) {
        candidate = point;
        best = distance;
        ambiguous = false;
      } else if (std::abs(distance - best) <= WELD_TOLERANCE &&
                 std::abs(point.z - candidate.z) > WELD_TOLERANCE)
        ambiguous = true;
    }
    if (best > MAX_STEP || ambiguous)
      continue;
    vertices[index] = candidate;
    bool valid = true;
    for (const auto face : incident[index]) {
      const auto &triangle = triangles[face];
      const auto a = vertices[triangle.vertices[0]];
      const auto b = vertices[triangle.vertices[1]];
      const auto c = vertices[triangle.vertices[2]];
      valid &= Walkable(a, b, c, writer.validatedWalkability);
    }
    if (!valid)
      vertices[index] = original;
  }
  // Rebuild the weld map after changing positions. Old quantized keys must not
  // survive and cause subsequent seam splitting to reuse the wrong vertex.
  Writer rebuilt;
  rebuilt.validatedWalkability = writer.validatedWalkability;
  for (const auto &triangle : triangles) {
    if (stop())
      return false;
    rebuilt.TriangleAt(vertices[triangle.vertices[0]],
                       vertices[triangle.vertices[1]],
                       vertices[triangle.vertices[2]], triangle.flags,
                       !triangle.source.Authored(), triangle.source);
  }
  writer = std::move(rebuilt);
  return true;
}
BuildResult Finish(Writer writer, const std::function<bool()> &stop,
                   std::uint64_t revision,
                   const std::vector<Point> &constraints = {},
                   bool repair = true) {
  BuildResult result;
  result.revision = revision;
  if (repair && !writer.invalid && !Conform(writer, stop)) {
    result.cancelled = true;
    return result;
  }
  for (const auto point : constraints)
    writer.boundaryConstraints.insert(writer.Add(point));
  if (repair && !writer.invalid && !Stitch(writer, stop)) {
    result.cancelled = true;
    return result;
  }
  if (writer.invalid) {
    result.error = writer.limitsExceeded
                       ? "Skyrim 16-bit navmesh limits exceeded"
                       : "Non-finite mesh vertex";
    return result;
  }
  struct Edge {
    std::uint32_t triangle;
    std::uint32_t side;
    bool linked{};
  };
  std::map<std::pair<std::uint32_t, std::uint32_t>, Edge> edges;
  for (std::uint32_t index = 0; index < writer.mesh.triangles.size(); ++index) {
    if (stop()) {
      result.cancelled = true;
      return result;
    }
    auto &triangle = writer.mesh.triangles[index];
    for (std::uint32_t side = 0; side < 3; ++side) {
      const auto a = triangle.vertices[side],
                 b = triangle.vertices[(side + 1) % 3];
      const auto key = std::pair{std::min(a, b), std::max(a, b)};
      const auto [found, inserted] = edges.emplace(key, Edge{index, side});
      if (!inserted) {
        if (found->second.linked) {
          std::ostringstream detail;
          detail << "Non-manifold mesh edge: faces " << found->second.triangle
                 << " / " << index << ", vertices " << a << " / " << b;
          result.error = detail.str();
          result.mesh = std::move(writer.mesh);
          return result;
        }
        auto &other = writer.mesh.triangles[found->second.triangle];
        if (other.vertices[found->second.side] != b) {
          const auto p = writer.mesh.vertices[a];
          const auto q = writer.mesh.vertices[b];
          std::ostringstream detail;
          detail << "Inconsistent triangle winding: faces "
                 << found->second.triangle << " / " << index << ", authored "
                 << other.source.Authored() << " / "
                 << triangle.source.Authored() << ", edge (" << p.x << ','
                 << p.y << ',' << p.z << ") -> (" << q.x << ',' << q.y << ','
                 << q.z << ')';
          result.error = detail.str();
          // Failed geometry is retained only for detached diagnostics. Valid()
          // still rejects it before ownership, native staging or publication.
          result.mesh = std::move(writer.mesh);
          return result;
        }
        triangle.neighbors[side] =
            static_cast<std::int32_t>(found->second.triangle);
        other.neighbors[found->second.side] = static_cast<std::int32_t>(index);
        found->second.linked = true;
      }
    }
  }
  if (writer.mesh.vertices.size() > 65535 ||
      writer.mesh.triangles.size() > 65535) {
    result.error = "Skyrim 16-bit navmesh limits exceeded";
    return result;
  }
  result.mesh = std::move(writer.mesh);
  return result;
}
} // namespace
FootprintPlan PlanFootprints(const std::vector<BuildInput::Bounds> &objects,
                             const std::optional<BuildInput::Bounds> &cell,
                             float spacing, std::size_t maxQuads) {
  FootprintPlan result;
  auto valid = [](const BuildInput::Bounds &b) {
    return std::isfinite(b.left) && std::isfinite(b.bottom) &&
           std::isfinite(b.right) && std::isfinite(b.top) && b.left < b.right &&
           b.bottom < b.top;
  };
  if (!std::isfinite(spacing) || spacing <= 0 || (cell && !valid(*cell))) {
    result.error = "Invalid object footprint plan";
    return result;
  }
  // Deduplicate only the small grid cells touched by individual bounds.
  // Contiguous runs share storage; no enclosing rectangle fills empty gaps.
  std::set<std::pair<int, int>> quads;
  for (auto region : objects) {
    if (!valid(region)) {
      result.error = "Invalid object footprint bounds";
      return result;
    }
    if (cell) {
      region.left = std::max(region.left, cell->left);
      region.bottom = std::max(region.bottom, cell->bottom);
      region.right = std::min(region.right, cell->right);
      region.top = std::min(region.top, cell->top);
    }
    if (!valid(region))
      continue;
    if (std::abs(region.left / spacing) > 1000000 ||
        std::abs(region.right / spacing) > 1000000 ||
        std::abs(region.bottom / spacing) > 1000000 ||
        std::abs(region.top / spacing) > 1000000) {
      result.error = "Object footprint coordinates exceed grid limits";
      return result;
    }
    const int x0 = static_cast<int>(std::floor(region.left / spacing));
    const int x1 = static_cast<int>(std::ceil(region.right / spacing));
    const int y0 = static_cast<int>(std::floor(region.bottom / spacing));
    const int y1 = static_cast<int>(std::ceil(region.top / spacing));
    if (std::uint64_t(x1 - x0) * std::uint64_t(y1 - y0) > maxQuads) {
      result.error = "Navigation footprint exceeds planning budget";
      return result;
    }
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        quads.emplace(y, x);
        if (quads.size() > maxQuads) {
          result.error = "Navigation footprints exceed planning budget";
          return result;
        }
      }
  }
  struct Patch {
    float x{}, y{};
    unsigned width{}, height{};
  };
  std::vector<Patch> patches;
  std::map<std::pair<int, int>, std::size_t> runs;
  for (auto iterator = quads.begin(); iterator != quads.end();) {
    const auto [y, x] = *iterator++;
    int end = x + 1;
    while (iterator != quads.end() && iterator->first == y &&
           iterator->second == end && end - x < 64) {
      ++end;
      ++iterator;
    }
    const auto span = std::pair{x, end};
    const auto prior = runs.find(span);
    if (prior != runs.end()) {
      auto &grid = patches[prior->second];
      if (grid.height < 65 &&
          grid.y + (grid.height - 1) * spacing == y * spacing) {
        ++grid.height;
        continue;
      }
    }
    Patch grid;
    grid.x = x * spacing;
    grid.y = y * spacing;
    grid.width = static_cast<std::uint32_t>(end - x + 1);
    grid.height = 2;
    runs[span] = patches.size();
    patches.push_back(std::move(grid));
  }
  for (auto &grid : patches) {
    BuildInput::Bounds footprint{grid.x, grid.y,
                                 grid.x + (grid.width - 1) * spacing,
                                 grid.y + (grid.height - 1) * spacing};
    if (cell) {
      footprint.left = std::max(footprint.left, cell->left);
      footprint.bottom = std::max(footprint.bottom, cell->bottom);
      footprint.right = std::min(footprint.right, cell->right);
      footprint.top = std::min(footprint.top, cell->top);
    }
    result.regions.push_back(footprint);
  }
  return result;
}
std::optional<Point> BaselineTriangleSurface(const Mesh &mesh,
                                             const Triangle &face, float x,
                                             float y, float low, float high) {
  if (face.flags & (8U | 32U))
    return std::nullopt;
  if (std::ranges::any_of(face.vertices,
                          [&](auto i) { return i >= mesh.vertices.size(); }))
    return std::nullopt;
  const auto a = mesh.vertices[face.vertices[0]],
             b = mesh.vertices[face.vertices[1]],
             c = mesh.vertices[face.vertices[2]];
  const double bx = double(b.x) - a.x, by = double(b.y) - a.y,
               cx = double(c.x) - a.x, cy = double(c.y) - a.y;
  const double determinant = bx * cy - by * cx;
  if (std::abs(determinant) < .000001)
    return std::nullopt;
  const double px = double(x) - a.x, py = double(y) - a.y;
  const double u = (px * cy - py * cx) / determinant,
               v = (bx * py - by * px) / determinant;
  if (u < -0.000001 || v < -0.000001 || u + v > 1.000001)
    return std::nullopt;
  const float z = static_cast<float>(a.z + u * (b.z - a.z) + v * (c.z - a.z));
  if (std::isfinite(z) && z >= low && z <= high)
    return Point{x, y, z};
  return std::nullopt;
}
std::optional<Point>
BaselineSurface(const std::vector<std::shared_ptr<const Baseline>> &originals,
                float x, float y, float low, float high) {
  std::optional<Point> result;
  for (const auto &baseline : originals) {
    if (!baseline)
      continue;
    for (const auto &face : baseline->mesh.triangles) {
      const auto point =
          BaselineTriangleSurface(baseline->mesh, face, x, y, low, high);
      if (point && (!result || point->z > result->z))
        result = point;
    }
  }
  return result;
}
BuildResult Build(const BuildInput &input,
                  const std::function<bool()> &cancelled,
                  const ReplacementGenerator &generator,
                  const std::function<void(unsigned)> &progress) {
  BuildResult result;
  result.revision = input.revision;
  Writer writer;
  const auto stop = [&] { return cancelled && cancelled(); };
  if (input.sampledBounds) {
    const auto &b = *input.sampledBounds;
    if (!std::isfinite(b.left) || !std::isfinite(b.bottom) ||
        !std::isfinite(b.right) || !std::isfinite(b.top) || b.left >= b.right ||
        b.bottom >= b.top) {
      result.error = "Invalid sampled cell bounds";
      return result;
    }
  }
  auto replacements = input.footprints;
  if (replacements.size() > 8192 ||
      std::ranges::any_of(replacements, [](const auto &b) {
        return !std::isfinite(b.left) || !std::isfinite(b.bottom) ||
               !std::isfinite(b.right) || !std::isfinite(b.top) ||
               b.left >= b.right || b.bottom >= b.top;
      })) {
    result.error = "Invalid replacement footprints";
    return result;
  }
  for (const auto &original : input.originals) {
    if (!original) {
      result.error = "Missing immutable baseline";
      return result;
    }
    for (std::size_t sourceIndex = 0;
         sourceIndex < original->mesh.triangles.size(); ++sourceIndex) {
      const auto &triangle = original->mesh.triangles[sourceIndex];
      if (stop()) {
        result.cancelled = true;
        return result;
      }
      // Native navcuts retain replaced faces with the overlapping flag. They
      // are not active surfaces and must not be regenerated into walkable area.
      if (triangle.flags & (8U | 32U))
        continue;
      if (std::ranges::any_of(triangle.vertices, [&](auto index) {
            return index >= original->mesh.vertices.size();
          })) {
        result.error = "Invalid baseline triangle index";
        return result;
      }
      std::vector<Polygon> polygons{
          {original->mesh.vertices[triangle.vertices[0]],
           original->mesh.vertices[triangle.vertices[1]],
           original->mesh.vertices[triangle.vertices[2]]}};
      for (const auto &rectangle : replacements) {
        std::vector<Polygon> retained;
        for (auto &polygon : polygons) {
          if (stop()) {
            result.cancelled = true;
            return result;
          }
          auto pieces =
              Subtract(std::move(polygon), rectangle, input.sampledBounds);
          for (auto &piece : pieces)
            retained.push_back(std::move(piece));
        }
        polygons = std::move(retained);
      }
      for (const auto &polygon : polygons)
        for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
          writer.TriangleAt(
              polygon[0], polygon[i], polygon[i + 1], triangle.flags, false,
              {original->formID, static_cast<std::uint32_t>(sourceIndex)});
    }
  }
  if (progress)
    progress(1); // Authored footprint clipping completed.
  auto generated = generator ? generator(input, cancelled)
                             : GenerateRecast(input, cancelled);
  writer.validatedWalkability = generated.validatedWalkability;
  if (!generated.Valid()) {
    generated.revision = input.revision;
    return generated;
  }
  if (progress)
    progress(2); // Replacement backend completed.
  const auto replacement = std::move(generated.mesh);
  for (const auto &face : replacement.triangles) {
    if (stop()) {
      result.cancelled = true;
      return result;
    }
    if (face.source.Authored() ||
        std::ranges::any_of(face.vertices, [&](auto v) {
          return v >= replacement.vertices.size();
        })) {
      result.error = "Invalid sampled backend output";
      return result;
    }
    Geometry::Polygon polygon{replacement.vertices[face.vertices[0]],
                              replacement.vertices[face.vertices[1]],
                              replacement.vertices[face.vertices[2]]};
    if (input.sampledBounds) {
      const auto &b = *input.sampledBounds;
      polygon = Clip(polygon, 0, b.left, true);
      polygon = Clip(polygon, 0, b.right, false);
      polygon = Clip(polygon, 1, b.bottom, true);
      polygon = Clip(polygon, 1, b.top, false);
    }
    const auto emit = [&](const Polygon &piece) {
      for (std::size_t i = 1; i + 1 < piece.size(); ++i)
        writer.TriangleAt(piece[0], piece[i], piece[i + 1], face.flags, true);
    };
    if (input.footprints.empty())
      emit(polygon);
    else
      for (const auto &r : input.footprints) {
        auto piece = Clip(polygon, 0, r.left, true);
        piece = Clip(piece, 0, r.right, false);
        piece = Clip(piece, 1, r.bottom, true);
        piece = Clip(piece, 1, r.top, false);
        emit(piece);
      }
  }
  if (progress)
    progress(3); // Generated geometry merged.
  auto finished = Finish(std::move(writer), stop, input.revision);
  if (finished.Valid() && progress)
    progress(4);
  finished.sourceGeometry = std::move(generated.sourceGeometry);
  return finished;
}
BuildResult RepairSampledSeams(const Mesh &mesh,
                               const std::function<bool()> &cancelled,
                               const std::vector<Point> &constraints) {
  Writer writer;
  writer.validatedWalkability = true;
  // Cached/generated meshes have already passed acquisition validation.
  // Their stair flights must survive boundary reconstruction unchanged.
  const auto stop = [&] { return cancelled && cancelled(); };
  for (const auto &triangle : mesh.triangles) {
    if (std::ranges::any_of(triangle.vertices, [&](auto index) {
          return index >= mesh.vertices.size();
        })) {
      BuildResult result;
      result.error = "Invalid replay vertex index";
      return result;
    }
    writer.TriangleAt(mesh.vertices[triangle.vertices[0]],
                      mesh.vertices[triangle.vertices[1]],
                      mesh.vertices[triangle.vertices[2]], triangle.flags,
                      !triangle.source.Authored(), triangle.source);
  }
  return Finish(std::move(writer), stop, 0, constraints);
}
BuildResult RefineGeneratedBoundaries(const Mesh &mesh,
                                      const std::vector<Point> &constraints) {
  Writer writer;
  writer.mesh.vertices = mesh.vertices;
  auto fail = [](const char *error) {
    BuildResult result;
    result.error = error;
    return result;
  };
  if (mesh.vertices.size() > 65535 || mesh.triangles.size() > 65535)
    return fail("Skyrim 16-bit navmesh limits exceeded");
  for (const auto &p : mesh.vertices)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      return fail("Non-finite cell boundary vertex");
  for (const auto &p : constraints)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      return fail("Non-finite cell boundary constraint");
  for (const auto &face : mesh.triangles) {
    for (const auto index : face.vertices)
      if (index >= mesh.vertices.size())
        return fail("Invalid cell boundary vertex index");
    std::vector<Triangle> pieces{face};
    if (!face.source.Authored())
      for (unsigned side = 0; side < 3; ++side) {
        if (face.neighbors[side] >= 0)
          continue;
        const auto ai = face.vertices[side], bi = face.vertices[(side + 1) % 3];
        const auto a = mesh.vertices[ai], b = mesh.vertices[bi];
        const double dx = double(b.x) - a.x, dy = double(b.y) - a.y,
                     dz = double(b.z) - a.z;
        const auto length = dx * dx + dy * dy + dz * dz;
        if (length <= 0)
          return fail("Degenerate cell boundary edge");
        std::map<double, Point> cuts;
        for (auto p : constraints) {
          const double t =
              ((double(p.x) - a.x) * dx + (double(p.y) - a.y) * dy +
               (double(p.z) - a.z) * dz) /
              length;
          if (!(t > 0 && t < 1))
            continue;
          const auto ex = double(p.x) - (a.x + t * dx),
                     ey = double(p.y) - (a.y + t * dy),
                     ez = double(p.z) - (a.z + t * dz);
          if (ex * ex + ey * ey + ez * ez > .000001)
            continue;
          cuts.try_emplace(t, p);
        }
        auto first = ai;
        for (const auto &[t, p] : cuts) {
          // This vertex belongs to this explicit boundary split. Coincident
          // vertices elsewhere do not authorize connecting their surfaces.
          if (writer.mesh.vertices.size() >= 65535)
            return fail("Skyrim 16-bit navmesh limits exceeded");
          const auto cut =
              static_cast<std::uint32_t>(writer.mesh.vertices.size());
          writer.mesh.vertices.push_back(p);
          bool split = false;
          for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto before = pieces[i];
            for (unsigned edge = 0; edge < 3; ++edge) {
              if (before.vertices[edge] != first ||
                  before.vertices[(edge + 1) % 3] != bi)
                continue;
              const auto opposite = before.vertices[(edge + 2) % 3];
              const auto c = writer.mesh.vertices[opposite];
              if (!Walkable(writer.mesh.vertices[first], p, c, true) ||
                  !Walkable(p, writer.mesh.vertices[bi], c, true))
                return fail(
                    "Cell boundary constraint collapses a generated face");
              auto left = before, right = before;
              left.vertices[(edge + 1) % 3] = cut;
              right.vertices[edge] = cut;
              pieces[i] = left;
              pieces.push_back(right);
              split = true;
              break;
            }
            if (split)
              break;
          }
          if (!split)
            return fail("Cell boundary refinement lost its source edge");
          first = cut;
        }
      }
    for (auto piece : pieces) {
      piece.neighbors = {-1, -1, -1};
      writer.mesh.triangles.push_back(piece);
      if (writer.mesh.triangles.size() > 65535)
        return fail("Skyrim 16-bit navmesh limits exceeded");
    }
  }
  return Finish(std::move(writer), [] { return false; }, 0, {}, false);
}
} // namespace SkyBuild::Navmesh
