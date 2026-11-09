#include "navmesh/cuts/NativeCellQueryView.h"
#include "navmesh/cuts/ObstacleCandidates.h"
#include <MinHook.h>
#include <atomic>
#include <algorithm>
#include <spdlog/spdlog.h>
namespace SkyBuild::Navmesh {
namespace {
using GetMeshes = RE::NavMeshArray *(*)(RE::TESObjectCELL *);
std::atomic<GetMeshes> original{};
thread_local NativeCellQueryView *active{};
std::atomic_uint filteredQueries{};
} // namespace
NativeCellQueryView::NativeCellQueryView(
    const std::map<RE::TESObjectCELL *, RE::NavMeshArray> &views)
    : cells(views) {
  if (active || cells.empty() || cells.size() > 256)
    throw std::runtime_error("Invalid/nested private cell-query scope");
  for (const auto &[cell, list] : cells) {
    if (!cell || !cell->GetFormID() ||
        !cellIDs.insert(cell->GetFormID()).second || list.navMeshes.empty())
      throw std::runtime_error(
          "Private cell-query view has no parent or meshes");
    for (const auto &mesh : list.navMeshes)
      if (!mesh || !mesh->unk110)
        throw std::runtime_error("Private cell-query view has an unbound mesh");
  }
  Initialize();
  active = this;
}
void NativeCellQueryView::Initialize() {
  if (!original.load(std::memory_order_acquire)) {
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
      throw std::runtime_error("Could not initialize native cell-query hook");
    const REL::Relocation<std::uintptr_t> get{REL::RelocationID(18515, 18973)};
    auto *address = reinterpret_cast<void *>(get.address());
    GetMeshes trampoline{};
    if (MH_CreateHook(address, reinterpret_cast<void *>(&Lookup),
                      reinterpret_cast<void **>(&trampoline)) != MH_OK)
      throw std::runtime_error("Could not create native cell-query hook");
    original.store(trampoline, std::memory_order_release);
    if (MH_EnableHook(address) != MH_OK) {
      MH_RemoveHook(address);
      original.store(nullptr, std::memory_order_release);
      throw std::runtime_error("Could not enable native cell-query hook");
    }
  }
}
NativeCellQueryView::NativeCellQueryView() : pathQuery(true), previous(active) {
  // Preserve a surrounding private cohort or nested query snapshot.
  if (!active)
    active = this;
}
NativeCellQueryView::~NativeCellQueryView() {
  if (active == this)
    active = previous;
}
RE::NavMeshArray *NativeCellQueryView::Lookup(RE::TESObjectCELL *cell) {
  if (!active)
    return original.load(std::memory_order_acquire)(cell);
  if (active->pathQuery) {
    const auto found = active->cells.find(cell);
    if (found != active->cells.end())
      return &found->second;
    const auto live = original.load(std::memory_order_acquire)(cell);
    if (!live)
      return nullptr;
    if (std::ranges::all_of(live->navMeshes, [](const auto &mesh) {
          return mesh && mesh->unk110;
        }))
      return live;
    // Only an unusable native list requires a copy. Keep that filtered list
    // stable through repeated lookups in this synchronous search. Result
    // meshes have their own native strong references afterward.
    auto &view = active->cells[cell];
    CopyEligibleMeshCandidates(
        view.navMeshes, live->navMeshes,
        [](const auto &mesh) { return mesh && mesh->unk110; });
    if (filteredQueries.fetch_add(1, std::memory_order_relaxed) < 16)
      spdlog::warn("Native path query excluded {} unbound mesh candidates "
                   "from cell {:08X}; live list unchanged.",
                   live->navMeshes.size() - view.navMeshes.size(),
                   cell ? cell->GetFormID() : 0);
    return &view;
  }
  const auto found = active->cells.find(cell);
  if (found == active->cells.end()) {
    ++active->misses;
    // Native30578 skips null mesh lists and continues the remaining candidate
    // cells (1170: 4CCCEE -> 4CCE23). Outside cells are intentionally excluded
    // from this scoped query. A different pointer for a declared cell is stale
    // cohort coverage, never an outside cell or permission to use world lists.
    if (!cell || active->cellIDs.contains(cell->GetFormID()))
      ++active->cohortMisses;
    return nullptr;
  }
  ++active->hits;
  return &found->second;
}
} // namespace SkyBuild::Navmesh
