#include "navmesh/native/NativeLocationCache.h"
#include "navmesh/native/LocationEpochMemo.h"
#include "navmesh/publication/PublicationMembership.h"
#include <MinHook.h>
#include <atomic>
#include <bit>
#include <cstring>
namespace SkyBuild::Navmesh::NativeLocationCache {
namespace {
using MeshLookup = bool (*)(RE::BSPathingLocation *,
                            RE::BSTSmartPointer<RE::BSNavmesh> &,
                            std::uint16_t &);
using MeshOnlyLookup = bool (*)(RE::BSPathingLocation *,
                                RE::BSTSmartPointer<RE::BSNavmesh> &);
MeshOnlyLookup meshOnlyLookup{};
using EdgeLookup = bool (*)(RE::BSPathingLocation *, void *);
using Resolve = bool (*)(RE::BSPathingLocation *, void *);
using Update = void (*)(RE::BSPathingLocation *, const RE::NiPoint3 *,
                        const RE::BSTSmartPointer<RE::BSPathingCell> *);
MeshLookup meshLookup{};
EdgeLookup edgeLookup{};
Resolve resolve{}, closest{};
Update update{};
std::atomic<std::shared_ptr<const Obsolete>> obsolete;
std::atomic_bool hasObsolete{};
std::atomic<std::shared_ptr<const Obsolete>> generated;
std::atomic_bool available{};
std::atomic_uint64_t geometryEpoch{}, nextGeometryEpoch{};
struct LocationState {
  std::array<std::uint32_t, 3> position;
  std::uintptr_t info{}, infoArray{}, cell{};
  std::uint16_t triangle{};
  std::uint8_t flags{}, clientData{};
  bool operator==(const LocationState &) const = default;
};
thread_local LocationEpochMemo<LocationState> resolvedLocations;
LocationState State(const RE::BSPathingLocation &location) {
  return {{std::bit_cast<std::uint32_t>(location.location.x),
           std::bit_cast<std::uint32_t>(location.location.y),
           std::bit_cast<std::uint32_t>(location.location.z)},
          reinterpret_cast<std::uintptr_t>(location.navMeshInfo),
          reinterpret_cast<std::uintptr_t>(location.navMeshInfoArray),
          reinterpret_cast<std::uintptr_t>(location.pathingCell.get()),
          location.triangle,
          location.flags,
          location.clientData};
}
bool HasCachedLocation(const RE::BSPathingLocation &location) {
  return location.navMeshInfo || location.navMeshInfoArray;
}
void Invalidate(RE::BSPathingLocation *location, std::uint64_t epoch) {
  if (!location)
    return;
  bool reset{};
  if (hasObsolete.load(std::memory_order_acquire)) {
    const auto cohort = obsolete.load(std::memory_order_acquire);
    reset = cohort && cohort->contains(location->navMeshInfo);
  }
  if (!reset && epoch && HasCachedLocation(*location))
    reset = resolvedLocations.NeedsResolution(
        epoch, reinterpret_cast<std::uintptr_t>(location), State(*location));
  if (!reset)
    return;
  location->navMeshInfo = nullptr;
  location->navMeshInfoArray = nullptr;
  location->triangle = 0xFFFF;
  location->flags = 0;
}
void Resolved(RE::BSPathingLocation *location, std::uint64_t epoch) noexcept {
  // Never stamp a call which crossed publication. No geometry epochs are
  // enabled by the legacy backend. The per-thread value memo allocates nothing.
  if (!epoch || !location || !HasCachedLocation(*location) ||
      geometryEpoch.load(std::memory_order_acquire) != epoch)
    return;
  resolvedLocations.Resolved(epoch, reinterpret_cast<std::uintptr_t>(location),
                             State(*location));
}
bool Mesh(RE::BSPathingLocation *location,
          RE::BSTSmartPointer<RE::BSNavmesh> &out, std::uint16_t &triangle) {
  const auto epoch = geometryEpoch.load(std::memory_order_acquire);
  Invalidate(location, epoch);
  const bool result = meshLookup(location, out, triangle);
  if (result)
    Resolved(location, epoch);
  return result;
}
bool MeshOnly(RE::BSPathingLocation *location,
              RE::BSTSmartPointer<RE::BSNavmesh> &out) {
  Invalidate(location, geometryEpoch.load(std::memory_order_acquire));
  const auto result = meshOnlyLookup(location, out);
  if (result && out && !out->unk110) {
    out.reset();
    return false;
  }
  return result;
}
bool Edge(RE::BSPathingLocation *location, void *out) {
  const auto epoch = geometryEpoch.load(std::memory_order_acquire);
  Invalidate(location, epoch);
  const bool result = edgeLookup(location, out);
  if (result)
    Resolved(location, epoch);
  return result;
}
bool ResolveLocation(RE::BSPathingLocation *location, void *filter) {
  const auto epoch = geometryEpoch.load(std::memory_order_acquire);
  Invalidate(location, epoch);
  const bool result = resolve(location, filter);
  if (result)
    Resolved(location, epoch);
  return result;
}
bool Closest(RE::BSPathingLocation *location, void *filter) {
  const auto epoch = geometryEpoch.load(std::memory_order_acquire);
  Invalidate(location, epoch);
  const bool result = closest(location, filter);
  if (result)
    Resolved(location, epoch);
  return result;
}
void UpdateLocation(RE::BSPathingLocation *location, const RE::NiPoint3 *point,
                    const RE::BSTSmartPointer<RE::BSPathingCell> *cell) {
  const auto epoch = geometryEpoch.load(std::memory_order_acquire);
  Invalidate(location, epoch);
  update(location, point, cell);
  Resolved(location, epoch);
}
} // namespace
bool Initialize() {
  if (available.load())
    return true;
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    return false;
  struct Binding {
    std::uint64_t id;
    void *hook;
    void **original;
  };
  const Binding bindings[]{{90358, reinterpret_cast<void *>(&MeshOnly),
                            reinterpret_cast<void **>(&meshOnlyLookup)},
                           {90360, reinterpret_cast<void *>(&Mesh),
                            reinterpret_cast<void **>(&meshLookup)},
                           {90363, reinterpret_cast<void *>(&Edge),
                            reinterpret_cast<void **>(&edgeLookup)},
                           {90367, reinterpret_cast<void *>(&ResolveLocation),
                            reinterpret_cast<void **>(&resolve)},
                           {90369, reinterpret_cast<void *>(&Closest),
                            reinterpret_cast<void **>(&closest)},
                           {90378, reinterpret_cast<void *>(&UpdateLocation),
                            reinterpret_cast<void **>(&update)}};
  std::vector<void *> targets;
  for (const auto &binding : bindings) {
    const REL::Relocation<std::uintptr_t> method{REL::ID(binding.id)};
    auto *target = reinterpret_cast<void *>(method.address());
    const auto status = MH_CreateHook(target, binding.hook, binding.original);
    if (status != MH_OK) {
      for (const auto created : targets)
        MH_RemoveHook(created);
      spdlog::error("Native location invalidation unavailable at {}: {}",
                    binding.id, MH_StatusToString(status));
      return false;
    }
    targets.push_back(target);
  }
  for (const auto target : targets)
    if (MH_QueueEnableHook(target) != MH_OK) {
      for (const auto created : targets)
        MH_RemoveHook(created);
      return false;
    }
  if (MH_ApplyQueued() != MH_OK) {
    for (const auto created : targets) {
      MH_DisableHook(created);
      MH_RemoveHook(created);
    }
    return false;
  }
  available.store(true);
  return true;
}
bool Available() { return available.load(); }
bool HasObsoleteInfos() noexcept {
  return hasObsolete.load(std::memory_order_acquire);
}
std::shared_ptr<const Obsolete> Prepare(const Obsolete &infos,
                                        const Obsolete &revived) {
  auto result = std::make_shared<Obsolete>();
  if (const auto existing = obsolete.load(std::memory_order_acquire))
    *result = *existing;
  *result = PublicationObsoleteInfos(*result, infos, revived);
  return result;
}
void RememberGenerated(const Obsolete &infos) {
  auto result = std::make_shared<Obsolete>(infos);
  if (const auto prior = generated.load(std::memory_order_acquire))
    result->insert(prior->begin(), prior->end());
  generated.store(std::move(result), std::memory_order_release);
}
Obsolete ObsoleteGenerated() {
  Obsolete result;
  const auto known = generated.load(std::memory_order_acquire);
  const auto departed = CaptureObsolete();
  if (known && departed)
    for (const auto *info : *known)
      if (departed->contains(info))
        result.insert(info);
  return result;
}
void Publish(std::shared_ptr<const Obsolete> infos) noexcept {
  const bool any = infos && !infos->empty();
  obsolete.store(std::move(infos), std::memory_order_release);
  hasObsolete.store(any, std::memory_order_release);
}
std::shared_ptr<const Obsolete> CaptureObsolete() noexcept {
  return obsolete.load(std::memory_order_acquire);
}
void PublishGeometryChange() noexcept {
  geometryEpoch.store(
      nextGeometryEpoch.fetch_add(1, std::memory_order_relaxed) + 1,
      std::memory_order_release);
}
void DisableGeometryTracking() noexcept {
  geometryEpoch.store(0, std::memory_order_release);
}
} // namespace SkyBuild::Navmesh::NativeLocationCache
