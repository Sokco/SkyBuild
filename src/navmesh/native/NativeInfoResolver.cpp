#include "navmesh/native/NativeInfoResolver.h"
#include <atomic>
namespace SkyBuild::Navmesh {
namespace {
using LookupFunction = RE::BSNavmeshInfo *(*)(RE::BSNavmeshInfoMap *,
                                              std::uint32_t);
std::atomic<LookupFunction> originals[2];
std::uintptr_t hookedTable{};
thread_local NativeInfoResolver *active{};
} // namespace
NativeInfoResolver::NativeInfoResolver(
    const std::map<std::uint32_t, RE::BSNavmeshInfo *> &values, bool allowClear)
    : infos(values), allowClearedIdentity(allowClear) {
  if (active)
    throw std::runtime_error("Nested detached info resolution is unsupported");
  const REL::Relocation<RE::BSNavmeshInfoMap **> global{REL::RelocationID(515943, 402040)};
  registry = *global.get();
  if (!registry || infos.empty())
    throw std::runtime_error("Missing detached info registry context");
  for (const auto &[id, info] : infos)
    if (id < 0xFF000000 || !info || info->navMeshID != id || !info->navMesh)
      throw std::runtime_error("Invalid detached info resolution map");
  const auto table = *reinterpret_cast<const std::uintptr_t *>(registry);
  if (!hookedTable) {
    for (unsigned slot = 0; slot < 2; ++slot) {
      const auto function = *reinterpret_cast<const LookupFunction *>(
          table + (slot + 1) * sizeof(std::uintptr_t));
      if (!function)
        throw std::runtime_error("Missing native info lookup method");
      originals[slot].store(function, std::memory_order_release);
    }
    REL::safe_write(table + sizeof(std::uintptr_t),
                    reinterpret_cast<std::uintptr_t>(&FixLookup));
    REL::safe_write(table + 2 * sizeof(std::uintptr_t),
                    reinterpret_cast<std::uintptr_t>(&ExactLookup));
    hookedTable = table;
    spdlog::info("Installed scoped detached navmesh info resolution; "
                 "outside-scope lookups remain native.");
  } else if (hookedTable != table ||
             *reinterpret_cast<const LookupFunction *>(
                 table + sizeof(std::uintptr_t)) != &FixLookup ||
             *reinterpret_cast<const LookupFunction *>(
                 table + 2 * sizeof(std::uintptr_t)) != &ExactLookup) {
    throw std::runtime_error(
        "Native info lookup implementation changed after binding");
  }
  // Registration and private resolution are different contracts. Check both
  // in the actual scoped context so a future protocol cannot accidentally use
  // a private lookup result as evidence of publication again.
  for (const auto &[id, info] : infos)
    if (LookupRegistered(registry, id))
      throw std::runtime_error("Detached info is already natively registered");
  active = this;
  try {
    const auto &[id, info] = *infos.begin();
    if (registry->GetNavmeshInfo(id) != info || LookupRegistered(registry, id))
      throw std::runtime_error(
          "Private resolution/registration isolation failed");
  } catch (...) {
    active = nullptr;
    throw;
  }
}
NativeInfoResolver::~NativeInfoResolver() { active = nullptr; }
RE::BSNavmeshInfo *
NativeInfoResolver::LookupRegistered(RE::BSNavmeshInfoMap *map,
                                     std::uint32_t id) {
  if (!map)
    return nullptr;
  if (const auto native = originals[1].load(std::memory_order_acquire))
    return native(map, id);
  // No private resolver has installed hooks yet, so normal virtual dispatch
  // still supplies the authoritative native lookup.
  return map->GetNavmeshInfo(id);
}
RE::BSNavmeshInfo *NativeInfoResolver::Lookup(RE::BSNavmeshInfoMap *map,
                                              std::uint32_t id,
                                              unsigned method) {
  if (!active)
    return originals[method].load(std::memory_order_acquire)(map, id);
  if (map == active->registry && !id && active->allowClearedIdentity)
    return nullptr;
  const auto found = active->infos.find(id);
  if (map != active->registry || found == active->infos.end()) {
    ++active->misses;
    active->lastMissingID = id;
    return nullptr;
  }
  ++active->hits;
  return found->second;
}
RE::BSNavmeshInfo *NativeInfoResolver::FixLookup(RE::BSNavmeshInfoMap *map,
                                                 std::uint32_t id) {
  return Lookup(map, id, 0);
}
RE::BSNavmeshInfo *NativeInfoResolver::ExactLookup(RE::BSNavmeshInfoMap *map,
                                                   std::uint32_t id) {
  return Lookup(map, id, 1);
}
} // namespace SkyBuild::Navmesh
