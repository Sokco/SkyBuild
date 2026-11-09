#include "building/GridRaycast.h"
#include "building/GridConnections.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace SkyBuild::GridRaycast {
namespace {
// A ray crossing must enter a cell with occupied or enabled grid elements.
// Candidate availability is still checked separately after neighbor fallback.
bool NearStructure(BuildingManager::BuildingID id,
                   const BuildingManager::GridPosition &cell) {
  using namespace BuildingManager;
  for (const auto &slot :
       Grid::OccupiedSlots(cell.x, cell.y, cell.z, GridObjectType::kBasic,
                           {Grid::ALL_OCCUPANCY}))
    if (IsSlotOccupied(id, slot))
      return true;
  return Grid::HasConnection(
      cell.x, cell.y, cell.z, GridObjectType::kBasic, {Grid::ALL_OCCUPANCY},
      [id](const Grid::Slot &slot) { return IsSlotOccupied(id, slot); });
}
Grid::Slot CrossedFace(const RE::NiPoint3 &point, int axis) {
  Grid::Slot face{static_cast<int>(std::floor(point.x / 256 + .5F)),
                  static_cast<int>(std::floor(point.y / 256 + .5F)),
                  static_cast<int>(std::floor(point.z / 256)),
                  Grid::FaceKind(axis)};
  if (axis == 0)
    face.x = static_cast<int>(std::lround(point.x / 256 + .5F));
  if (axis == 1)
    face.y = static_cast<int>(std::lround(point.y / 256 + .5F));
  if (axis == 2)
    face.z = static_cast<int>(std::lround(point.z / 256));
  return face;
}
} // namespace
Result
CastGridRay(const RE::NiPoint3 &worldOrigin, const RE::NiPoint3 &d,
            float maximum, Grid::SnapMode mode, Grid::Settings settings,
            std::optional<BuildingManager::BuildingID> requestedBuilding) {
  using namespace BuildingManager;
  if (maximum <= 0 || d.Dot(d) < .000001F || mode == Grid::SnapMode::kNone)
    return {};
  if (!requestedBuilding && settings.sideRole == Grid::SideRole::kDoor) {
    // Openings may belong to a different grid than the last object aimed at.
    // Compare actual ray/plane crossings, never distances to face centers.
    Result selected;
    float nearest = maximum;
    for (const auto &[buildingID, grid] : GetBuildings()) {
      const auto candidate = CastGridRay(worldOrigin, d, maximum, mode, settings, buildingID);
      if (!candidate.didSnap)
        continue;
      const auto normal =
          candidate.type == GridObjectType::kSideX
              ? RE::NiPoint3{std::cos(grid.yaw), -std::sin(grid.yaw), 0}
              : RE::NiPoint3{std::sin(grid.yaw), std::cos(grid.yaw), 0};
      const float denominator = d.Dot(normal);
      if (std::abs(denominator) < .000001F)
        continue;
      const float distance =
          (candidate.position - worldOrigin).Dot(normal) / denominator;
      if (!selected.didSnap || distance < nearest) {
        selected = candidate;
        nearest = distance;
      }
    }
    return selected;
  }
  const auto id = requestedBuilding ? requestedBuilding : GetLastSeenBuilding();
  const auto building = id ? GetBuilding(*id) : std::nullopt;
  if (!building)
    return {};
  const auto delta = worldOrigin - building->origin;
  const float c = std::cos(building->yaw), s = std::sin(building->yaw);
  const RE::NiPoint3 origin{delta.x * c - delta.y * s,
                            delta.x * s + delta.y * c, delta.z},
      direction{d.x * c - d.y * s, d.x * s + d.y * c, d.z};
  struct Crossing {
    float distance;
    int axis;
  };
  std::vector<Crossing> crossings;
  const std::array<float, 3> start{origin.x, origin.y, origin.z},
      travel{direction.x, direction.y, direction.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(travel[axis]) < .000001F)
      continue;
    const float offset = axis < 2 ? 128.0F : 0.0F;
    const float finish = start[axis] + travel[axis] * maximum;
    const int first = static_cast<int>(
                  std::ceil((std::min(start[axis], finish) + offset) / 256)),
              last = static_cast<int>(
                  std::floor((std::max(start[axis], finish) + offset) / 256));
    for (int plane = first; plane <= last; ++plane) {
      const float distance =
          (plane * 256.0F - offset - start[axis]) / travel[axis];
      if (distance >= 0 && distance <= maximum)
        crossings.push_back({distance, axis});
    }
  }
  // Start at the nearest forward crossing, bounded by the physics hit distance.
  std::stable_sort(
      crossings.begin(), crossings.end(),
      [](const auto &a, const auto &b) { return a.distance < b.distance; });
  Result selected;
  Grid::BoundaryConnections selectedConnections;
  const auto consider = [&](const Result &candidate) {
    if (!candidate.didSnap)
      return;
    const auto occupied = [id](const Grid::Slot &slot) {
      return IsSlotOccupied(*id, slot);
    };
    const auto connections = Grid::GetBoundaryConnections(
        Grid::OccupiedSlots(candidate.gridPosition.x, candidate.gridPosition.y,
                            candidate.gridPosition.z, candidate.type, settings),
        occupied);
    // Prefer an extension of the exposed boundary for every occupancy profile.
    // Retain the nearest crossing on exact ties.
    // Availability still comes from topology and selection from the grid ray.
    if (!selected.didSnap || connections.BetterThan(selectedConnections)) {
      selected = candidate;
      selectedConnections = connections;
    }
  };
  // Unlike direct hits, grid rays use only a volume's flush face connections,
  // not extensions to adjacent plane cells. Volume neighbors remain enabled.
  for (const auto &crossing : crossings) {
    // Only the intersected face is eligible. Nearby faces on the same infinite
    // plane are not ray hits, even when they connect to an existing piece.
    if (mode == Grid::SnapMode::kSide && crossing.axis != 2) {
      const auto face =
          CrossedFace(origin + direction * crossing.distance, crossing.axis);
      const auto result = Snapping::MakeCandidate(
          *id, {face.x, face.y, face.z},
          crossing.axis == 0 ? GridObjectType::kSideX : GridObjectType::kSideY,
          settings, false);
      consider(result);
    } else if (mode == Grid::SnapMode::kBasic && crossing.axis == 2 &&
               (settings.occupy & (Grid::kBottom | Grid::kTop))) {
      const auto face = CrossedFace(origin + direction * crossing.distance, 2);
      if (settings.occupy & Grid::kBottom) {
        const auto result =
            Snapping::MakeCandidate(*id, {face.x, face.y, face.z},
                                    GridObjectType::kBasic, settings, false);
        consider(result);
      }
      if (settings.occupy & Grid::kTop) {
        const auto result =
            Snapping::MakeCandidate(*id, {face.x, face.y, face.z - 1},
                                    GridObjectType::kBasic, settings, false);
        consider(result);
      }
    }
  }
  if (mode == Grid::SnapMode::kSide || (mode == Grid::SnapMode::kBasic &&
                                        Grid::HasOnlyHorizontalFaces(settings)))
    return selected;
  for (const auto &crossing : crossings) {
    const auto point = origin + direction * crossing.distance;
    GridPosition cell{static_cast<int>(std::floor(point.x / 256 + .5F)),
                      static_cast<int>(std::floor(point.y / 256 + .5F)),
                      static_cast<int>(std::floor(point.z / 256))};
    if (mode == Grid::SnapMode::kBasic) {
      if (Grid::HasVolume(settings)) {
        // Both cells sharing this crossing are ray hits. Rank their complete
        // shells just like face pieces, without off-ray neighbor fallback.
        for (float sign : {-1.F, 1.F}) {
          const auto inside = point + direction * (sign * .01F);
          const GridPosition crossed{
              static_cast<int>(std::floor(inside.x / 256 + .5F)),
              static_cast<int>(std::floor(inside.y / 256 + .5F)),
              static_cast<int>(std::floor(inside.z / 256))};
          consider(Snapping::MakeCandidate(*id, crossed, GridObjectType::kBasic,
                                           settings, false));
        }
        continue;
      } else {
        if (crossing.axis != 2)
          continue;
        cell.z = static_cast<int>(std::lround(point.z / 256));
      }
    } else {
      cell.x = static_cast<int>(std::lround(point.x / 256 + .5F));
      cell.y = static_cast<int>(std::lround(point.y / 256 + .5F));
    }
    if (!NearStructure(*id, cell))
      continue;
    const auto worldPoint = worldOrigin + d * crossing.distance;
    const auto result = Snapping::SnapCellAtPoint(
        *id, Snapping::OffsetGridHit(*id, worldPoint, d), d, mode, settings);
    consider(result);
  }
  return selected;
}
} // namespace SkyBuild::GridRaycast
