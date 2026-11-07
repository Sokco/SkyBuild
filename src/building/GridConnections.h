#pragma once
#include "building/SnappingProfile.h"
#include <span>
namespace SkyBuild::Grid {
struct ConnectionDefinition {
  SlotKind target;
  std::int32_t dx, dy, dz;
};
struct NeighborSet {
  std::array<Slot, 64> values{};
  std::size_t size{};
  constexpr void Add(Slot slot) {
    for (std::size_t i = 0; i < size; ++i)
      if (values[i] == slot)
        return;
    values[size++] = slot;
  }
};
constexpr SlotKind FaceKind(int axis) {
  return axis == 0   ? SlotKind::kSideX
         : axis == 1 ? SlotKind::kSideY
                     : SlotKind::kHorizontal;
}
constexpr int FaceAxis(SlotKind kind) {
  return kind == SlotKind::kSideX ? 0 : kind == SlotKind::kSideY ? 1 : 2;
}
constexpr Slot At(std::array<int, 3> p, SlotKind kind) {
  return {p[0], p[1], p[2], kind};
}
struct BoundaryConnections {
  int flush{};
  int shared{};
  int exposed{};
  bool operator==(const BoundaryConnections &) const = default;
  constexpr bool BetterThan(const BoundaryConnections &other) const {
    return flush < other.flush ||
           (flush == other.flush &&
            (shared < other.shared ||
             (shared == other.shared && exposed > other.exposed)));
  }
};
// Compare no-hit placements by contact with the existing build: fewer flush
// surfaces, fewer shared seams, then more exposed edges. The candidate boundary
// is the union of its occupied elements; a volume supplies its six shell faces.
// This is ranking only: it does not occupy or enable any additional grid slots.
template <class IsOccupied>
BoundaryConnections GetBoundaryConnections(const Slots &candidate,
                                           IsOccupied occupied) {
  struct Edge {
    std::array<int, 3> start{};
    int axis{};
    bool operator==(const Edge &) const = default;
  };
  std::array<Edge, 24> edges{}; // At most six faces with four edges each.
  std::size_t size = 0;
  std::array<Slot, 6> faces{};
  std::size_t faceCount = 0;
  bool hasVolume = false;
  for (const auto &slot : candidate)
    hasVolume |= slot.kind == SlotKind::kCenter;
  const auto addEdge = [&](Edge edge) {
    for (std::size_t i = 0; i < size; ++i)
      if (edges[i] == edge)
        return;
    edges[size++] = edge;
  };
  const auto addFace = [&](const Slot &face) {
    for (std::size_t i = 0; i < faceCount; ++i)
      if (faces[i] == face)
        return;
    faces[faceCount++] = face;
    const int normal = FaceAxis(face.kind);
    for (int tangent = 0; tangent < 3; ++tangent) {
      if (tangent == normal)
        continue;
      for (int end = 0; end <= 1; ++end) {
        Edge edge{{face.x, face.y, face.z}, 3 - normal - tangent};
        edge.start[tangent] += end;
        addEdge(edge);
      }
    }
  };
  for (const auto &slot : candidate) {
    if (slot.kind == SlotKind::kCenter) {
      // Volume pieces are ranked by their shell, without occupying its faces.
      for (int normal = 0; normal < 3; ++normal)
        for (int end = 0; end <= 1; ++end) {
          std::array<int, 3> p{slot.x, slot.y, slot.z};
          p[normal] += end;
          addFace(At(p, FaceKind(normal)));
        }
    } else if (slot.kind == SlotKind::kHorizontal ||
               slot.kind == SlotKind::kSideX || slot.kind == SlotKind::kSideY) {
      addFace(slot);
    } else if (slot.kind == SlotKind::kCorner) {
      addEdge({{slot.x, slot.y, slot.z}, 2});
    }
  }
  BoundaryConnections result;
  for (std::size_t i = 0; i < faceCount; ++i) {
    const auto &face = faces[i];
    if (hasVolume) {
      result.flush += int(occupied(face));
    } else {
      // A face is flush with either incident volume. A plane sandwiched
      // between two occupied volumes is less of an extension than an outer one.
      const int normal = FaceAxis(face.kind);
      std::array<int, 3> cell{face.x, face.y, face.z};
      result.flush += int(occupied(At(cell, SlotKind::kCenter)));
      --cell[normal];
      result.flush += int(occupied(At(cell, SlotKind::kCenter)));
    }
  }
  for (std::size_t i = 0; i < size; ++i) {
    const auto &edge = edges[i];
    int connections = 0;
    // A volume supplies the whole edge. Its adjoining faces do not add
    // another branch at that same edge.
    const int first = (edge.axis + 1) % 3, second = (edge.axis + 2) % 3;
    for (int a : {-1, 0})
      for (int b : {-1, 0}) {
        auto cell = edge.start;
        cell[first] += a;
        cell[second] += b;
        connections += int(occupied(At(cell, SlotKind::kCenter)));
      }
    // Four face slots meet a grid edge, in two perpendicular planes.
    if (!connections)
      for (int normal = 0; normal < 3; ++normal) {
        if (normal == edge.axis)
          continue;
        for (int offset : {-1, 0}) {
          auto face = edge.start;
          face[3 - normal - edge.axis] += offset;
          connections += int(occupied(At(face, FaceKind(normal))));
        }
      }
    result.exposed += connections == 1;
    result.shared += connections >= 2;
  }
  // A line-only piece also extends through its endpoints. Faces and volumes
  // already account for these contacts through their boundary edges.
  if (!hasVolume && !faceCount)
    for (const auto &slot : candidate) {
      if (slot.kind != SlotKind::kCorner)
        continue;
      const int ends =
          int(occupied(Slot{slot.x, slot.y, slot.z - 1, SlotKind::kCorner})) +
          int(occupied(Slot{slot.x, slot.y, slot.z + 1, SlotKind::kCorner}));
      result.exposed += ends == 1;
      result.shared += ends == 2;
    }
  return result;
}
constexpr void AddCoplanarNeighbors(NeighborSet &set, Slot face) {
  const int normal = FaceAxis(face.kind);
  for (int axis = 0; axis < 3; ++axis)
    if (axis != normal)
      for (int sign : {-1, 1}) {
        std::array<int, 3> p{face.x, face.y, face.z};
        p[axis] += sign;
        set.Add(At(p, face.kind));
      }
}
// A placed grid element enables only its immediate topology neighbors.
// Enabled but empty elements never enable further connections.
constexpr NeighborSet AvailableNeighbors(Slot occupied) {
  NeighborSet set;
  const std::array<int, 3> origin{occupied.x, occupied.y, occupied.z};
  if (occupied.kind == SlotKind::kCenter) {
    for (int axis = 0; axis < 3; ++axis) {
      for (int sign : {-1, 1}) {
        auto p = origin;
        p[axis] += sign;
        set.Add(At(p, SlotKind::kCenter));
      }
      for (int end = 0; end <= 1; ++end) {
        auto p = origin;
        p[axis] += end;
        const auto face = At(p, FaceKind(axis));
        set.Add(face);
        AddCoplanarNeighbors(set, face);
      }
    }
    for (int x = 0; x <= 1; ++x)
      for (int y = 0; y <= 1; ++y)
        set.Add(
            {occupied.x + x, occupied.y + y, occupied.z, SlotKind::kCorner});
  } else if (occupied.kind == SlotKind::kCorner) {
    for (int sign : {-1, 1})
      set.Add({occupied.x, occupied.y, occupied.z + sign, SlotKind::kCorner});
    for (int x = -1; x <= 0; ++x)
      for (int y = -1; y <= 0; ++y) {
        set.Add(
            {occupied.x + x, occupied.y + y, occupied.z, SlotKind::kCenter});
        for (int end = 0; end <= 1; ++end)
          set.Add({occupied.x + x, occupied.y + y, occupied.z + end,
                   SlotKind::kHorizontal});
      }
    for (int offset : {-1, 0}) {
      set.Add({occupied.x, occupied.y + offset, occupied.z, SlotKind::kSideX});
      set.Add({occupied.x + offset, occupied.y, occupied.z, SlotKind::kSideY});
    }
  } else {
    const int normal = FaceAxis(occupied.kind);
    AddCoplanarNeighbors(set, occupied);
    for (int offset : {-1, 0}) {
      auto p = origin;
      p[normal] += offset;
      set.Add(At(p, SlotKind::kCenter));
      // Wall perimeters support volume extensions as well as face extensions.
      // Horizontal planes retain their direct above/below volume connections.
      if (normal != 2)
        for (int tangent = 0; tangent < 3; ++tangent)
          if (tangent != normal)
            for (int sign : {-1, 1}) {
              auto neighbor = p;
              neighbor[tangent] += sign;
              set.Add(At(neighbor, SlotKind::kCenter));
            }
    }
    // Each face edge has two incident perpendicular faces, one on each side.
    for (int axis = 0; axis < 3; ++axis)
      if (axis != normal)
        for (int end = 0; end <= 1; ++end)
          for (int offset : {-1, 0}) {
            auto p = origin;
            p[axis] += end;
            p[normal] += offset;
            set.Add(At(p, FaceKind(axis)));
          }
    // Vertical edges are the pillar slots in the current grid schema.
    if (normal == 2) {
      for (int x = 0; x <= 1; ++x)
        for (int y = 0; y <= 1; ++y)
          for (int offset : {-1, 0})
            set.Add({occupied.x + x, occupied.y + y, occupied.z + offset,
                     SlotKind::kCorner});
    } else {
      const int tangent = normal == 0 ? 1 : 0;
      for (int end = 0; end <= 1; ++end) {
        auto p = origin;
        p[tangent] += end;
        set.Add(At(p, SlotKind::kCorner));
      }
    }
  }
  return set;
}
struct ConnectionSet {
  std::array<ConnectionDefinition, 64> values{};
  std::size_t size{};
};
// Invert the availability definitions once at compile time. At runtime a
// candidate queries only the placed slots capable of enabling that candidate.
constexpr ConnectionSet BuildConnections(SlotKind candidate) {
  ConnectionSet set;
  for (const auto kind :
       {SlotKind::kCenter, SlotKind::kHorizontal, SlotKind::kSideX,
        SlotKind::kSideY, SlotKind::kCorner}) {
    const auto neighbors = AvailableNeighbors({0, 0, 0, kind});
    for (std::size_t i = 0; i < neighbors.size; ++i) {
      const auto &slot = neighbors.values[i];
      if (slot.kind == candidate)
        set.values[set.size++] = {kind, -slot.x, -slot.y, -slot.z};
    }
  }
  return set;
}
inline constexpr auto CENTER_CONNECTIONS = BuildConnections(SlotKind::kCenter);
inline constexpr auto HORIZONTAL_CONNECTIONS =
    BuildConnections(SlotKind::kHorizontal);
inline constexpr auto SIDEX_CONNECTIONS = BuildConnections(SlotKind::kSideX);
inline constexpr auto SIDEY_CONNECTIONS = BuildConnections(SlotKind::kSideY);
inline constexpr auto CORNER_CONNECTIONS = BuildConnections(SlotKind::kCorner);
inline std::span<const ConnectionDefinition>
ConnectionDefinitions(SlotKind kind) {
  const auto &set = kind == SlotKind::kCenter       ? CENTER_CONNECTIONS
                    : kind == SlotKind::kHorizontal ? HORIZONTAL_CONNECTIONS
                    : kind == SlotKind::kSideX      ? SIDEX_CONNECTIONS
                    : kind == SlotKind::kSideY      ? SIDEY_CONNECTIONS
                                                    : CORNER_CONNECTIONS;
  return {set.values.data(), set.size};
}
template <class IsOccupied>
bool HasConnection(int x, int y, int z, ObjectType type, Settings settings,
                   IsOccupied occupied, bool allowAdjacentVolumeFaces = true) {
  if (settings.sideRole == SideRole::kDoor)
    return (type == ObjectType::kSideX || type == ObjectType::kSideY) &&
           occupied(Slot{x, y, z,
                         type == ObjectType::kSideX ? SlotKind::kDoorwayX
                                                    : SlotKind::kDoorwayY});
  for (const auto &slot : OccupiedSlots(x, y, z, type, settings)) {
    if (slot.kind > SlotKind::kCorner)
      continue;
    for (const auto &connection : ConnectionDefinitions(slot.kind)) {
      if (!allowAdjacentVolumeFaces && connection.target == SlotKind::kCenter &&
          (slot.kind == SlotKind::kHorizontal ||
           slot.kind == SlotKind::kSideX || slot.kind == SlotKind::kSideY)) {
        // No-hit rays may select a volume's six flush faces, but not the
        // coplanar extensions that are available when directly targeting it.
        const int normal = FaceAxis(slot.kind);
        const std::array<int, 3> offset{connection.dx, connection.dy,
                                        connection.dz};
        bool flush = offset[normal] == 0 || offset[normal] == -1;
        for (int axis = 0; axis < 3; ++axis)
          if (axis != normal)
            flush &= offset[axis] == 0;
        if (!flush)
          continue;
      }
      if (occupied(Slot{slot.x + connection.dx, slot.y + connection.dy,
                        slot.z + connection.dz, connection.target}))
        return true;
    }
  }
  return false;
}
} // namespace SkyBuild::Grid
