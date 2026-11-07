#pragma once
#include "building/BuildCostData.h"
#include "building/BuildingManager.h"
#include "ui/settings/Settings.h"
#include <deque>
namespace SkyBuild::BuildCosts {
inline std::vector<Material>
EffectiveMaterials(const std::vector<Material> &materials) {
  return ConfiguredCosts(materials, Settings::SandboxMode(),
                         Settings::BuildingCostMultiplier());
}

struct Source {
  RE::NiPointer<RE::TESObjectREFR> owner;
  RE::TESObjectREFR::InventoryItemMap inventory;
};
inline std::deque<Source>
GatherSources(const std::set<RE::TESBoundObject *> &items,
              RE::PlayerCharacter *player) {
  std::vector<RE::NiPointer<RE::TESObjectREFR>> containers;
  if (auto *world = RE::TES::GetSingleton())
    world->ForEachReferenceInRange(player, 2048.F, [&](RE::TESObjectREFR *ref) {
      if (ref && !ref->IsDeleted() && !ref->IsDisabled() && !ref->IsLocked() &&
          ref->GetBaseObject() &&
          ref->GetBaseObject()->As<RE::TESObjectCONT>() &&
          (ref->GetParentCell() == player->GetParentCell() ||
           (player->GetWorldspace() &&
            ref->GetWorldspace() == player->GetWorldspace())) &&
          (ref->IsAnOwner(player, true, true) ||
           (BuildingManager::IsPlacedObject(ref->GetFormID()) &&
            !ref->GetOwner())))
        containers.emplace_back(ref);
      return RE::BSContainer::ForEachResult::kContinue;
    });
  std::ranges::sort(containers, [&](const auto &a, const auto &b) {
    return a->GetPosition().GetDistance(player->GetPosition()) <
           b->GetPosition().GetDistance(player->GetPosition());
  });
  std::deque<Source> sources;
  const auto addSource = [&](RE::TESObjectREFR *owner) {
    sources.push_back({RE::NiPointer<RE::TESObjectREFR>{owner},
                       owner->GetInventory([&](RE::TESBoundObject &item) {
                         return items.contains(&item);
                       })});
  };
  addSource(player);
  for (const auto &container : containers)
    addSource(container.get());
  return sources;
}
inline int UsableCount(const Source &source, RE::TESBoundObject *item) {
  const auto entry = source.inventory.find(item);
  const bool usable = entry != source.inventory.end() && entry->second.second &&
                      !entry->second.second->IsQuestObject() &&
                      !entry->second.second->IsWorn() &&
                      !entry->second.second->IsFavorited();
  return usable ? std::max(0, entry->second.first) : 0;
}
inline std::vector<Requirement>
CheckAvailability(const std::vector<Material> &materials) {
  const auto effective = EffectiveMaterials(materials);
  std::vector<Requirement> result;
  if (effective.empty())
    return result;
  auto *player = RE::PlayerCharacter::GetSingleton();
  auto *handler = RE::TESDataHandler::GetSingleton();
  std::set<RE::TESBoundObject *> items;
  std::vector<RE::TESBoundObject *> resolved;
  for (const auto &material : effective) {
    auto *item = handler ? handler->LookupForm<RE::TESBoundObject>(
                               material.local, material.plugin)
                         : nullptr;
    if (item && (!item->IsInventoryObject() || item->IsDeleted()))
      item = nullptr;
    resolved.push_back(item);
    if (item)
      items.insert(item);
    result.push_back({material, 0});
  }
  if (!player || items.empty())
    return result;
  const auto sources = GatherSources(items, player);
  for (std::size_t i = 0; i < result.size(); ++i)
    if (resolved[i])
      for (const auto &source : sources)
        result[i].available = static_cast<int>(std::min<std::int64_t>(
            INT32_MAX, std::int64_t(result[i].available) +
                           UsableCount(source, resolved[i])));
  return result;
}
// Resolve every material before removal, so unavailable resources cannot cause
// a partial refund after the building piece has already been disabled.
class Refund {
public:
  bool Prepare(const std::vector<Material> &materials, bool scaled = false) {
    const auto effective =
        ConfiguredCosts(materials, Settings::SandboxMode(),
                        Settings::BuildingCostMultiplier(), scaled);
    auto *handler = RE::TESDataHandler::GetSingleton();
    player = RE::PlayerCharacter::GetSingleton();
    if (!handler || !player)
      return false;
    items.clear();
    for (const auto &material : effective) {
      auto *item = handler->LookupForm<RE::TESBoundObject>(material.local,
                                                           material.plugin);
      if (!item || !item->IsInventoryObject() || item->IsDeleted()) {
        RE::SendHUDMessage::ShowHUDMessage(
            "SkyBuild: a refund material is unavailable; removal cancelled.");
        return false;
      }
      items.emplace_back(item, material.amount);
    }
    return true;
  }
  void Grant() {
    for (const auto &[item, amount] : items)
      player->AddObjectToContainer(item, nullptr, amount, nullptr);
    items.clear();
  }

private:
  RE::PlayerCharacter *player{};
  std::vector<std::pair<RE::TESBoundObject *, int>> items;
};
// A payment is rolled back if placement fails before Commit(). No idle scans.
class Payment {
public:
  Payment() = default;
  Payment(const Payment &) = delete;
  Payment &operator=(const Payment &) = delete;
  ~Payment() {
    if (!committed)
      for (const auto &part : paid)
        part.owner->AddObjectToContainer(part.item, nullptr, part.amount,
                                         nullptr);
  }
  bool Pay(const std::vector<Material> &materials, bool scaled = false) {
    const auto effective =
        ConfiguredCosts(materials, Settings::SandboxMode(),
                        Settings::BuildingCostMultiplier(), scaled);
    if (effective.empty())
      return true;
    auto *player = RE::PlayerCharacter::GetSingleton();
    auto *handler = RE::TESDataHandler::GetSingleton();
    if (!player || !handler)
      return false;
    struct Required {
      RE::TESBoundObject *item;
      int amount;
    };
    std::vector<Required> required;
    std::set<RE::TESBoundObject *> items;
    for (const auto &material : effective) {
      auto *item = handler->LookupForm<RE::TESBoundObject>(material.local,
                                                           material.plugin);
      if (!item || !item->IsInventoryObject() || item->IsDeleted()) {
        RE::SendHUDMessage::ShowHUDMessage(
            "SkyBuild: a required material is unavailable.");
        return false;
      }
      required.push_back({item, material.amount});
      items.insert(item);
    }
    auto sources = GatherSources(items, player);
    std::vector<Part> plan;
    for (const auto &requirement : required) {
      std::vector<int> counts;
      for (const auto &source : sources) {
        counts.push_back(UsableCount(source, requirement.item));
      }
      const auto allocation = Allocate(requirement.amount, counts);
      if (!allocation) {
        RE::SendHUDMessage::ShowHUDMessage(
            std::format(
                "SkyBuild: need {} {} (inventory and nearby owned containers).",
                requirement.amount, requirement.item->GetName())
                .c_str());
        return false;
      }
      for (std::size_t i = 0; i < sources.size(); ++i)
        if ((*allocation)[i])
          plan.push_back(
              {sources[i].owner, requirement.item, (*allocation)[i]});
    }
    paid.reserve(plan.size());
    for (const auto &part : plan) {
      const auto count = [&] {
        const auto counts = part.owner->GetInventoryCounts(
            [&](RE::TESBoundObject &item) { return &item == part.item; });
        const auto found = counts.find(part.item);
        return found == counts.end() ? 0 : found->second;
      };
      const auto before = count();
      part.owner->RemoveItem(part.item, part.amount,
                             RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
      const int removed = std::clamp(before - count(), 0, part.amount);
      if (removed)
        paid.push_back({part.owner, part.item, removed});
      if (removed != part.amount) {
        RE::SendHUDMessage::ShowHUDMessage(
            "SkyBuild: materials changed; placement cancelled.");
        return false;
      }
    }
    return true;
  }
  void Commit() { committed = true; }

private:
  struct Part {
    RE::NiPointer<RE::TESObjectREFR> owner;
    RE::TESBoundObject *item;
    int amount;
  };
  std::vector<Part> paid;
  bool committed{};
};
} // namespace SkyBuild::BuildCosts
