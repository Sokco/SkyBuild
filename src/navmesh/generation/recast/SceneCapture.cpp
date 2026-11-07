#include "navmesh/generation/recast/SceneCapture.h"
#include <set>
#include <cstring>
#include <stdexcept>
namespace SkyBuild::Navmesh {
namespace {
Point Vec(const RE::hkVector4 &v) {
  float p[4];
  _mm_storeu_ps(p, v.quad);
  return {p[0], p[1], p[2]};
}
std::array<float, 4> Four(const RE::hkVector4 &v) {
  std::array<float, 4> p;
  _mm_storeu_ps(p.data(), v.quad);
  return p;
}
GeometryTransform Transform(const RE::hkQsTransform &value) {
  const auto q = Four(value.rotation.vec);
  const auto s = Vec(value.scale);
  const auto x = q[0], y = q[1], z = q[2], w = q[3];
  GeometryTransform t;
  t.translation = Vec(value.translation);
  t.columns = {{{(1 - 2 * (y * y + z * z)) * s.x, 2 * (x * y + w * z) * s.x,
                 2 * (x * z - w * y) * s.x},
                {2 * (x * y - w * z) * s.y, (1 - 2 * (x * x + z * z)) * s.y,
                 2 * (y * z + w * x) * s.y},
                {2 * (x * z + w * y) * s.z, 2 * (y * z - w * x) * s.z,
                 (1 - 2 * (x * x + y * y)) * s.z}}};
  return t;
}
template <class T>
std::vector<T> Copy(const RE::hkArray<T> &source, std::size_t max = 1000000) {
  if (source.size() < 0 || static_cast<std::size_t>(source.size()) > max ||
      (source.size() && !source.data()))
    throw std::runtime_error("Invalid or oversized collision array");
  if (!source.size())
    return {};
  return {source.data(), source.data() + source.size()};
}
void Shape(const RE::hkpShape *shape, const GeometryTransform &world,
           RecastSceneGeometry &scene, unsigned depth = 0) {
  if (!shape)
    throw std::runtime_error("Missing collision shape");
  if (depth > 32 || scene.collisions.size() > 4096)
    throw std::runtime_error("Collision shape capture budget exceeded");
  CollisionGeometry value;
  value.world = world;
  const auto account = [&](std::size_t bytes) {
    if (bytes > 64 * 1024 * 1024 ||
        scene.capturedBytes > 64 * 1024 * 1024 - bytes)
      throw std::runtime_error(
          "Recast collision snapshot exceeds 64 MiB budget");
    scene.capturedBytes += bytes;
  };
  switch (shape->type) {
  case RE::hkpShapeType::kMOPP:
    Shape(static_cast<const RE::hkpMoppBvTreeShape *>(shape)->child.childShape,
          world, scene, depth + 1);
    return;
  case RE::hkpShapeType::kList: {
    const auto list = static_cast<const RE::hkpListShape *>(shape);
    const auto count = list->childInfo.size();
    if (count < 0 || count > 4096)
      throw std::runtime_error("Invalid collision list size");
    std::set<RE::hkpShapeKey> keys;
    for (auto key = list->GetFirstKey(); key != RE::HK_INVALID_SHAPE_KEY;
         key = list->GetNextKey(key)) {
      if (key >= static_cast<unsigned>(count) || !keys.insert(key).second)
        throw std::runtime_error("Invalid collision list key");
      const auto layer =
          list->childInfo[key].collisionFilterInfo.GetCollisionLayer();
      if (layer != RE::COL_LAYER::kUnidentified &&
          layer != RE::COL_LAYER::kStatic && layer != RE::COL_LAYER::kClutter &&
          layer != RE::COL_LAYER::kProps &&
          layer != RE::COL_LAYER::kAnimStatic &&
          layer != RE::COL_LAYER::kStairHelper)
        continue;
      Shape(list->childInfo[key].shape, world, scene, depth + 1);
    }
    return;
  }
  case RE::hkpShapeType::kCompressedMesh: {
    value.kind = CollisionGeometry::Kind::Compressed;
    const auto mesh = static_cast<const RE::hkpCompressedMeshShape *>(shape);
    if (!mesh->convexPieces.empty())
      throw std::runtime_error(
          "Compressed convex pieces are not supported by Recast capture yet");
    auto &out = value.compressed;
    // Skyrim extends hkpCompressedMeshShape with an hkVector4 at +0x120.
    // CommonLib ends at +0x120. Native container GetChildShape (ID 80699)
    // multiplies transformed vertices by this vector before the body pose.
    // Verified on 1.6.1170 and 1.7.104; do not substitute reference scale,
    // which need not equal the collision instance's scale.
    static_assert(sizeof(RE::hkpCompressedMeshShape) == 0x120);
    RE::hkVector4 instanceScale;
    std::memcpy(&instanceScale,
                reinterpret_cast<const std::byte *>(mesh) + sizeof(*mesh),
                sizeof(instanceScale));
    out.scale = Vec(instanceScale);
    out.error = mesh->error;
    account(std::size_t(mesh->transforms.size()) * sizeof(GeometryTransform) +
            std::size_t(mesh->bigVertices.size()) * sizeof(Point) +
            std::size_t(mesh->bigTriangles.size()) *
                sizeof(CompressedGeometry::BigTriangle));
    for (const auto &t : Copy(mesh->transforms, 65535))
      out.transforms.push_back(Transform(t));
    for (const auto &v : Copy(mesh->bigVertices, 65535))
      out.bigVertices.push_back(Vec(v));
    for (const auto &t : Copy(mesh->bigTriangles, 262144))
      out.bigTriangles.push_back({{t.a, t.b, t.c}, t.transformIndex});
    for (const auto &c : Copy(mesh->chunks, 65535)) {
      account((std::size_t(c.vertices.size()) + std::size_t(c.indices.size()) +
               std::size_t(c.stripLengths.size())) *
              sizeof(std::uint16_t));
      out.chunks.push_back({Vec(c.offset), Copy(c.vertices), Copy(c.indices),
                            Copy(c.stripLengths), c.reference,
                            c.transformIndex});
    }
    break;
  }
  case RE::hkpShapeType::kBox:
    value.kind = CollisionGeometry::Kind::Box;
    value.halfExtents =
        Vec(static_cast<const RE::hkpBoxShape *>(shape)->halfExtents);
    break;
  case RE::hkpShapeType::kConvexVertices: {
    value.kind = CollisionGeometry::Kind::Convex;
    const auto convex = static_cast<const RE::hkpConvexVerticesShape *>(shape);
    if (convex->numVertices < 4 || convex->numVertices > 4096 ||
        convex->rotatedVertices.size() < (convex->numVertices + 3) / 4)
      throw std::runtime_error("Invalid convex collision vertex count");
    for (int i = 0; i < convex->numVertices; ++i) {
      const auto &batch = convex->rotatedVertices[i / 4];
      const auto x = Four(batch.x), y = Four(batch.y), z = Four(batch.z);
      value.vertices.push_back({x[i % 4], y[i % 4], z[i % 4]});
    }
    account(value.vertices.size() * sizeof(Point));
    break;
  }
  case RE::hkpShapeType::kSphere:
    value.kind = CollisionGeometry::Kind::Sphere;
    value.radius = static_cast<const RE::hkpSphereShape *>(shape)->radius;
    break;
  case RE::hkpShapeType::kCapsule: {
    value.kind = CollisionGeometry::Kind::Capsule;
    const auto capsule = static_cast<const RE::hkpCapsuleShape *>(shape);
    value.endA = Vec(capsule->vertexA);
    value.endB = Vec(capsule->vertexB);
    value.radius = capsule->radius;
    break;
  }
  default:
    throw std::runtime_error(
        std::format("Unsupported collision shape type {} for Recast",
                    static_cast<unsigned>(shape->type)));
  }
  scene.collisions.push_back(std::move(value));
}
} // namespace
bool CaptureRecastCollision(RE::TESObjectREFR *reference,
                            RecastSceneGeometry &scene, std::string &error,
                            const RE::hkpCollidable *selectedCollider) {
  try {
    const auto cell = reference ? reference->GetParentCell() : nullptr;
    const auto root = reference ? reference->Get3D() : nullptr;
    const auto world = cell ? cell->GetbhkWorld() : nullptr;
    if (!cell || !cell->IsAttached() || !root || !world)
      throw std::runtime_error("Recast object geometry is no longer loaded");
    std::vector<RE::NiAVObject *> nodes{root};
    std::set<const RE::hkpRigidBody *> bodies;
    std::size_t visited{};
    const RE::BSReadLockGuard lock(world->worldLock);
    const auto inverse = RE::bhkWorld::GetWorldScaleInverse();
    while (!nodes.empty()) {
      const auto object = nodes.back();
      nodes.pop_back();
      if (++visited > 65535)
        throw std::runtime_error("Recast scene-node budget exceeded");
      if (const auto collision =
              object->collisionObject
                  ? object->collisionObject->AsBhkNiCollisionObject()
                  : nullptr) {
        const auto wrapper =
            collision->body ? collision->body->AsBhkRigidBody() : nullptr;
        const auto body = wrapper ? static_cast<RE::hkpRigidBody *>(
                                        wrapper->referencedObject.get())
                                  : nullptr;
        if (body && bodies.insert(body).second) {
          const auto layer = body->collidable.GetCollisionLayer();
          if (selectedCollider ? body->GetCollidable() == selectedCollider
                               : (layer == RE::COL_LAYER::kStatic ||
                                  layer == RE::COL_LAYER::kClutter ||
                                  layer == RE::COL_LAYER::kProps ||
                                  layer == RE::COL_LAYER::kAnimStatic ||
                                  layer == RE::COL_LAYER::kStairHelper)) {
            const auto &pose = body->motion.motionState.transform;
            GeometryTransform transform;
            transform.columns = {Vec(pose.rotation.col0),
                                 Vec(pose.rotation.col1),
                                 Vec(pose.rotation.col2)};
            transform.translation = Vec(pose.translation);
            for (auto &p : transform.columns) {
              p.x *= inverse;
              p.y *= inverse;
              p.z *= inverse;
            }
            transform.translation.x *= inverse;
            transform.translation.y *= inverse;
            transform.translation.z *= inverse;
            const auto firstShape = scene.collisions.size();
            Shape(body->GetShape(), transform, scene);
            for (auto i = firstShape; i < scene.collisions.size(); ++i)
              scene.collisions[i].sourceFormID = reference->GetFormID();
          }
        }
      }
      if (const auto node = object->AsNode())
        for (const auto &child : node->GetChildren())
          if (child)
            nodes.push_back(child.get());
    }
    return true;
  } catch (const std::exception &problem) {
    error = std::format("Reference {:08X}: {}",
                        reference ? reference->GetFormID() : 0, problem.what());
    return false;
  }
}
} // namespace SkyBuild::Navmesh
