#pragma once
#include <map>
#include <set>
#include <vector>
namespace SkyBuild::Navmesh {
// Caller holds the native pause/locks and has validated the cohort with
// CaptureNativeCutReferences. Prepare every slot before Apply; Commit keeps
// the exchange, otherwise destruction restores all original references.
class NativeCutOwnerTransfer {
public:
  using Mapping = std::map<RE::NiRefObject *, RE::NiRefObject *>;
  NativeCutOwnerTransfer(
      const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &cohort,
      const Mapping &mapping);
  ~NativeCutOwnerTransfer();
  NativeCutOwnerTransfer(const NativeCutOwnerTransfer &) = delete;
  NativeCutOwnerTransfer &operator=(const NativeCutOwnerTransfer &) = delete;
  void Apply() noexcept;
  void Commit() noexcept;
  [[nodiscard]] std::size_t Size() const { return slots.size(); }

private:
  struct Slot {
    void *storage;
    RE::NiPointer<RE::NiRefObject> before, after;
  };
  std::vector<Slot> slots;
  bool applied{};
};
} // namespace SkyBuild::Navmesh
