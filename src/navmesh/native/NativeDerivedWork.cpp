#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/cuts/NativeCutData.h"
#include <cstring>
#include "navmesh/cuts/RetiredCutWork.h"
#include "navmesh/cuts/NativeCutAudit.h"
namespace SkyBuild::Navmesh {
bool NativeDerivedWork::QueuesIdle() noexcept {
  try {
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
    const RE::BSReadLockGuard guard(*publication.get());
    const REL::Relocation<void **> manager{REL::RelocationID(514282, 400442)};
    const auto *bytes = static_cast<const std::byte *>(*manager.get());
    if (!bytes)
      return false;
    const RE::BSSpinLockGuard queueGuard(*reinterpret_cast<RE::BSSpinLock *>(
        const_cast<std::byte *>(bytes) + 0x208));
    const auto read = [bytes](std::size_t offset) {
      std::uint32_t value;
      std::memcpy(&value, bytes + offset, sizeof(value));
      return value;
    };
    const auto capacity = read(0x1B4), free = read(0x1B8);
    const auto audit = NativeCutAudit::Snapshot();
    return audit.available && !audit.observation.active && capacity <= 65536 &&
           free == capacity && !read(0x1E8) && !read(0x200);
  } catch (...) {
    return false;
  }
}
NativeDerivedWork::NativeDerivedWork() {
  const auto census = CaptureRegisteredCutData();
  if (census.preparedTasks || census.retainedCollections.at(0x1D8) ||
      census.retainedCollections.at(0x1F0))
    throw std::runtime_error("Native geometry tasks have not completed");
  const REL::Relocation<void **> manager{REL::RelocationID(514282, 400442)};
  auto *bytes = static_cast<std::byte *>(*manager.get());
  edges = reinterpret_cast<RE::BSTArray<Edge> *>(bytes + 0x210);
  triangles = reinterpret_cast<RE::BSTArray<Triangle> *>(bytes + 0x228);
  const auto capture = [](const auto &array, auto &copy) {
    if (array.size() > 65536 || array.size() > array.capacity() ||
        (array.size() && !array.data()))
      throw std::runtime_error("Native derived-work array is invalid");
    if (!array.empty())
      copy.assign(array.begin(), array.end());
  };
  capture(*edges, beforeEdges);
  capture(*triangles, beforeTriangles);
  edgeAllocation = edges->data();
  triangleAllocation = triangles->data();
  edgeCapacity = edges->capacity();
  triangleCapacity = triangles->capacity();
}
std::set<RE::BSNavmeshInfo *> NativeDerivedWork::BorrowedInfos() const {
  std::set<RE::BSNavmeshInfo *> result;
  for (const auto &edge : beforeEdges)
    if (edge.info)
      result.insert(edge.info);
  for (const auto &triangle : beforeTriangles)
    if (triangle.info)
      result.insert(triangle.info);
  return result;
}
std::size_t
NativeDerivedWork::Retire(const std::set<const RE::BSNavmeshInfo *> &infos) {
  if (!Unchanged())
    throw std::runtime_error(
        "Native deferred cut work changed before retirement");
  const auto retired = [&](const auto &record) {
    return infos.contains(record.info);
  };
  return RemoveRetiredCutWork(*edges, retired) +
         RemoveRetiredCutWork(*triangles, retired);
}
bool NativeDerivedWork::Unchanged() const {
  const auto same = [](const auto &array, const auto &before,
                       const void *allocation, std::uint32_t capacity) {
    return array.data() == allocation && array.capacity() == capacity &&
           array.size() == before.size() &&
           (before.empty() ||
            !std::memcmp(array.data(), before.data(),
                         before.size() * sizeof(before.front())));
  };
  return same(*edges, beforeEdges, edgeAllocation, edgeCapacity) &&
         same(*triangles, beforeTriangles, triangleAllocation,
              triangleCapacity);
}
} // namespace SkyBuild::Navmesh
