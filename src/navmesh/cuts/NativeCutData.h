#pragma once
#include <array>
#include <vector>
namespace SkyBuild::Navmesh {
class NativeGraphDraft;
struct NativeCutSnapshot;
// Native-layer baseline only; never sent to workers or serialized. Captured
// while search and publication are paused, with independently retained inputs.
struct NativeCutDataSource {
  std::uint32_t meshID{};
  // Previous native reference state (+130): preserve/retire its memberships,
  // but never apply its historical shape as a fresh cutting input.
  bool retainedOnly{};
  // Manager +160 inputs drive closed-door access records (native operation
  // 0x30), not obstacle geometry (0x10). Keep their discovery/memberships so
  // Skyrim can remove/update those records, but never materialize them as cuts.
  bool cutsGeometry{true};
  RE::NiPointer<RE::NiRefObject> original, rigidBody, auxiliary, ownerGroup;
  void *owner{}; // Native group pointer, kept alive by ownerGroup.
  const void *affectedAllocation{};
  std::array<std::byte, 0x80> values{}; // value fields +0x20 through +0x9F
  std::vector<std::uint32_t> affectedMeshes;
  // Borrowed info pointers, decoded against the loaded graph during capture.
  // Diagnostic clones copy these values but never dereference/use this list.
  std::vector<RE::BSNavmeshInfo *> affectedInfos;
};
[[nodiscard]] std::vector<NativeCutDataSource> CaptureNativeCutData(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &loaded,
    const NativeCutSnapshot *captured = nullptr);
// Read-only registry input census, independent of current cut/undo membership.
// Owns input, body, auxiliary and owner-group references; infos remain
// borrowed.
struct RegisteredCutCensus {
  std::vector<NativeCutDataSource> inputs;
  // Stable registry observation is not a certificate that all producers
  // drained.
  std::map<std::size_t, std::uint32_t> retainedCollections;
  std::uint32_t preparedTasks{};
  [[nodiscard]] bool NoRetainedWorkObserved() const {
    return !preparedTasks &&
           std::all_of(retainedCollections.begin(), retainedCollections.end(),
                       [](const auto &entry) { return entry.second == 0; });
  }
};
[[nodiscard]] RegisteredCutCensus CaptureRegisteredCutData();
// Detached data clone. It does not construct processor scratch state, attach
// undo to a mesh, run cutting, or enter the obstacle manager.
class NativeCutDataDraft {
public:
  [[nodiscard]] static std::unique_ptr<NativeCutDataDraft>
  Create(const NativeCutDataSource &source, std::string &error);
  [[nodiscard]] bool Matches() const;

private:
  friend class NativeGraphDraft;
  friend class NativePublication;
  void
  RemapAffectedInfos(const std::map<std::uint32_t, RE::BSNavmeshInfo *> &infos);
  void ClearAffectedInfos();
  NativeCutDataSource expected;
  RE::NiPointer<RE::NiRefObject> data;
};
} // namespace SkyBuild::Navmesh
