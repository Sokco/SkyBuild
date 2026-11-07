#pragma once
#include <algorithm>
#include <array>
#include <string_view>
namespace SkyBuild::Containers {
enum class DisplayType {
  None,
  Sword,
  Dagger,
  WarAxe,
  Mace,
  GreatSword,
  BattleAxe,
  Warhammer,
  Bow,
  Staff,
  Crossbow,
  Shield
};
inline std::string_view NodeName(DisplayType type) {
  switch (type) {
  case DisplayType::Sword:
    return "SwordPivot01";
  case DisplayType::Dagger:
    return "DaggerPivot01";
  case DisplayType::WarAxe:
    return "WarAxePivot01";
  case DisplayType::Mace:
    return "MacePivot01";
  case DisplayType::GreatSword:
    return "GreatSwordPivot01";
  case DisplayType::BattleAxe:
    return "BattleAxePivot01";
  case DisplayType::Warhammer:
    return "WarhammerPivot01";
  case DisplayType::Bow:
    return "BowPivot01";
  case DisplayType::Crossbow:
    return "CrossbowPivot01";
  case DisplayType::Staff:
    return "StaffPivot01";
  case DisplayType::Shield:
    return "ShieldPivot01";
  default:
    return {};
  }
}
// Prefer the authored item pivot, then a related shape, then an explicit
// generic display point. Finally use another known display pivot. Never use
// the scene root or an arbitrary bone as an accidental fallback.
template <class Exists>
std::string_view SelectPivot(DisplayType type, Exists exists) {
  if (type == DisplayType::None)
    return {};
  const auto exact = NodeName(type);
  if (exists(exact))
    return exact;
  const auto tryType = [&](DisplayType candidate) -> std::string_view {
    const auto name = NodeName(candidate);
    return exists(name) ? name : std::string_view{};
  };
  DisplayType related = DisplayType::None;
  switch (type) {
  case DisplayType::BattleAxe:
    related = DisplayType::Warhammer;
    break;
  case DisplayType::Warhammer:
    related = DisplayType::BattleAxe;
    break;
  case DisplayType::Bow:
    related = DisplayType::Crossbow;
    break;
  case DisplayType::Crossbow:
    related = DisplayType::Bow;
    break;
  case DisplayType::Dagger:
    related = DisplayType::Sword;
    break;
  case DisplayType::WarAxe:
    related = DisplayType::Mace;
    break;
  case DisplayType::Mace:
    related = DisplayType::WarAxe;
    break;
  case DisplayType::GreatSword:
  case DisplayType::Staff:
    related = DisplayType::Sword;
    break;
  default:
    break;
  }
  if (related != DisplayType::None)
    if (const auto name = tryType(related); !name.empty())
      return name;
  for (const auto name : {"ItemPivot01", "WeaponPivot01", "DisplayPivot01"})
    if (exists(name))
      return name;
  constexpr std::array candidates{
      DisplayType::Sword,  DisplayType::GreatSword, DisplayType::WarAxe,
      DisplayType::Mace,   DisplayType::BattleAxe,  DisplayType::Warhammer,
      DisplayType::Dagger, DisplayType::Bow,        DisplayType::Crossbow,
      DisplayType::Staff,  DisplayType::Shield};
  for (const auto candidate : candidates)
    if (const auto name = tryType(candidate); !name.empty())
      return name;
  return {};
}
// Skyrim's world/inventory weapon presentation excludes scabbards. Match
// complete branch names only so ordinary hilt/guard geometry is preserved.
inline bool IsScabbardBranch(std::string_view name) {
  const auto equals = [name](std::string_view expected) {
    if (name.size() != expected.size())
      return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
      const auto c = name[i];
      const auto lower = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
      if (lower != expected[i])
        return false;
    }
    return true;
  };
  return equals("scb") || equals("scbleft");
}
inline bool SafeContainer(bool respawns, unsigned startingEntries) {
  return !respawns && startingEntries == 0;
}
// Weapon layout is independent of the additional shield on Single.
inline std::array<bool, 3> ActiveRackPivots(int weapons) {
  if (weapons <= 0)
    return {false, false, false};
  return {weapons != 2, weapons >= 2, weapons >= 2};
}
} // namespace SkyBuild::Containers
