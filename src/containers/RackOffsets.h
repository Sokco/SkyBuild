#pragma once
#include "containers/RackRules.h"
#include <array>
namespace SkyBuild::Containers {
struct RackOffset {
  std::array<float, 3> translation{};
  std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
};
// Measured from wrsingle01.nif, relative to StaffPivot01, not rack/world axes.
// Delta = inverse(staff) * weapon. Unit scale throughout the source NIF.
inline RackOffset StaffRelativeOffset(DisplayType type) {
  switch (type) {
  case DisplayType::Sword:
    return {
        {0.0F, 22.5901938F, 1.04891699F},
        {1.0F, 0.0F, 0.0F, 0.0F, -0.99999994F, 0.0F, 0.0F, 0.0F, -0.99999994F}};
  case DisplayType::Dagger:
    return {{0.0F, 9.06840396F, 1.33208418F},
            {1.0F, 0.0F, 0.0F, 0.0F, -0.995396171F, 0.0958465893F, 0.0F,
             -0.0958465893F, -0.995396171F}};
  case DisplayType::WarAxe:
    return {{0.0F, -12.6799402F, 1.04886447F},
            {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F}};
  case DisplayType::Mace:
    return {
        {0.0F, 18.3233633F, 1.04890793F},
        {1.0F, 0.0F, 0.0F, 0.0F, -0.99999994F, 0.0F, 0.0F, 0.0F, -0.99999994F}};
  case DisplayType::GreatSword:
    return {{0.0F, 25.7562781F, 1.04891982F},
            {1.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 0.0F, -1.0F}};
  case DisplayType::BattleAxe:
    return {
        {0.0F, -5.96375305F, 0.34429923F},
        {1.0F, 0.0F, 0.0F, 0.0F, 0.99999994F, 0.0F, 0.0F, 0.0F, 0.99999994F}};
  case DisplayType::Warhammer:
    return {{0.0F, -11.8463748F, 0.278284925F},
            {1.0F, 0.0F, 0.0F, 0.0F, 0.996917378F, 0.0784578947F, 0.0F,
             -0.0784578947F, 0.996917378F}};
  case DisplayType::Bow:
  case DisplayType::Crossbow: // No crossbow pivot in the source; use bow fit.
    return {
        {0.0995292664F, 8.10874922F, 0.19490967F},
        {1.0F, 0.0F, 0.0F, 0.0F, 0.99999994F, 0.0F, 0.0F, 0.0F, 0.99999994F}};
  default:
    return {}; // Staff, shield (not present in source), and unsupported types.
  }
}
} // namespace SkyBuild::Containers
