#include "navmesh/generation/recast/ConvexHull.h"
#include "navmesh/generation/recast/SceneGeometry.h"
#include "navmesh/generation/MeshGeometry.h"
#include "navmesh/generation/TerrainGeometry.h"
#include "navmesh/generation/recast/RecastBuilder.h"
#include <format>
#include <numbers>
#include <stdexcept>
namespace SkyBuild::Navmesh {
Point GeometryTransform::Apply(Point p) const {
  return {translation.x + columns[0].x * p.x + columns[1].x * p.y +
              columns[2].x * p.z,
          translation.y + columns[0].y * p.x + columns[1].y * p.y +
              columns[2].y * p.z,
          translation.z + columns[0].z * p.x + columns[1].z * p.y +
              columns[2].z * p.z};
}
namespace {
struct StopCapture {};
Point Sub(Point a, Point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point Cross(Point a, Point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(Point a, Point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Point Normal(Point p) {
  const auto len = std::sqrt(Dot(p, p));
  if (len < .000001F)
    throw std::runtime_error("Degenerate collision axis");
  return {p.x / len, p.y / len, p.z / len};
}
} // namespace
BuildResult DecodeSceneGeometry(const RecastSceneGeometry &scene,
                                const BuildInput &input,
                                const std::function<bool()> &cancelled) {
  BuildResult result;
  result.revision = input.revision;
  const auto stop = [&] {
    if (cancelled && cancelled())
      throw StopCapture{};
  };
  try {
    std::vector<BuildInput::Bounds> regions = input.footprints;
    if (regions.empty())
      return result;
    auto domain = regions.front();
    for (const auto &r : regions) {
      domain.left = std::min(domain.left, r.left);
      domain.bottom = std::min(domain.bottom, r.bottom);
      domain.right = std::max(domain.right, r.right);
      domain.top = std::max(domain.top, r.top);
    }
    // Context only. Output remains clipped to each individual footprint.
    constexpr float halo = RECAST_CONTEXT_HALO;
    domain.left -= halo;
    domain.bottom -= halo;
    domain.right += halo;
    domain.top += halo;
    const auto emit = [&](Point a, Point b, Point c, std::uint16_t flags = 0) {
      stop();
      for (const auto p : {a, b, c})
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          throw std::runtime_error("Non-finite collision vertex");
      Geometry::Polygon polygon{a, b, c};
      polygon = Geometry::Clip(polygon, 0, domain.left, true);
      polygon = Geometry::Clip(polygon, 0, domain.right, false);
      polygon = Geometry::Clip(polygon, 1, domain.bottom, true);
      polygon = Geometry::Clip(polygon, 1, domain.top, false);
      for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
        const auto normal =
            Cross(Sub(polygon[i], polygon[0]), Sub(polygon[i + 1], polygon[0]));
        if (Dot(normal, normal) < .000001F)
          continue;
        if (result.mesh.triangles.size() >= 262144)
          throw std::runtime_error("Recast source triangle budget exceeded");
        const auto base =
            static_cast<std::uint32_t>(result.mesh.vertices.size());
        result.mesh.vertices.insert(result.mesh.vertices.end(),
                                    {polygon[0], polygon[i], polygon[i + 1]});
        result.mesh.triangles.push_back(
            {{base, base + 1, base + 2}, {-1, -1, -1}, flags, {}});
      }
    };
    for (const auto &ground : scene.ground) {
      if (!ground)
        throw std::runtime_error("Missing Recast ground snapshot");
      for (const auto &t : ground->mesh.triangles) {
        if (t.flags & (8U | 32U))
          continue;
        for (const auto v : t.vertices)
          if (v >= ground->mesh.vertices.size())
            throw std::runtime_error("Invalid Recast ground vertex index");
        std::vector<Geometry::Polygon> pieces{
            {ground->mesh.vertices[t.vertices[0]],
             ground->mesh.vertices[t.vertices[1]],
             ground->mesh.vertices[t.vertices[2]]}};
        for (const auto &cell : scene.terrain) {
          std::vector<Geometry::Polygon> remaining;
          for (auto &piece : pieces) {
            auto retained = TerrainGroundRemainder(std::move(piece), cell);
            remaining.insert(remaining.end(),
                             std::make_move_iterator(retained.begin()),
                             std::make_move_iterator(retained.end()));
          }
          pieces = std::move(remaining);
        }
        for (const auto &piece : pieces)
          for (std::size_t i = 1; i + 1 < piece.size(); ++i)
            emit(piece[0], piece[i], piece[i + 1], t.flags & 64U);
      }
    }
    for (const auto &cell : scene.terrain)
      for (const auto &surface : cell.surfaces)
        if (surface) {
          const auto &mesh = surface->Geometry();
          for (const auto &triangle : mesh.triangles) {
            const auto point = [&](unsigned index) {
              const auto p = mesh.vertices[index].position;
              return Point{p.x, p.y, p.z};
            };
            emit(point(triangle[0]), point(triangle[1]), point(triangle[2]));
          }
        }

    for (const auto &shape : scene.collisions) {
      stop();
      const auto triangle = [&](Point a, Point b, Point c) {
        emit(shape.world.Apply(a), shape.world.Apply(b), shape.world.Apply(c));
      };
      if (shape.kind == CollisionGeometry::Kind::Compressed) {
        const auto &compressed = shape.compressed;
        if (!std::isfinite(compressed.error) || compressed.error <= 0)
          throw std::runtime_error("Invalid compressed collision quantization");
        const auto scale = compressed.scale;
        if (!std::isfinite(scale.x) || !std::isfinite(scale.y) ||
            !std::isfinite(scale.z) || scale.x <= 0 || scale.y <= 0 ||
            scale.z <= 0)
          throw std::runtime_error(
              "Invalid compressed collision instance scale");
        const auto transform = [&](Point p, std::uint16_t id) {
          if (id != 0xFFFF) {
            if (id >= compressed.transforms.size())
              throw std::runtime_error(
                  "Invalid compressed collision transform");
            p = compressed.transforms[id].Apply(p);
          }
          return Point{p.x * scale.x, p.y * scale.y, p.z * scale.z};
        };
        for (std::size_t i = 0; i < compressed.chunks.size(); ++i) {
          const auto &instance = compressed.chunks[i];
          auto source = i;
          for (std::size_t depth = 0;
               compressed.chunks[source].reference != 0xFFFF; ++depth) {
            source = compressed.chunks[source].reference;
            if (source >= compressed.chunks.size() ||
                depth >= compressed.chunks.size())
              throw std::runtime_error(
                  "Invalid or cyclic compressed collision reference");
          }
          const auto &chunk = compressed.chunks[source];
          if (chunk.vertices.size() % 3)
            throw std::runtime_error("Malformed compressed collision vertices");
          const auto vertex = [&](std::uint16_t v) {
            if (std::size_t(v) * 3 + 2 >= chunk.vertices.size())
              throw std::runtime_error("Invalid compressed collision index");
            const auto p = Point{
                chunk.offset.x + chunk.vertices[v * 3] * compressed.error,
                chunk.offset.y + chunk.vertices[v * 3 + 1] * compressed.error,
                chunk.offset.z + chunk.vertices[v * 3 + 2] * compressed.error};
            return transform(p, instance.transform);
          };
          std::size_t offset{};
          for (const auto length : chunk.strips) {
            if (length < 3 || offset + length > chunk.indices.size())
              throw std::runtime_error("Invalid compressed collision strip");
            for (std::size_t j = 0; j + 2 < length; ++j) {
              const auto a = vertex(chunk.indices[offset + j]),
                         b = vertex(chunk.indices[offset + j + 1]),
                         c = vertex(chunk.indices[offset + j + 2]);
              if (j & 1)
                triangle(b, a, c);
              else
                triangle(a, b, c);
            }
            offset += length;
          }
          if ((chunk.indices.size() - offset) % 3)
            throw std::runtime_error(
                "Malformed compressed collision triangle list");
          for (; offset < chunk.indices.size(); offset += 3)
            triangle(vertex(chunk.indices[offset]),
                     vertex(chunk.indices[offset + 1]),
                     vertex(chunk.indices[offset + 2]));
        }
        for (const auto &t : compressed.bigTriangles) {
          for (const auto v : t.indices)
            if (v >= compressed.bigVertices.size())
              throw std::runtime_error(
                  "Invalid large collision triangle index");
          triangle(
              transform(compressed.bigVertices[t.indices[0]], t.transform),
              transform(compressed.bigVertices[t.indices[1]], t.transform),
              transform(compressed.bigVertices[t.indices[2]], t.transform));
        }
      } else if (shape.kind == CollisionGeometry::Kind::Box) {
        const auto h = shape.halfExtents;
        const std::array<Point, 8> v{{{-h.x, -h.y, -h.z},
                                      {h.x, -h.y, -h.z},
                                      {h.x, h.y, -h.z},
                                      {-h.x, h.y, -h.z},
                                      {-h.x, -h.y, h.z},
                                      {h.x, -h.y, h.z},
                                      {h.x, h.y, h.z},
                                      {-h.x, h.y, h.z}}};
        for (const auto face :
             std::array<std::array<unsigned, 4>, 6>{{{0, 3, 2, 1},
                                                     {4, 5, 6, 7},
                                                     {0, 1, 5, 4},
                                                     {1, 2, 6, 5},
                                                     {2, 3, 7, 6},
                                                     {3, 0, 4, 7}}}) {
          triangle(v[face[0]], v[face[1]], v[face[2]]);
          triangle(v[face[0]], v[face[2]], v[face[3]]);
        }
      } else if (shape.kind == CollisionGeometry::Kind::Convex) {
        std::vector<std::array<std::size_t, 3>> faces;
        try {
          faces = ConvexCollisionHull(shape.vertices);
        } catch (const std::exception &problem) {
          throw std::runtime_error(std::format(
              "Reference {:08X}, convex collision ({} vertices): {}",
              shape.sourceFormID, shape.vertices.size(), problem.what()));
        }
        for (const auto &face : faces)
          triangle(shape.vertices[face[0]], shape.vertices[face[1]],
                   shape.vertices[face[2]]);
      } else {
        const bool sphere = shape.kind == CollisionGeometry::Kind::Sphere;
        const auto axis =
            sphere ? Point{0, 0, 1} : Normal(Sub(shape.endB, shape.endA));
        const auto u = Normal(Cross(axis, std::abs(axis.z) < .9F
                                              ? Point{0, 0, 1}
                                              : Point{0, 1, 0})),
                   v = Cross(axis, u);
        constexpr unsigned slices = 16, halfRings = 4;
        std::vector<std::array<Point, slices>> rings;
        for (unsigned cap = 0; cap < 2; ++cap)
          for (unsigned j = 0; j <= halfRings; ++j) {
            const float angle =
                (-.5F + float(cap) * .5F + float(j) / halfRings * .5F) *
                std::numbers::pi_v<float>;
            const auto center = cap ? shape.endB : shape.endA;
            std::array<Point, slices> ring;
            for (unsigned k = 0; k < slices; ++k) {
              const auto theta =
                  float(k) / slices * 2 * std::numbers::pi_v<float>;
              const auto radial = shape.radius * std::cos(angle),
                         height = shape.radius * std::sin(angle);
              ring[k] = {
                  center.x + axis.x * height +
                      radial * (u.x * std::cos(theta) + v.x * std::sin(theta)),
                  center.y + axis.y * height +
                      radial * (u.y * std::cos(theta) + v.y * std::sin(theta)),
                  center.z + axis.z * height +
                      radial * (u.z * std::cos(theta) + v.z * std::sin(theta))};
            }
            rings.push_back(ring);
          }
        for (std::size_t j = 0; j + 1 < rings.size(); ++j)
          for (unsigned k = 0; k < slices; ++k) {
            const auto next = (k + 1) % slices;
            triangle(rings[j][k], rings[j][next], rings[j + 1][next]);
            triangle(rings[j][k], rings[j + 1][next], rings[j + 1][k]);
          }
      }
    }
  } catch (const StopCapture &) {
    result.cancelled = true;
    result.mesh = {};
  } catch (const std::exception &error) {
    result.error = error.what();
    result.mesh = {};
  }
  return result;
}
} // namespace SkyBuild::Navmesh
