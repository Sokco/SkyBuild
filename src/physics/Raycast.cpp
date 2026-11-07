#include "models/VertexBounds.h"
#include "building/BuildingManager.h"
#include "building/PlaceableRegistry.h"
#include "physics/Raycast.h"
#include "physics/RayBox.h"
#include "physics/SelectionLayers.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace SkyBuild::Physics {
namespace {
[[nodiscard]]
RE::NiPoint3 Normalize(const RE::NiPoint3 &a_vector) {
  const float lengthSquared = a_vector.x * a_vector.x +
                              a_vector.y * a_vector.y + a_vector.z * a_vector.z;

  if (lengthSquared <= 0.000001F) {
    return {};
  }

  const float inverseLength = 1.0F / std::sqrt(lengthSquared);

  return {a_vector.x * inverseLength, a_vector.y * inverseLength,
          a_vector.z * inverseLength};
}

[[nodiscard]]
bool IsZero(const RE::NiPoint3 &a_vector) {
  return std::abs(a_vector.x) <= 0.000001F &&
         std::abs(a_vector.y) <= 0.000001F && std::abs(a_vector.z) <= 0.000001F;
}

[[nodiscard]]
RE::NiPoint3 GetCameraDirection(const RE::PlayerCamera *a_camera) {
  if (!a_camera || !a_camera->cameraRoot) {
    return {};
  }

  return Normalize(a_camera->cameraRoot->world.rotate.GetVectorY());
}

bool HasVisibleSelectionGeometry(RE::NiAVObject *root) {
  if (!root)
    return false;
  bool visible = false;
  RE::BSVisit::TraverseScenegraphGeometries(
      root, [&](RE::BSGeometry *geometry) {
        if (visible || !geometry)
          return RE::BSVisit::BSVisitControl::kContinue;
        // Effect-only references (sunbeams, fog, glow cards) must not
        // become solid targets merely because their NIF has bounds.
        const auto *shader = netimmerse_cast<RE::BSLightingShaderProperty *>(
            geometry->GetGeometryRuntimeData().shaderProperty.get());
        if (!shader)
          return RE::BSVisit::BSVisitControl::kContinue;
        for (auto *object = static_cast<RE::NiAVObject *>(geometry); object;
             object = object->parent) {
          if (object->GetAppCulled())
            return RE::BSVisit::BSVisitControl::kContinue;
          if (object == root)
            break;
        }
        visible = true;
        return RE::BSVisit::BSVisitControl::kContinue;
      });
  return visible;
}
} // namespace

RaycastResults CastRay(const RE::NiPoint3 &a_start,
                       const RE::NiPoint3 &a_direction, float a_maxDistance,
                       RE::COL_LAYER a_queryLayer, RE::Actor *a_actor,
                       bool a_resolveReferences) {
  RaycastResults results{};

  if (a_maxDistance <= 0.0F) {
    return results;
  }

  if (!a_actor) {
    a_actor = RE::PlayerCharacter::GetSingleton();
  }

  if (!a_actor) {
    return results;
  }

  const auto cell = a_actor->GetParentCell();

  if (!cell) {
    return results;
  }

  const auto world = cell->GetbhkWorld();

  if (!world) {
    return results;
  }

  const auto direction = Normalize(a_direction);

  if (IsZero(direction)) {
    return results;
  }

  const RE::NiPoint3 rayEnd = a_start + direction * a_maxDistance;

  const float havokWorldScale = RE::bhkWorld::GetWorldScale();

  RE::bhkPickData pickData{};

  pickData.rayInput.from = a_start * havokWorldScale;

  pickData.rayInput.to = rayEnd * havokWorldScale;

  pickData.rayInput.enableShapeCollectionFilter = false;

  /*
   * Start with the actor's filter information. This preserves its
   * collision system group, which helps prevent the ray from
   * immediately selecting that actor.
   */
  RE::CFilter filter{};
  a_actor->GetCollisionFilterInfo(filter);

  filter.SetCollisionLayer(a_queryLayer);

  pickData.rayInput.filterInfo = filter;

  RE::hkpAllRayHitCollector hitCollector{};
  pickData.allRayHitCollector = std::addressof(hitCollector);

  world->PickObject(pickData);

  const auto addHit = [&](const RE::hkpWorldRayCastOutput &a_hit) {
    if (!a_hit.HasHit() || !a_hit.rootCollidable) {
      return;
    }

    // Reject actor collision before sorting/selecting hits so geometry
    // behind NPCs remains targetable, including ragdolls and corpses.
    const auto layer = a_hit.rootCollidable->GetCollisionLayer();
    if (layer == RE::COL_LAYER::kBiped ||
        layer == RE::COL_LAYER::kCharController ||
        layer == RE::COL_LAYER::kDeadBip || layer == RE::COL_LAYER::kBipedNoCC)
      return;
    // Also catch actors with custom/modded collision layers. This check
    // applies even when the caller does not request reference results.
    const auto reference =
        RE::TESHavokUtilities::FindCollidableRef(*a_hit.rootCollidable);
    if (reference && reference->IsActor())
      return;
    RaycastResult result{};
    result.didHit = true;
    result.fraction = a_hit.hitFraction;
    result.distance = a_maxDistance * result.fraction;
    result.normal = a_hit.normal;
    result.position = a_start + direction * result.distance;
    result.layer = layer;
    result.collidable = a_hit.rootCollidable;
    // Keep reference ownership optional for collider-only terrain queries.
    if (a_resolveReferences)
      result.hitReference = RE::NiPointer<RE::TESObjectREFR>(reference);

    results.push_back(std::move(result));
  };

  if (!hitCollector.hits.empty()) {
    for (const auto &hit : hitCollector.hits) {
      addHit(hit);
    }
  } else {
    addHit(pickData.rayOutput);
  }

  std::ranges::sort(results, {}, &RaycastResult::distance);
  return results;
}

RaycastResults
CastSelectionRay(float distance,
                 const std::function<bool(RE::TESObjectREFR *)> &eligible) {
  auto results = CastCameraRay(distance, RE::COL_LAYER::kCameraPick);
  std::erase_if(results, [](const auto &hit) {
    if (IgnoreSelectionLayer(hit.layer))
      return true;
    const auto ref = hit.hitReference;
    return ref && (!ref->Get3D() || !HasVisibleSelectionGeometry(ref->Get3D()));
  });
  auto *player = RE::PlayerCharacter::GetSingleton();
  auto *camera = RE::PlayerCamera::GetSingleton();
  auto *world = RE::TES::GetSingleton();
  if (!player || !camera || !camera->cameraRoot || !world)
    return results;
  const auto origin = camera->cameraRoot->world.translate;
  const auto direction = GetCameraDirection(camera);
  // Include the third-person camera displacement in the search radius.
  const float radius = distance + origin.GetDistance(player->GetPosition());
  world->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR *ref) {
    if (!ref || ref->IsActor() || ref->IsDeleted() || ref->IsDisabled() ||
        !eligible(ref))
      return RE::BSContainer::ForEachResult::kContinue;
    auto *root = ref->Get3D();
    auto *base = ref->GetBaseObject();
    if (!root || !base || root->GetAppCulled())
      return RE::BSContainer::ForEachResult::kContinue;
    if (root->world.scale <= 0)
      return RE::BSContainer::ForEachResult::kContinue;
    bool hasHit = false;
    bool hasRigidBody = false;
    bool hasSolidCollider = false;
    RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject *node) {
      const auto collision =
          node->collisionObject
              ? node->collisionObject->AsBhkNiCollisionObject()
              : nullptr;
      const auto body = collision && collision->body
                            ? collision->body->AsBhkRigidBody()
                            : nullptr;
      if (body && body->GetRigidBody() && body->GetRigidBody()->GetShape()) {
        hasRigidBody = true;
        const auto *rigidBody = body->GetRigidBody();
        const auto collidable = rigidBody->GetCollidable();
        hasSolidCollider |=
            !IgnoreSelectionLayer(collidable->GetCollisionLayer()) &&
            rigidBody->material.responseType.get() ==
                RE::hkpMaterial::ResponseType::kSimpleContact;
        for (auto &hit : results)
          if (hit.collidable == collidable) {
            hit.hitReference = RE::NiPointer<RE::TESObjectREFR>(ref);
            hasHit = true;
          }
      }
      return RE::BSVisit::BSVisitControl::kContinue;
    });
    // Bounds are exclusively for objects without solid physical collision.
    // A miss through gaps in a collider must remain a miss. Traverse every
    // child so a trigger root cannot hide the object's solid child bodies.
    if (hasSolidCollider || hasHit) {
      if (!HasVisibleSelectionGeometry(root))
        std::erase_if(results, [ref](const auto &hit) {
          return hit.hitReference.get() == ref;
        });
      return RE::BSContainer::ForEachResult::kContinue;
    }
    const auto &broad = root->worldBound;
    if (std::isfinite(broad.radius) && broad.radius > 0) {
      const auto &c = broad.center;
      const auto r = broad.radius;
      if (!IntersectBox({origin.x, origin.y, origin.z},
                        {direction.x, direction.y, direction.z},
                        {c.x - r, c.y - r, c.z - r},
                        {c.x + r, c.y + r, c.z + r}, distance))
        return RE::BSContainer::ForEachResult::kContinue;
    }
    if (!HasVisibleSelectionGeometry(root))
      return RE::BSContainer::ForEachResult::kContinue;
    const auto *definition =
        BuildingManager::GetPlaceableDefinition(ref->GetFormID());
    if (!definition)
      definition = PlaceableRegistry::FindUnique(base);
    const auto bounds = definition && definition->selectionBounds
                            ? definition->selectionBounds
                            : Models::GetVertexBounds(base);
    if (!bounds)
      return RE::BSContainer::ForEachResult::kContinue;
    // NIF bounds include the root's authored transform, so transform the
    // ray by the reference placement, not the scene root a second time.
    RE::NiTransform placement;
    placement.translate = ref->GetPosition();
    placement.rotate.SetEulerAnglesXYZ(ref->GetAngle());
    placement.scale = ref->GetScale();
    if (placement.scale <= 0)
      return RE::BSContainer::ForEachResult::kContinue;
    const auto inverse = placement.Invert();
    const auto start = inverse * origin;
    const auto ray = inverse.rotate * direction * inverse.scale;
    const auto hit =
        IntersectBox({start.x, start.y, start.z}, {ray.x, ray.y, ray.z},
                     bounds->min, bounds->max, distance);
    if (!hit)
      return RE::BSContainer::ForEachResult::kContinue;
    {
      RaycastResult result;
      result.didHit = true;
      result.distance = *hit;
      result.fraction = *hit / distance;
      result.position = origin + direction * *hit;
      result.hitReference = RE::NiPointer<RE::TESObjectREFR>(ref);
      result.hasRigidBody = hasRigidBody;
      std::array<RE::NiPoint3, 8> corners;
      for (unsigned i = 0; i < corners.size(); ++i)
        corners[i] =
            placement * RE::NiPoint3{(i & 1) ? bounds->max[0] : bounds->min[0],
                                     (i & 2) ? bounds->max[1] : bounds->min[1],
                                     (i & 4) ? bounds->max[2] : bounds->min[2]};
      result.selectionBox = corners;
      results.push_back(std::move(result));
    }
    return RE::BSContainer::ForEachResult::kContinue;
  });
  std::ranges::sort(results, {}, &RaycastResult::distance);
  return results;
}

RaycastResults CastCameraRay(float a_maxDistance, RE::COL_LAYER a_queryLayer,
                             bool a_resolveReferences) {
  const auto camera = RE::PlayerCamera::GetSingleton();

  if (!camera || !camera->cameraRoot) {
    return {};
  }

  const RE::NiPoint3 origin = camera->cameraRoot->world.translate;

  const RE::NiPoint3 direction = GetCameraDirection(camera);

  return CastRay(origin, direction, a_maxDistance, a_queryLayer,
                 RE::PlayerCharacter::GetSingleton(), a_resolveReferences);
}
} // namespace SkyBuild::Physics
