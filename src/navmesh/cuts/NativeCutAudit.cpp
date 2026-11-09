#include "navmesh/cuts/NativeCutAudit.h"
namespace SkyBuild::Navmesh::NativeCutAudit {
namespace {
// The inspected ObstacleTaskData dispatch vtable204262 slot2 (30612) is
// void Execute(task). It clones source geometry via30245, sharing undo objects
// via30269, performs cuts, then queues its result through30456.
using Execute = void (*)(void *);
Execute original{};
WriterObserver observer;
std::atomic_bool available{};
void ExecuteObserved(void *task) {
  observer.Enter();
  struct Leave {
    ~Leave() { observer.Leave(); }
  } leave;
  original(task);
}
} // namespace
void Initialize() {
  if (available.load())
    return;
  const REL::Relocation<std::uintptr_t> table{REL::RelocationID(254794, 204262)};
  const REL::Relocation<std::uintptr_t> method{REL::RelocationID(29795, 30612)};
  const auto slot = table.address() + 2 * sizeof(std::uintptr_t);
  if (*reinterpret_cast<const std::uintptr_t *>(slot) != method.address()) {
    spdlog::warn("Native cut writer audit unavailable: task execution slot "
                 "replaced. No audit hook installed.");
    return;
  }
  original = reinterpret_cast<Execute>(method.address());
  REL::safe_write(slot, reinterpret_cast<std::uintptr_t>(&ExecuteObserved));
  available.store(true);
  spdlog::info(
      "Installed passive ObstacleTaskData writer audit (204262/30612). It "
      "neither blocks writers nor certifies capture coherence.");
}
Report Snapshot() noexcept { return {observer.Snapshot(), available.load()}; }
} // namespace SkyBuild::Navmesh::NativeCutAudit
