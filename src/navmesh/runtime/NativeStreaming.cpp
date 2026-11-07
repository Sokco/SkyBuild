#include "navmesh/runtime/NativeStreaming.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/native/NativeSearchAudit.h"
#include "navmesh/runtime/NativeSynchronization.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include <MinHook.h>
#include <atomic>
namespace SkyBuild::Navmesh::NativeStreaming {
namespace {
// 1.6.1170 ID19130: full TESObjectCELL in RCX, boolean in DL, no result.
// At entry the cell is still attached. Native code then emits type2 at19361,
// changes state7/6 to5, detaches references, and reaches loaded state4.
using Detach = void (*)(RE::TESObjectCELL *, bool);
Detach detach{};
std::atomic_bool available{};
bool HoldsNavigationLocks() {
  const auto thread = GetCurrentThreadId();
  bool held = false;
  for (const auto id : {402045ULL, 402042ULL}) {
    const REL::Relocation<std::uintptr_t> lock{REL::ID(id)};
    const auto owner =
        reinterpret_cast<const volatile std::uint32_t *>(lock.address());
    held |= owner[0] == thread && owner[1] != 0;
  }
  const REL::Relocation<RE::BSNavmeshInfoMap **> global{REL::ID(402040)};
  if (const auto map = static_cast<RE::NavMeshInfoMap *>(*global.get())) {
    const auto owner =
        reinterpret_cast<const volatile std::uint32_t *>(&map->mapLock);
    held |= owner[0] == thread && owner[1] != 0;
  }
  return held;
}
void ObserveDetach(RE::TESObjectCELL *cell, bool releaseResources) {
  try {
    const bool held = HoldsNavigationLocks();
    // The journal includes every cell owning forms in this publication,
    // including original portal neighbors. Outside cells do not own any of
    // its native cell arrays, so their detach does not retire this cohort.
    if (NativeReplacement::Installed() &&
        (!cell || NativeReplacement::DependsOnCell(cell->GetFormID()))) {
      if (held || !NativeSynchronization::RetireBeforeStreaming()) {
        spdlog::critical(
            "Navigation retirement rejected before cell {:08X}: {}",
            cell ? cell->GetFormID() : 0, NativeReplacement::Status());
      }
    }
    if (!NativeReplacement::Installed() || !cell ||
        NativeReplacement::DependsOnCell(cell->GetFormID()))
      NotifyStreamingDetach();
  } catch (...) {
    spdlog::critical(
        "Navigation retirement raised an exception before cell teardown.");
  }
  detach(cell, releaseResources);
}
} // namespace
bool Initialize() {
  if (available.load())
    return true;
  const auto status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    return false;
  const REL::Relocation<std::uintptr_t> target{REL::ID(19130)};
  auto *address = reinterpret_cast<void *>(target.address());
  if (MH_CreateHook(address, reinterpret_cast<void *>(&ObserveDetach),
                    reinterpret_cast<void **>(&detach)) != MH_OK)
    return false;
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    return false;
  }
  available.store(true);
  spdlog::debug("Bound pre-detach cell lifecycle retirement (19130); originals "
                "restored through an owned search pause before teardown.");
  return true;
}
} // namespace SkyBuild::Navmesh::NativeStreaming
