#include "navmesh/cuts/NativeCutOwnerTransfer.h"
#include "navmesh/native/RetainedReferenceSlot.h"
#include <memory>
#include <stdexcept>
#include <type_traits>
namespace SkyBuild::Navmesh {
NativeCutOwnerTransfer::NativeCutOwnerTransfer(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &cohort,
    const Mapping &mapping) {
  std::set<void *> visited;
  const auto add = [&](void *storage) {
    RE::NiRefObject *before{};
    std::memcpy(&before, storage, sizeof(before));
    const auto target = mapping.find(before);
    if (target == mapping.end() || !visited.insert(storage).second)
      return;
    if (!target->second)
      throw std::runtime_error("Null cut transfer target");
    slots.push_back({storage, RE::NiPointer<RE::NiRefObject>{before},
                     RE::NiPointer<RE::NiRefObject>{target->second}});
  };
  using Owners =
      std::remove_pointer_t<decltype(RE::BSNavmesh::triangleToObstacleMap)>;
  for (const auto &mesh : cohort) {
    if (mesh->triangleToObstacleMap)
      for (auto &[face, owner] : *mesh->triangleToObstacleMap)
        add(std::addressof(owner));
    for (const auto &undo : mesh->obstacles) {
      auto *bytes = reinterpret_cast<std::byte *>(undo.get());
      add(bytes + 0x18);
      for (auto &[face, owner] : *reinterpret_cast<Owners *>(bytes + 0x60))
        add(std::addressof(owner));
    }
  }
}
NativeCutOwnerTransfer::~NativeCutOwnerTransfer() {
  if (applied)
    for (const auto &slot : slots)
      ReplaceRetainedReferenceSlot(slot.storage, slot.before.get());
}
void NativeCutOwnerTransfer::Apply() noexcept {
  assert(!applied);
  for (const auto &slot : slots)
    ReplaceRetainedReferenceSlot(slot.storage, slot.after.get());
  applied = true;
}
void NativeCutOwnerTransfer::Commit() noexcept {
  assert(applied);
  applied = false;
}
} // namespace SkyBuild::Navmesh
