#include "building/Snapping.h"
#include "building/PlacementRotation.h"
#include "building/GridConnections.h"
#include "building/PlaceableRegistry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>
namespace SkyBuild::Snapping {
using namespace BuildingManager;
RE::NiPoint3 OffsetGridHit(BuildingID id, const RE::NiPoint3 &point,
                           const RE::NiPoint3 &rayDirection,
                           std::optional<GridPosition> sourceCell) {
  const auto building = GetBuilding(id);
  if (!building)
    return point;
  constexpr float band = 5.F;
  const float c = std::cos(building->yaw), s = std::sin(building->yaw);
  const auto delta = point - building->origin;
  const std::array<float, 3> local{delta.x * c - delta.y * s,
                                   delta.x * s + delta.y * c, delta.z};
  const std::array<float, 3> direction{rayDirection.x * c - rayDirection.y * s,
                                       rayDirection.x * s + rayDirection.y * c,
                                       rayDirection.z};
  const float length = std::sqrt(rayDirection.Dot(rayDirection));
  std::array<float, 3> lower{};
  bool nearSource = sourceCell.has_value();
  if (sourceCell) {
    lower = {sourceCell->x * 256.F - 128.F, sourceCell->y * 256.F - 128.F,
             sourceCell->z * 256.F};
    for (int axis = 0; axis < 3; ++axis)
      nearSource &= local[axis] >= lower[axis] - band &&
                    local[axis] <= lower[axis] + 256.F + band;
  }
  if (!nearSource) {
    for (int axis = 0; axis < 3; ++axis) {
      const float shift = axis < 2 ? 128.F : 0.F;
      // At a mathematical plane, classify the source voxel on the ray's
      // inward side. A known volume cell avoids this tie/noise inference.
      const float inside =
          local[axis] +
          (length > .000001F ? direction[axis] * (.01F / length) : 0.F);
      lower[axis] = std::floor((inside + shift) / 256.F) * 256.F - shift;
    }
  }
  int closestAxis = 0;
  float closest = std::numeric_limits<float>::max(), sign = -1;
  for (int axis = 0; axis < 3; ++axis) {
    const float below = local[axis] - lower[axis];
    const float above = lower[axis] + 256.F - local[axis];
    const float distance = std::min(below, above);
    if (distance < closest ||
        (distance == closest &&
         std::abs(direction[axis]) > std::abs(direction[closestAxis]))) {
      closest = distance;
      closestAxis = axis;
      sign = below <= above ? -1.F : 1.F;
    }
  }
  if (closest > band)
    return point;
  // Cross the selected face by five units. This also handles a source hit
  // rounded just beyond the boundary without pushing it back into the volume.
  const float offset = sign * (std::max(0.F, closest) + band);
  const RE::NiPoint3 bias =
      closestAxis == 0   ? RE::NiPoint3{offset * c, -offset * s, 0}
      : closestAxis == 1 ? RE::NiPoint3{offset * s, offset * c, 0}
                         : RE::NiPoint3{0, 0, offset};
  return point + bias;
}
SnapResult MakeCandidate(BuildingID id, const GridPosition &cell,
                         GridObjectType type, Grid::Settings settings,
                         bool allowAdjacentVolumeFaces) {
  const auto building = GetBuilding(id);
  if (!building || !CanPlaceGridObject(id, cell, type, settings) ||
      !Grid::HasConnection(
          cell.x, cell.y, cell.z, type, settings,
          [id](const Grid::Slot &slot) { return IsSlotOccupied(id, slot); },
          allowAdjacentVolumeFaces))
    return {};
  const RE::NiPoint3 right{std::cos(building->yaw), -std::sin(building->yaw),
                           0};
  const RE::NiPoint3 forward{std::sin(building->yaw), std::cos(building->yaw),
                             0};
  auto position = GetWorldPosition(id, cell);
  float yaw = building->yaw;
  if (type == GridObjectType::kSideX) {
    position = position - right * 128.0F;
    yaw += std::numbers::pi_v<float> * .5F;
  }
  if (type == GridObjectType::kSideY)
    position = position - forward * 128.0F;
  if (type == GridObjectType::kCorner)
    position = position - (right + forward) * 128.0F;
  if (settings.sideRole == Grid::SideRole::kDoor) {
    for (const auto &[formID, object] : GetPlacedObjects()) {
      if (object.buildingID != id || object.gridPosition != cell ||
          object.type != type ||
          object.grid.sideRole != Grid::SideRole::kDoorway)
        continue;
      const auto doorway = ResolvePlacedObject(formID);
      if (!doorway)
        continue;
      RE::NiMatrix3 world, model;
      world.SetEulerAnglesXYZ(doorway->GetAngle());
      RE::NiPoint3 offset{};
      if (const auto definition = GetPlaceableDefinition(formID))
        offset = definition->rotationOffset;
      model.SetEulerAnglesXYZ(offset);
      RE::NiPoint3 rotation;
      PlacementRotation::ToEulerXYZ(world * model.Transpose(), rotation);
      return {doorway->GetPosition(), rotation, id, cell, type, true};
    }
    return {};
  }
  return {position, {0, 0, yaw}, id, cell, type, true};
}
namespace {
struct Candidate {
  GridPosition cell;
  GridObjectType type;
};
float Dot2(const RE::NiPoint3 &a, const RE::NiPoint3 &b) {
  return a.x * b.x + a.y * b.y;
}
SnapResult Closest(BuildingID id, std::vector<Candidate> candidates,
                   Grid::Settings settings, const RE::NiPoint3 &point) {
  std::vector<SnapResult> valid;
  for (const auto &candidate : candidates) {
    auto result = MakeCandidate(id, candidate.cell, candidate.type, settings);
    if (result.didSnap)
      valid.push_back(result);
  }
  if (valid.empty())
    return {};
  return *std::min_element(
      valid.begin(), valid.end(), [&](const auto &a, const auto &b) {
        const auto da = a.position - point, db = b.position - point;
        return da.Dot(da) < db.Dot(db);
      });
}
} // namespace
SnapResult SnapBasicAtPoint(BuildingID id, const RE::NiPoint3 &point,
                            const RE::NiPoint3 &sampleDirection,
                            Grid::Settings settings) {
  const auto building = GetBuilding(id);
  if (!building)
    return {};
  const float c = std::cos(building->yaw), s = std::sin(building->yaw);
  const auto delta = point - building->origin;
  const RE::NiPoint3 local{delta.x * c - delta.y * s, delta.x * s + delta.y * c,
                           delta.z};
  const RE::NiPoint3 direction{sampleDirection.x * c - sampleDirection.y * s,
                               sampleDirection.x * s + sampleDirection.y * c,
                               sampleDirection.z};
  const auto sample = local + direction * .01F;
  const bool faceOnly = Grid::HasOnlyHorizontalFaces(settings);
  const bool topOnly = faceOnly && !(settings.occupy & Grid::kBottom);
  const bool volume = Grid::HasVolume(settings) && !faceOnly;
  const GridPosition cell{
      static_cast<int>(std::floor(sample.x / 256 + .5F)),
      static_cast<int>(std::floor(sample.y / 256 + .5F)),
      volume ? static_cast<int>(std::floor(sample.z / 256))
             : static_cast<int>(std::floor(local.z / 256 + .5F)) -
                   (topOnly ? 1 : 0)};
  auto result = MakeCandidate(id, cell, GridObjectType::kBasic, settings);
  if (result.didSnap)
    return result;
  struct Neighbor {
    GridPosition offset;
    float distance;
  };
  const float x = local.x - cell.x * 256, y = local.y - cell.y * 256,
              z = local.z - (cell.z + (topOnly ? 1 : 0)) * 256;
  // Rank outward neighbor directions using the hit relative to the cell center.
  const float vertical = volume ? z - 128.0F : z;
  std::array<Neighbor, 6> neighbors{{{{1, 0, 0}, -x},
                                     {{-1, 0, 0}, x},
                                     {{0, 1, 0}, -y},
                                     {{0, -1, 0}, y},
                                     {{0, 0, 1}, -vertical},
                                     {{0, 0, -1}, vertical}}};
  std::stable_sort(
      neighbors.begin(), neighbors.end(),
      [](const auto &a, const auto &b) { return a.distance < b.distance; });
  for (const auto &neighbor : neighbors) {
    const auto &d = neighbor.offset;
    result = MakeCandidate(id, {cell.x + d.x, cell.y + d.y, cell.z + d.z},
                           GridObjectType::kBasic, settings);
    if (result.didSnap)
      return result;
  }
  return {};
}
SnapResult SnapCellAtPoint(BuildingID id, const RE::NiPoint3 &point,
                           const RE::NiPoint3 &sampleDirection,
                           Grid::SnapMode mode, Grid::Settings settings) {
  if (mode == Grid::SnapMode::kBasic)
    return SnapBasicAtPoint(id, point, sampleDirection, settings);
  const auto building = GetBuilding(id);
  if (!building || mode == Grid::SnapMode::kNone)
    return {};
  const auto sample = point + sampleDirection * .01F;
  const auto delta = sample - building->origin;
  const float c = std::cos(building->yaw), s = std::sin(building->yaw);
  const GridPosition cell{
      static_cast<int>(std::floor((delta.x * c - delta.y * s) / 256 + .5F)),
      static_cast<int>(std::floor((delta.x * s + delta.y * c) / 256 + .5F)),
      static_cast<int>(std::floor(delta.z / 256))};
  const auto candidatesFor = [&](GridPosition cell) {
    std::vector<Candidate> candidates;
    if (mode == Grid::SnapMode::kSide) {
      candidates = {{cell, GridObjectType::kSideX},
                    {{cell.x + 1, cell.y, cell.z}, GridObjectType::kSideX},
                    {cell, GridObjectType::kSideY},
                    {{cell.x, cell.y + 1, cell.z}, GridObjectType::kSideY}};
    } else {
      for (int x = 0; x <= 1; ++x)
        for (int y = 0; y <= 1; ++y)
          candidates.push_back(
              {{cell.x + x, cell.y + y, cell.z}, GridObjectType::kCorner});
    }
    return candidates;
  };
  const auto cellCenter = GetWorldPosition(id, cell) + RE::NiPoint3{0, 0, 128};
  const auto hitDirection = point - cellCenter;
  const auto selectDirection = [&](std::vector<Candidate> candidates) {
    struct Ranked {
      SnapResult result;
      float alignment;
    };
    std::vector<Ranked> ranked;
    for (const auto &candidate : candidates) {
      auto result = MakeCandidate(id, candidate.cell, candidate.type, settings);
      if (!result.didSnap)
        continue;
      auto direction = result.position + RE::NiPoint3{0, 0, 128} - cellCenter;
      const float length = std::sqrt(direction.Dot(direction));
      ranked.push_back(
          {result, length > .0001F ? direction.Dot(hitDirection) / length : 0});
    }
    if (ranked.empty())
      return SnapResult{};
    return std::max_element(ranked.begin(), ranked.end(),
                            [](const auto &a, const auto &b) {
                              return a.alignment < b.alignment;
                            })
        ->result;
  };
  auto result = selectDirection(candidatesFor(cell));
  if (result.didSnap)
    return result;
  std::vector<Candidate> neighbors;
  for (const auto &d : std::array<GridPosition, 6>{{{1, 0, 0},
                                                    {-1, 0, 0},
                                                    {0, 1, 0},
                                                    {0, -1, 0},
                                                    {0, 0, 1},
                                                    {0, 0, -1}}}) {
    auto candidates = candidatesFor({cell.x + d.x, cell.y + d.y, cell.z + d.z});
    neighbors.insert(neighbors.end(), candidates.begin(), candidates.end());
  }
  return selectDirection(std::move(neighbors));
}
SnapResult SnapToObject(RE::TESObjectREFR *reference,
                        const RE::NiPoint3 &rawPoint,
                        const RE::NiPoint3 &normal, Grid::SnapMode mode,
                        Grid::Settings settings, const RE::NiPoint3 &camera) {
  if (!reference || mode == Grid::SnapMode::kNone)
    return {};
  const auto id = GetBuildingID(reference->GetFormID());
  const auto type = GetGridObjectType(reference->GetFormID());
  const auto building = id ? GetBuilding(*id) : std::nullopt;
  if (!building || !type || *type == GridObjectType::kNone)
    return {};
  if (GetGridSettings(reference->GetFormID()).sideRole == Grid::SideRole::kDoor)
    return {}; // Doors never act as structural snap sources.
  if (settings.sideRole == Grid::SideRole::kDoor)
    return MakeCandidate(*id, GetGridPosition(reference->GetFormID()), *type,
                         settings);
  auto rayDirection = rawPoint - camera;
  if (rayDirection.Dot(rayDirection) < .0001F)
    rayDirection = normal * -1.F;
  const float rayLength = std::sqrt(rayDirection.Dot(rayDirection));
  if (rayLength > .000001F)
    rayDirection = rayDirection * (1.F / rayLength);
  const auto source = GetGridPosition(reference->GetFormID());
  const auto target = GetGridSettings(reference->GetFormID());
  auto point =
      OffsetGridHit(*id, rawPoint, rayDirection,
                    *type == GridObjectType::kBasic && Grid::HasVolume(target)
                        ? std::optional{source}
                        : std::nullopt);
  // A face-only source has two sides, not a solid voxel interior. Resolve
  // its known plane toward the viewing side, including small hit-point noise.
  if (*type != GridObjectType::kBasic || Grid::HasOnlyHorizontalFaces(target)) {
    float nearest = 5.F;
    for (const auto &face :
         Grid::OccupiedSlots(source.x, source.y, source.z, *type, target)) {
      if (face.kind != Grid::SlotKind::kHorizontal &&
          face.kind != Grid::SlotKind::kSideX &&
          face.kind != Grid::SlotKind::kSideY)
        continue;
      const bool x = face.kind == Grid::SlotKind::kSideX;
      const bool y = face.kind == Grid::SlotKind::kSideY;
      const RE::NiPoint3 axis =
          x ? RE::NiPoint3{std::cos(building->yaw), -std::sin(building->yaw), 0}
          : y ? RE::NiPoint3{std::sin(building->yaw), std::cos(building->yaw),
                             0}
              : RE::NiPoint3{0, 0, 1};
      auto origin = GetWorldPosition(*id, {face.x, face.y, face.z});
      if (x || y)
        origin = origin - axis * 128.F;
      const float distance = (rawPoint - origin).Dot(axis);
      if (std::abs(distance) > nearest)
        continue;
      nearest = std::abs(distance);
      point = rawPoint +
              axis * ((rayDirection.Dot(axis) > 0 ? -5.F : 5.F) - distance);
    }
  }
  // A horizontal face chooses its plane from the actual hit height. The
  // classification bias must not move a lower-half hit above the midpoint.
  if (mode == Grid::SnapMode::kBasic && Grid::HasOnlyHorizontalFaces(settings))
    point.z = rawPoint.z;
  const auto cell = GetGridPosition(reference->GetFormID());
  if (mode == Grid::SnapMode::kBasic || *type == GridObjectType::kBasic) {
    return SnapCellAtPoint(*id, point, rayDirection, mode, settings);
  }
  const RE::NiPoint3 right{std::cos(building->yaw), -std::sin(building->yaw),
                           0},
      forward{std::sin(building->yaw), std::cos(building->yaw), 0};
  if (*type == GridObjectType::kCorner) {
    const bool upper = point.z - reference->GetPositionZ() >= 128;
    if (mode == Grid::SnapMode::kCorner) {
      auto next = cell;
      next.z += upper ? 1 : -1;
      auto result = MakeCandidate(*id, next, GridObjectType::kCorner, settings);
      if (result.didSnap)
        return result;
      next.z = cell.z + (upper ? -1 : 1);
      return MakeCandidate(*id, next, GridObjectType::kCorner, settings);
    }

    return Closest(*id,
                   {{cell, GridObjectType::kSideX},
                    {{cell.x, cell.y - 1, cell.z}, GridObjectType::kSideX},
                    {cell, GridObjectType::kSideY},
                    {{cell.x - 1, cell.y, cell.z}, GridObjectType::kSideY}},
                   settings, camera);
  }
  const bool sideX = *type == GridObjectType::kSideX;
  const auto difference = point - reference->GetPosition();

  if (mode == Grid::SnapMode::kCorner) {
    auto next = cell;
    if (sideX)
      next.y += Dot2(difference, forward) >= 0 ? 1 : 0;
    else
      next.x += Dot2(difference, right) >= 0 ? 1 : 0;
    return MakeCandidate(*id, next, GridObjectType::kCorner, settings);
  }
  const float along = Dot2(difference, sideX ? forward : right),
              vertical = difference.z - 128;
  const auto approach = camera - reference->GetPosition();
  const bool positive = Dot2(approach, sideX ? right : forward) >= 0;
  const auto perpendicular =
      sideX ? GridObjectType::kSideY : GridObjectType::kSideX;
  const int endpointX = cell.x - (sideX && !positive ? 1 : 0),
            endpointY = cell.y - (!sideX && !positive ? 1 : 0);
  struct WallCandidate {
    GridPosition cell;
    GridObjectType type;
    float score;
  };
  std::array<WallCandidate, 4> candidates{
      {{{endpointX + (sideX ? 0 : 1), endpointY + (sideX ? 1 : 0), cell.z},
        perpendicular,
        along},
       {{endpointX, endpointY, cell.z}, perpendicular, -along},
       {{cell.x, cell.y, cell.z + 1}, *type, vertical},
       {{cell.x, cell.y, cell.z - 1}, *type, -vertical}}};
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const auto &a, const auto &b) { return a.score > b.score; });
  for (const auto &candidate : candidates) {
    auto result = MakeCandidate(*id, candidate.cell, candidate.type, settings);
    if (result.didSnap)
      return result;
  }
  return {};
}
} // namespace SkyBuild::Snapping
