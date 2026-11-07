#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace SkyBuild::Grid {
enum class SnapMode : std::uint8_t { kNone, kBasic, kSide, kCorner };
enum Occupancy : std::uint8_t {
  kCenter = 1,
  kBottom = 2,
  kTop = 4,
  kCorners = 8,
  kSides = 16
};
using OccupancyMask = std::uint8_t;
inline constexpr OccupancyMask ALL_OCCUPANCY = 31;
enum class SideRole : std::uint8_t { kWall, kDoorway, kDoor };
struct Settings {
  OccupancyMask occupy{kCenter};
  SideRole sideRole{SideRole::kWall};
  bool operator==(const Settings &) const = default;
};
inline bool HasOnlyHorizontalFaces(Settings s) {
  return (s.occupy & (kBottom | kTop)) &&
         !(s.occupy & (kCenter | kCorners | kSides));
}
inline bool HasVolume(Settings s) {
  return (s.occupy & (kCenter | kTop | kCorners | kSides)) != 0;
}
enum class ObjectType : std::uint8_t { kBasic, kSideX, kSideY, kCorner, kNone };
enum class SlotKind : std::uint8_t {
  kCenter,
  kHorizontal,
  kSideX,
  kSideY,
  kCorner,
  kDoorwayX,
  kDoorwayY,
  kDoorX,
  kDoorY
};
struct Slot {
  std::int32_t x{}, y{}, z{};
  SlotKind kind{SlotKind::kCenter};
  constexpr bool operator==(const Slot &) const = default;
};
struct Slots {
  std::array<Slot, 11> values{};
  std::size_t size{};
  const Slot *begin() const { return values.data(); }
  const Slot *end() const { return values.data() + size; }
};
inline Slots OccupiedSlots(std::int32_t x, std::int32_t y, std::int32_t z,
                           ObjectType type, Settings settings = {}) {
  Slots slots;
  const auto add = [&](int a, int b, int c, SlotKind kind) {
    slots.values[slots.size++] = {a, b, c, kind};
  };
  if (type == ObjectType::kNone)
    return slots;
  if (type == ObjectType::kSideX) {
    if (settings.sideRole == SideRole::kDoor)
      add(x, y, z, SlotKind::kDoorX);
    else {
      add(x, y, z, SlotKind::kSideX);
      if (settings.sideRole == SideRole::kDoorway)
        add(x, y, z, SlotKind::kDoorwayX);
    }
    return slots;
  }
  if (type == ObjectType::kSideY) {
    if (settings.sideRole == SideRole::kDoor)
      add(x, y, z, SlotKind::kDoorY);
    else {
      add(x, y, z, SlotKind::kSideY);
      if (settings.sideRole == SideRole::kDoorway)
        add(x, y, z, SlotKind::kDoorwayY);
    }
    return slots;
  }
  if (type == ObjectType::kCorner) {
    add(x, y, z, SlotKind::kCorner);
    return slots;
  }
  if (settings.occupy & kCenter)
    add(x, y, z, SlotKind::kCenter);
  if (settings.occupy & kBottom)
    add(x, y, z, SlotKind::kHorizontal);
  if (settings.occupy & kTop)
    add(x, y, z + 1, SlotKind::kHorizontal);
  if (settings.occupy & kCorners)
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 2; ++b)
        add(x + a, y + b, z, SlotKind::kCorner);
  if (settings.occupy & kSides) {
    add(x, y, z, SlotKind::kSideX);
    add(x + 1, y, z, SlotKind::kSideX);
    add(x, y, z, SlotKind::kSideY);
    add(x, y + 1, z, SlotKind::kSideY);
  }
  return slots;
}
} // namespace SkyBuild::Grid
