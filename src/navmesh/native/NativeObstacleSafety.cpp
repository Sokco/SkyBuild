#include "navmesh/native/NativeObstacleSafety.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/cuts/ObstacleCandidates.h"
#include "navmesh/cuts/NativeCellQueryView.h"
#include "navmesh/native/NativeLocationCache.h"
#include <MinHook.h>
#include <atomic>

namespace SkyBuild::Navmesh::NativeObstacleSafety {
namespace {
// Native 30578 fills a 0x20 BSScrapArray of full NavMesh form pointers.
// Caller 30476 obtains BSNavmesh::unk110 and passes it to 30478 unchecked.
// The reported crash is the second call, at 30476+0x184, with a null backlink.
using Meshes = RE::BSScrapArray<RE::BSTSmartPointer<RE::NavMesh>>;
static_assert(sizeof(Meshes) == 0x20);
using Enumerate = bool (*)(void *, Meshes *);
Enumerate enumerate{};
// 30696: location in RCX, radius in XMM1, owned mesh results in R8,
// same-cell/search option in R9B, boolean result. It enumerates cell lists
// then dereferences each candidate's info without checking for null.
using PathCandidates = bool (*)(RE::BSPathingLocation *, float, void *, bool);
PathCandidates pathCandidates{};
bool EnumeratePathCandidates(RE::BSPathingLocation *location, float radius,
                             void *output, bool option) {
  if (!NativeLocationCache::HasObsoleteInfos())
    return pathCandidates(location, radius, output, option);
  NativeCellQueryView view;
  return pathCandidates(location, radius, output, option);
}
std::atomic_bool available{};
std::atomic_size_t rejected{};
bool EnumerateValid(void *input, Meshes *output) {
  const auto result = enumerate(input, output);
  if (!result || !output)
    return result;
  // Remove only unusable scratch query results. Never change a cell's mesh
  // list, registry, geometry, cut stack, or active navigation here.
  FilterObstacleCandidates(
      *output, [](const auto &mesh) { return mesh && mesh->unk110; },
      [](const auto &mesh) {
        const auto count = ++rejected;
        if (count <= 16)
          spdlog::warn("Native obstacle query skipped mesh {:08X}: null "
                       "navigation info ({} vertices, {} triangles, flags "
                       "{:08X}). Active navigation unchanged.",
                       mesh ? mesh->GetFormID() : 0,
                       mesh ? mesh->vertices.size() : 0,
                       mesh ? mesh->triangles.size() : 0,
                       mesh ? mesh->GetFormFlags() : 0);
      });
  return result;
}
} // namespace
bool Initialize() {
  if (available.load())
    return true;
  const auto status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    return false;
  try {
    NativeCellQueryView::Initialize();
  } catch (const std::exception &error) {
    spdlog::error("Native path candidate view unavailable: {}", error.what());
    return false;
  }
  const REL::Relocation<std::uintptr_t> target{REL::ID(30578)};
  auto *address = reinterpret_cast<void *>(target.address());
  if (MH_CreateHook(address, reinterpret_cast<void *>(&EnumerateValid),
                    reinterpret_cast<void **>(&enumerate)) != MH_OK)
    return false;
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    return false;
  }
  const REL::Relocation<std::uintptr_t> pathTarget{REL::ID(30696)};
  auto *pathAddress = reinterpret_cast<void *>(pathTarget.address());
  if (MH_CreateHook(pathAddress,
                    reinterpret_cast<void *>(&EnumeratePathCandidates),
                    reinterpret_cast<void **>(&pathCandidates)) != MH_OK ||
      MH_EnableHook(pathAddress) != MH_OK) {
    MH_RemoveHook(pathAddress);
    MH_DisableHook(address);
    MH_RemoveHook(address);
    return false;
  }
  available.store(true);
  spdlog::info("Bound native obstacle/path candidate validation (30578/30696); "
               "navigation changes remain command-only.");
  return true;
}
} // namespace SkyBuild::Navmesh::NativeObstacleSafety
