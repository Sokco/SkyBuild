#include "physics/PlacementClearance.h"
#include "physics/PlacementShapeMath.h"
#include "RE/H/hkpCdPoint.h"
#include "RE/H/hkpCdPointCollector.h"
#include "RE/H/hkpLinearCastInput.h"
#include "RE/H/hkAabb.h"
#include <array>
#include <limits>
#include <set>
// CommonLib declares this callback interface but supplies no definitions.
namespace RE {
hkpCdPointCollector::~hkpCdPointCollector() = default;
void hkpCdPointCollector::Reset() {
  earlyOutDistance = std::numeric_limits<float>::max();
}
} // namespace RE
namespace SkyBuild::Physics {
namespace {
const RE::hkpCdBody *Root(const RE::hkpCdBody *body) {
  while (body && body->parent)
    body = body->parent;
  return body;
}
bool AcceptContact(const RE::hkpCdPoint &point, const RE::hkpCollidable *query,
                   RE::TESObjectREFR *ignore) {
  const auto a = Root(point.cdBodyA), b = Root(point.cdBodyB);
  const auto body = a == query ? b : b == query ? a : nullptr;
  if (!body)
    return false;
  const auto collidable = static_cast<const RE::hkpCollidable *>(body);
  const auto layer = collidable->GetCollisionLayer();
  if (layer == RE::COL_LAYER::kBiped ||
      layer == RE::COL_LAYER::kCharController ||
      layer == RE::COL_LAYER::kDeadBip || layer == RE::COL_LAYER::kBipedNoCC ||
      layer == RE::COL_LAYER::kTransparent)
    return false;
  const auto ref = RE::TESHavokUtilities::FindCollidableRef(*collidable);
  return !ref || (ref != ignore && !ref->IsActor());
}
class Contacts final : public RE::hkpCdPointCollector {
  RE::TESObjectREFR *ignore;
  float inverseScale;

public:
  const RE::hkpCollidable *query = nullptr;
  std::vector<PlacementContact> values;
  Contacts(RE::TESObjectREFR *ignore, float inverseScale)
      : ignore(ignore), inverseScale(inverseScale) {
    Reset();
  }
  void AddCdPoint(const RE::hkpCdPoint &point) override {
    if (!AcceptContact(point, query, ignore))
      return;
    alignas(16) float n[4];
    _mm_store_ps(n, point.contact.separatingNormal.quad);
    const float sign = Root(point.cdBodyA) == query ? 1.F : -1.F;
    if (std::isfinite(n[3]) && n[3] < 0)
      values.push_back(
          {{n[0] * sign, n[1] * sign, n[2] * sign}, -n[3] * inverseScale});
  }
};
class CastContacts final : public RE::hkpCdPointCollector {
  RE::TESObjectREFR *ignore;
  RE::NiPoint3 direction;

public:
  const RE::hkpCollidable *query = nullptr;
  float fraction = 1.F;
  bool valid = true;
  CastContacts(RE::TESObjectREFR *ignored, RE::NiPoint3 travel)
      : ignore(ignored), direction(travel) {
    const float length = std::sqrt(travel.x * travel.x + travel.y * travel.y +
                                   travel.z * travel.z);
    if (length > 0)
      direction = travel * (1.F / length);
    Reset();
  }
  void AddCdPoint(const RE::hkpCdPoint &point) override {
    if (!AcceptContact(point, query, ignore))
      return;
    alignas(16) float n[4];
    _mm_store_ps(n, point.contact.separatingNormal.quad);
    const float sign = Root(point.cdBodyA) == query ? 1.F : -1.F;
    const float approach =
        sign * (n[0] * direction.x + n[1] * direction.y + n[2] * direction.z);
    // Existing support contact blocks inward travel, not motion tangent to
    // the surface or away from it. Otherwise a floor contact freezes sliding.
    if (std::isfinite(approach) && approach >= -.000001F)
      return;
    if (!std::isfinite(n[3]) || n[3] < 0 || n[3] > 1)
      valid = false;
    else
      fraction = std::min(fraction, n[3]);
  }
};
} // namespace
PlacementShapes CapturePlacementShapes(RE::NiAVObject *model) {
  PlacementShapes result;
  std::set<RE::bhkRigidBody *> seen;
  const auto point = [](const RE::hkVector4 &value) {
    alignas(16) float v[4];
    _mm_store_ps(v, value.quad);
    return RE::NiPoint3{v[0], v[1], v[2]};
  };
  const auto visit = [&](auto &&self, RE::NiAVObject *node,
                         const RE::NiTransform &relative) -> void {
    if (!node)
      return;
    const auto collision = node->collisionObject
                               ? node->collisionObject->AsBhkNiCollisionObject()
                               : nullptr;
    const auto body = collision && collision->body
                          ? collision->body->AsBhkRigidBody()
                          : nullptr;
    if (body && body->GetRigidBody() && body->GetRigidBody()->GetShape() &&
        seen.insert(body).second) {
      const auto convert = [&](const RE::hkTransform &source) {
        RE::NiTransform pose;
        const auto x = point(source.rotation.col0),
                   y = point(source.rotation.col1),
                   z = point(source.rotation.col2);
        pose.rotate.entry[0][0] = x.x;
        pose.rotate.entry[1][0] = x.y;
        pose.rotate.entry[2][0] = x.z;
        pose.rotate.entry[0][1] = y.x;
        pose.rotate.entry[1][1] = y.y;
        pose.rotate.entry[2][1] = y.z;
        pose.rotate.entry[0][2] = z.x;
        pose.rotate.entry[1][2] = z.y;
        pose.rotate.entry[2][2] = z.z;
        pose.translate =
            point(source.translation) * (1.F / RE::bhkWorld::GetWorldScale());
        pose.scale = 1.F;
        return pose;
      };
      // The T wrapper exposes graphics pose G; motion exposes physical pose P.
      // G^-1 P preserves the collider offset while removing cached world pose.
      RE::hkTransform graphics;
      body->GetTransform(graphics);
      const auto physical = body->GetRigidBody()->GetMotionState()->transform;
      result.push_back(
          {RE::NiPointer<RE::bhkRigidBody>(body),
           ColliderLocalPose(relative, convert(graphics), convert(physical))});
    }
    if (const auto group = node->AsNode())
      for (const auto &child : group->GetChildren())
        if (child)
          self(self, child.get(), relative * child->local);
  };
  // The preview replaces the model root pose; only descendant local transforms
  // and the body's own (including bhkRigidBodyT) pose belong in the query.
  visit(visit, model, RE::NiTransform{});
  return result;
}
bool CanQueryShapes(const PlacementShapes &shapes, float scale) {
  return REL::Module::get().version() == REL::Version(1, 6, 1170, 0) &&
         !shapes.empty() && std::abs(scale - 1.F) < .00001F &&
         std::all_of(
             shapes.begin(), shapes.end(),
             [](const auto &part) {
               return part.owner && part.owner->GetRigidBody() &&
                      part.owner->GetRigidBody()->GetShape() &&
                      std::abs(part.local.scale - 1.F) < .00001F;
             });
}
std::optional<std::array<RE::NiPoint3, 2>>
PlacementShapeBounds(const PlacementShapes &shapes) {
  if (!CanQueryShapes(shapes, 1.F))
    return {};
  std::optional<std::array<RE::NiPoint3, 2>> result;
  const float scale = RE::bhkWorld::GetWorldScale();
  for (const auto &part : shapes) {
    RE::hkTransform transform;
    transform.rotation.col0 = part.local.rotate.GetVectorX();
    transform.rotation.col1 = part.local.rotate.GetVectorY();
    transform.rotation.col2 = part.local.rotate.GetVectorZ();
    transform.translation = part.local.translate * scale;
    RE::hkAabb aabb;
    part.owner->GetRigidBody()->GetShape()->GetAabbImpl(transform, 0.F, aabb);
    alignas(16) float lo[4], hi[4];
    _mm_store_ps(lo, aabb.min.quad);
    _mm_store_ps(hi, aabb.max.quad);
    const RE::NiPoint3 min{lo[0] / scale, lo[1] / scale, lo[2] / scale};
    const RE::NiPoint3 max{hi[0] / scale, hi[1] / scale, hi[2] / scale};
    if (!std::isfinite(min.x) || !std::isfinite(min.y) ||
        !std::isfinite(min.z) || !std::isfinite(max.x) ||
        !std::isfinite(max.y) || !std::isfinite(max.z))
      return {};
    if (!result)
      result = std::array{min, max};
    else
      for (unsigned axis = 0; axis < 3; ++axis) {
        // NiPoint3's indexing follows x,y,z.
        (*result)[0][axis] = std::min((*result)[0][axis], min[axis]);
        (*result)[1][axis] = std::max((*result)[1][axis], max[axis]);
      }
  }
  return result;
}
std::vector<PlacementContact> ShapeOverlaps(RE::bhkWorld *world,
                                            const PlacementShapes &shapes,
                                            const RE::NiPoint3 &position,
                                            const RE::NiMatrix3 &rotation,
                                            RE::TESObjectREFR *ignore) {
  if (!world || REL::Module::get().version() != REL::Version(1, 6, 1170, 0))
    return {};
  const auto native = world->GetWorld1();
  if (!native)
    return {};
  const float scale = RE::bhkWorld::GetWorldScale();
  Contacts starts(ignore, 1.F / scale), sweep(ignore, 1.F / scale);
  const RE::BSReadLockGuard lock(world->worldLock);
  for (const auto &part : shapes) {
    const auto source = part.owner->GetRigidBody();
    if (!source || !source->GetShape())
      continue;
    const auto worldRotation = rotation * part.local.rotate;
    const auto center = position + rotation * part.local.translate;
    RE::hkTransform transform;
    transform.rotation.col0 = worldRotation.GetVectorX();
    transform.rotation.col1 = worldRotation.GetVectorY();
    transform.rotation.col2 = worldRotation.GetVectorZ();
    transform.translation = center * scale;
    RE::hkpCollidable query{};
    query.shape = source->GetShape();
    query.shapeKey = RE::HK_INVALID_SHAPE_KEY;
    query.motion = &transform;
    query.broadPhaseHandle.type = 1;
    query.broadPhaseHandle.ownerOffset = -0x24;
    query.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(
        RE::COL_LAYER::kProjectile);
    RE::hkpLinearCastInput input{};
    input.to = (center + RE::NiPoint3{0, 0, .001F}) * scale;
    input.startPointTolerance = .05F * scale;
    starts.query = &query;
    sweep.query = &query;
    native->LinearCast(&query, input, sweep, &starts);
  }
  return std::move(starts.values);
}
std::optional<float> ShapeCast(RE::bhkWorld *world,
                               const PlacementShapes &shapes,
                               const RE::NiPoint3 &from, const RE::NiPoint3 &to,
                               const RE::NiMatrix3 &rotation,
                               RE::TESObjectREFR *ignore) {
  if (!world || !CanQueryShapes(shapes, 1.F))
    return {};
  const auto native = world->GetWorld1();
  if (!native)
    return {};
  const float scale = RE::bhkWorld::GetWorldScale();
  Contacts starts(ignore, 1.F / scale);
  CastContacts sweep(ignore, to - from);
  const RE::BSReadLockGuard lock(world->worldLock);
  for (const auto &part : shapes) {
    const auto worldRotation = rotation * part.local.rotate;
    const auto offset = rotation * part.local.translate;
    RE::hkTransform transform;
    transform.rotation.col0 = worldRotation.GetVectorX();
    transform.rotation.col1 = worldRotation.GetVectorY();
    transform.rotation.col2 = worldRotation.GetVectorZ();
    transform.translation = (from + offset) * scale;
    RE::hkpCollidable query{};
    query.shape = part.owner->GetRigidBody()->GetShape();
    query.shapeKey = RE::HK_INVALID_SHAPE_KEY;
    query.motion = &transform;
    query.broadPhaseHandle.type = 1;
    query.broadPhaseHandle.ownerOffset = -0x24;
    query.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(
        RE::COL_LAYER::kProjectile);
    RE::hkpLinearCastInput input{};
    input.to = (to + offset) * scale;
    input.startPointTolerance = .05F * scale;
    starts.query = &query;
    sweep.query = &query;
    native->LinearCast(&query, input, sweep, &starts);
  }
  if (!sweep.valid)
    return {};
  for (const auto &contact : starts.values)
    if (contact.depth > .05F)
      return {};
  return sweep.fraction;
}
std::vector<PlacementContact> BoxOverlaps(RE::bhkWorld *world,
                                          const RE::NiPoint3 &center,
                                          const RE::NiPoint3 &halfExtents,
                                          const RE::NiMatrix3 &rotation,
                                          RE::TESObjectREFR *ignore) {
  // Constructor and callback ABI verified against the pinned 1.6.1170 image.
  // Unknown runtimes retain ordinary placement rather than calling an
  // unverified ABI.
  if (!world || REL::Module::get().version() != REL::Version(1, 6, 1170, 0))
    return {};
  auto *native = world->GetWorld1();
  if (!native)
    return {};
  const float scale = RE::bhkWorld::GetWorldScale();
  alignas(16) std::array<std::byte, 0x40> storage{};
  RE::hkVector4 half;
  half = halfExtents * scale;
  // Native box constructor: this, half extents, convex radius. The box owns no
  // auxiliary allocations, and its stack storage is never reference-released.
  const REL::Relocation<void *(*)(void *, const RE::hkVector4 *, float)>
      construct{REL::ID(60287)};
  construct(storage.data(), &half, 0.F);
  RE::hkTransform transform{};
  transform.rotation.col0 = rotation.GetVectorX();
  transform.rotation.col1 = rotation.GetVectorY();
  transform.rotation.col2 = rotation.GetVectorZ();
  transform.translation = center * scale;
  RE::hkpCollidable box{};
  box.shape = reinterpret_cast<const RE::hkpShape *>(storage.data());
  box.shapeKey = RE::HK_INVALID_SHAPE_KEY;
  box.motion = &transform;
  box.broadPhaseHandle.type = 1;
  box.broadPhaseHandle.ownerOffset = -0x24;
  box.broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(
      RE::COL_LAYER::kProjectile);
  RE::hkpLinearCastInput input{};
  input.to = (center + RE::NiPoint3{0, 0, .001F}) * scale;
  input.startPointTolerance = .05F * scale;
  Contacts starts(ignore, 1.F / scale), sweep(ignore, 1.F / scale);
  starts.query = &box;
  sweep.query = &box;
  const RE::BSReadLockGuard lock(world->worldLock);
  native->LinearCast(&box, input, sweep, &starts);
  return std::move(starts.values);
}
} // namespace SkyBuild::Physics
