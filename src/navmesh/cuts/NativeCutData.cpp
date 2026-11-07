#include "navmesh/cuts/NativeCutData.h"
#include "navmesh/cuts/CutLifecycle.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/cuts/NativeCutAudit.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include <cstring>
namespace SkyBuild::Navmesh {
namespace {
template <class T> T Read(const std::byte *source, std::size_t offset) {
  T result;
  std::memcpy(&result, source + offset, sizeof(result));
  return result;
}
const std::byte *Bytes(const RE::NiRefObject *value) {
  return reinterpret_cast<const std::byte *>(value);
}
bool IsData(const RE::NiRefObject *value) {
  const REL::Relocation<std::uintptr_t> table{REL::ID(245920)};
  return value && Read<std::uintptr_t>(Bytes(value), 0) == table.address();
}
std::vector<RE::BSNavmeshInfo *> Infos(const RE::NiRefObject *value) {
  const auto bytes = Bytes(value);
  const auto count = Read<std::uint32_t>(bytes, 0xB0);
  const auto capacity = Read<std::uint32_t>(bytes, 0xA8);
  const auto pointer = Read<RE::BSNavmeshInfo *const *>(bytes, 0xA0);
  if (count > 1024 || count > capacity || (count && !pointer))
    throw std::runtime_error(
        "Native cut-data mesh list exceeds capture limits");
  if (!count)
    return {};
  return {pointer, pointer + count};
}
} // namespace
RegisteredCutCensus CaptureRegisteredCutData() {
  using Coordinator = void *(*)();
  using Ready = bool (*)(void *);
  const REL::Relocation<Coordinator> coordinator{REL::ID(30584)};
  const REL::Relocation<Ready> ready{REL::ID(91403)};
  if (!ready(coordinator()))
    throw std::runtime_error(
        "Registered cut census requires ready navigation pause");
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
  const auto *owner =
      reinterpret_cast<const volatile std::uint32_t *>(publication.get());
  std::optional<RE::BSWriteLockGuard> guard;
  if (owner[0] != GetCurrentThreadId() ||
      !(owner[1] & RE::BSReadWriteLock::kLockWrite))
    guard.emplace(*publication.get());
  const auto before = NativeCutAudit::Snapshot();
  const auto loadBefore = NativeAuthoredBasis::LoadObservation();
  if (!before.available || before.observation.active ||
      !NativeAuthoredBasis::Available() || loadBefore.active)
    throw std::runtime_error(
        "Registered cut census has active/unobserved native workers");
  const REL::Relocation<void **> manager{REL::ID(400442)};
  auto *bytes = static_cast<const std::byte *>(*manager.get());
  if (!bytes)
    throw std::runtime_error("Registered cut census has no native manager");
  RegisteredCutCensus census;
  // Dispatch records queued tasks in +1D8 before starting them (30466/30479).
  // Workers append completions to +1F0 under this lock (30456). Publication
  // ownership alone does not protect those concurrently written headers.
  std::optional<RE::BSSpinLockGuard> queueGuard;
  queueGuard.emplace(*reinterpret_cast<RE::BSSpinLock *>(
      const_cast<std::byte *>(bytes) + 0x208));
  // These collections are distinct from the registry. In particular +0x40
  // contains registration retries and +0x210 retains derived edge effects.
  // Neither must be empty for a read-only census. They DO prevent treating
  // this observation as permission to publish or complete producer coverage.
  for (const auto offset : {0x40, 0x58, 0x88, 0xA0, 0xE8, 0x100, 0x118, 0x1D8,
                            0x1F0, 0x210, 0x228}) {
    const auto count = Read<std::uint32_t>(bytes, offset + 0x10);
    const auto capacity = Read<std::uint32_t>(bytes, offset + 8);
    if (count > capacity || count > 65536 ||
        (count && !Read<const void *>(bytes, offset)))
      throw std::runtime_error(
          "Native producer collection exceeds census budget");
    census.retainedCollections.emplace(offset, count);
  }
  const auto preparedCapacity = Read<std::uint32_t>(bytes, 0x1B4);
  const auto preparedFree = Read<std::uint32_t>(bytes, 0x1B8);
  if (preparedCapacity > 65536 || preparedFree > preparedCapacity)
    throw std::runtime_error("Registered cut prepared-task header invalid");
  census.preparedTasks = preparedCapacity - preparedFree;
  queueGuard.reset();
  using Groups = RE::BSTHashMap<std::uint32_t, RE::NiPointer<RE::NiRefObject>>;
  static_assert(sizeof(Groups) == 0x30);
  const REL::Relocation<std::uintptr_t> groupTable{REL::ID(204241)};
  std::map<RE::NiRefObject *, RE::NiPointer<RE::NiRefObject>> groups;
  std::map<RE::NiRefObject *, RE::NiPointer<RE::NiRefObject>> retained;
  std::set<RE::NiRefObject *> currentInputs;
  std::set<RE::NiRefObject *> geometryInputs;
  const auto append =
      [&](const RE::BSTArray<RE::NiPointer<RE::NiRefObject>> &array,
          bool current, bool geometry) {
        const auto *header = reinterpret_cast<const std::byte *>(&array);
        const auto capacity = Read<std::uint32_t>(header, 8);
        const auto size = Read<std::uint32_t>(header, 16);
        if (size > 4096 || size > capacity ||
            (size && !Read<const void *>(header, 0)))
          throw std::runtime_error(
              "Registered cut input array exceeds census budget");
        for (const auto &data : array) {
          if (!IsData(data.get()) ||
              (retained.size() >= 4096 && !retained.contains(data.get())))
            throw std::runtime_error(
                "Registered cut input type/budget unsupported");
          retained.try_emplace(data.get(), data);
          if (current)
            currentInputs.insert(data.get());
          if (geometry)
            geometryInputs.insert(data.get());
        }
      };
  // Native30471/30473 consume +160 using operation 0x40 (remove door
  // access metadata); 30472 schedules 0x30 (add door access metadata).
  // These are not the 0x10/0x20 geometry cut/undo operations. Keep their
  // memberships for native updates without applying their shapes via 91150.
  // +130 preserves the previous reference state.
  // +10 is the independently registered reference group, not a complete census.
  for (const auto offset : {0x10, 0x130, 0x160}) {
    const auto *header = bytes + offset;
    const auto capacity = Read<std::uint32_t>(header, 0x0C);
    const auto free = Read<std::uint32_t>(header, 0x10);
    if (capacity > 65536 || free > capacity ||
        (capacity && !Read<const void *>(header, 0x28)))
      throw std::runtime_error("Native cut group header exceeds census budget");
    for (const auto &[id, group] : *reinterpret_cast<const Groups *>(header)) {
      if (!group ||
          Read<std::uintptr_t>(Bytes(group.get()), 0) != groupTable.address())
        throw std::runtime_error("Registered cut group type unsupported");
      groups.try_emplace(group.get(), group);
      append(*reinterpret_cast<
                 const RE::BSTArray<RE::NiPointer<RE::NiRefObject>> *>(
                 Bytes(group.get()) + 0x18),
             offset != 0x130, offset == 0x10);
    }
  }
  append(
      *reinterpret_cast<const RE::BSTArray<RE::NiPointer<RE::NiRefObject>> *>(
          bytes + 0x70),
      true, true);
  auto &result = census.inputs;
  for (const auto &[pointer, reference] : retained) {
    NativeCutDataSource input;
    input.original = reference;
    input.retainedOnly = !currentInputs.contains(pointer);
    input.cutsGeometry = geometryInputs.contains(pointer);
    const auto *data = Bytes(pointer);
    input.owner = Read<void *>(data, 0x10);
    if (input.owner) {
      if (const auto group =
              groups.find(static_cast<RE::NiRefObject *>(input.owner));
          group != groups.end())
        input.ownerGroup = group->second;
      if (!input.ownerGroup)
        throw std::runtime_error(
            "Registered cut owner group is outside retained registry");
    }
    input.rigidBody =
        RE::NiPointer<RE::NiRefObject>{Read<RE::NiRefObject *>(data, 0x18)};
    input.auxiliary =
        RE::NiPointer<RE::NiRefObject>{Read<RE::NiRefObject *>(data, 0xB8)};
    std::memcpy(input.values.data(), data + 0x20, input.values.size());
    input.affectedInfos = Infos(pointer); // opaque, not dereferenced
    input.affectedAllocation = Read<const void *>(data, 0xA0);
    result.push_back(std::move(input));
  }
  const auto after = NativeCutAudit::Snapshot();
  const auto loadAfter = NativeAuthoredBasis::LoadObservation();
  if (WriterObserver::Changed(before.observation, after.observation) ||
      WriterObserver::Changed(loadBefore, loadAfter))
    throw std::runtime_error(
        "Native writer changed during registered cut census");
  return census;
}
std::vector<NativeCutDataSource> CaptureNativeCutData(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &loaded,
    const NativeCutSnapshot *captured) {
  const auto owned = captured ? NativeCutSnapshot{} : CaptureNativeCuts(loaded);
  const auto &snapshot = captured ? *captured : owned;
  std::vector<NativeCutDataSource> result;
  if (loaded.size() > 1024)
    throw std::runtime_error("Native cut-data mesh budget exceeded");
  std::map<const void *, std::uint32_t> loadedInfos;
  for (const auto &mesh : loaded)
    if (mesh && mesh->unk110)
      loadedInfos.emplace(mesh->unk110, mesh->QNavmeshID());
  const REL::Relocation<std::uintptr_t> undoTable{REL::ID(246891)};
  for (const auto &mesh : loaded) {
    if (!mesh)
      continue;
    if (mesh->obstacles.size() > 4096)
      throw std::runtime_error("Native cut-data undo budget exceeded");
    const auto found = snapshot.records.find(mesh->QNavmeshID());
    if (found == snapshot.records.end() ||
        found->second.size() != mesh->obstacles.size())
      throw std::runtime_error(
          "Native cut membership changed between captures");
    std::size_t index{};
    for (const auto &undo : mesh->obstacles) {
      if (result.size() >= 4096 || !undo)
        throw std::runtime_error("Native cut-data source budget exceeded");
      if (Read<std::uintptr_t>(reinterpret_cast<const std::byte *>(undo.get()),
                               0) != undoTable.address())
        throw std::runtime_error("Unsupported native cut undo type");
      const auto lifecycle = DecodeCutLifecycle(Read<std::uint32_t>(
          reinterpret_cast<const std::byte *>(undo.get()), 0x10));
      if (!lifecycle)
        throw std::runtime_error("Unsupported native cut-data lifecycle");
      const auto &record = found->second.at(index++);
      if (record.rawStatus !=
          Read<std::uint32_t>(reinterpret_cast<const std::byte *>(undo.get()),
                              0x10))
        throw std::runtime_error(
            "Native shared cut status changed between captures");
      if (!record.Applied())
        continue; // Legacy behavior pending full cut-reference integration.
      const auto pointer = Read<RE::NiRefObject *>(
          reinterpret_cast<const std::byte *>(undo.get()), 0x18);
      if (!IsData(pointer))
        throw std::runtime_error("Unsupported retained native cut-data type");
      NativeCutDataSource source;
      source.meshID = mesh->QNavmeshID();
      source.original = RE::NiPointer<RE::NiRefObject>(pointer);
      const auto bytes = Bytes(pointer);
      source.owner = Read<void *>(bytes, 0x10);
      if (source.owner) {
        auto *group = static_cast<RE::NiRefObject *>(source.owner);
        const REL::Relocation<std::uintptr_t> groupTable{REL::ID(204241)};
        if (Read<std::uintptr_t>(Bytes(group), 0) != groupTable.address())
          throw std::runtime_error(
              "Retained native cut owner group type unsupported");
        source.ownerGroup = RE::NiPointer<RE::NiRefObject>{group};
      }
      source.rigidBody =
          RE::NiPointer<RE::NiRefObject>(Read<RE::NiRefObject *>(bytes, 0x18));
      source.auxiliary =
          RE::NiPointer<RE::NiRefObject>(Read<RE::NiRefObject *>(bytes, 0xB8));
      std::memcpy(source.values.data(), bytes + 0x20, source.values.size());
      // 30476/30481 iterate eight-byte borrowed info pointers. 91061 returns
      // BSNavmesh::unk110 and the native writer appends those same pointers.
      source.affectedInfos = Infos(pointer);
      for (const auto info : source.affectedInfos) {
        const auto found = loadedInfos.find(info);
        if (found == loadedInfos.end())
          throw std::runtime_error(
              "Native cut-data list contains unresolved mesh info");
        source.affectedMeshes.push_back(found->second);
      }
      if (std::find(source.affectedMeshes.begin(), source.affectedMeshes.end(),
                    source.meshID) == source.affectedMeshes.end())
        throw std::runtime_error(
            "Native cut-data list does not include its owning mesh");
      source.affectedAllocation = Read<const void *>(bytes, 0xA0);
      result.push_back(std::move(source));
    }
  }
  return result;
}
std::unique_ptr<NativeCutDataDraft>
NativeCutDataDraft::Create(const NativeCutDataSource &source,
                           std::string &error) {
  if (!IsData(source.original.get()) || source.affectedMeshes.size() > 1024) {
    error = "Invalid native cut-data baseline";
    return nullptr;
  }
  auto draft = std::make_unique<NativeCutDataDraft>();
  draft->expected = source;
  const auto storage = RE::malloc(0xC0);
  if (!storage) {
    error = "Could not allocate detached cut data";
    return nullptr;
  }
  if (reinterpret_cast<std::uintptr_t>(storage) % 16 != 0) {
    RE::free(storage);
    error = "Native cut-data allocation lacks required alignment";
    return nullptr;
  }
  using Construct = RE::NiRefObject *(*)(void *);
  const REL::Relocation<Construct> construct{REL::ID(91378)};
  draft->data = RE::NiPointer<RE::NiRefObject>(construct(storage));
  const auto bytes = reinterpret_cast<std::byte *>(draft->data.get());
  std::memcpy(bytes + 0x10, &source.owner, sizeof(source.owner));
  // Assignment retains each input. Native destructor91381 releases them,
  // frees the owned BSTArray, and balances the constructor's Ni object count.
  *reinterpret_cast<RE::NiPointer<RE::NiRefObject> *>(bytes + 0x18) =
      source.rigidBody;
  *reinterpret_cast<RE::NiPointer<RE::NiRefObject> *>(bytes + 0xB8) =
      source.auxiliary;
  std::memcpy(bytes + 0x20, source.values.data(), source.values.size());
  auto &meshes =
      *reinterpret_cast<RE::BSTArray<RE::BSNavmeshInfo *> *>(bytes + 0xA0);
  for (const auto info : source.affectedInfos)
    meshes.push_back(info);
  if (!draft->Matches()) {
    error = "Detached native cut-data clone mismatch";
    return nullptr;
  }
  return draft;
}
bool NativeCutDataDraft::Matches() const {
  if (!IsData(data.get()) || data.get() == expected.original.get())
    return false;
  const auto bytes = Bytes(data.get());
  if (Read<void *>(bytes, 0x10) != expected.owner ||
      Read<RE::NiRefObject *>(bytes, 0x18) != expected.rigidBody.get() ||
      Read<RE::NiRefObject *>(bytes, 0xB8) != expected.auxiliary.get() ||
      std::memcmp(bytes + 0x20, expected.values.data(), expected.values.size()))
    return false;
  const auto cloneArray = Read<const void *>(bytes, 0xA0);
  return Infos(data.get()) == expected.affectedInfos &&
         (expected.affectedMeshes.empty() ||
          cloneArray != expected.affectedAllocation);
}
void NativeCutDataDraft::ClearAffectedInfos() {
  auto &infos = *reinterpret_cast<RE::BSTArray<RE::BSNavmeshInfo *> *>(
      reinterpret_cast<std::byte *>(data.get()) + 0xA0);
  infos.clear();
  expected.affectedInfos.clear();
  expected.affectedMeshes.clear();
}
void NativeCutDataDraft::RemapAffectedInfos(
    const std::map<std::uint32_t, RE::BSNavmeshInfo *> &mapping) {
  std::vector<RE::BSNavmeshInfo *> remapped;
  std::vector<std::uint32_t> ids;
  for (const auto id : expected.affectedMeshes) {
    const auto found = mapping.find(id);
    if (found == mapping.end() || !found->second ||
        found->second->navMeshID < 0xFF000000)
      throw std::runtime_error("Unresolved detached cut-data membership");
    remapped.push_back(found->second);
    ids.push_back(found->second->navMeshID);
  }
  auto &infos = *reinterpret_cast<RE::BSTArray<RE::BSNavmeshInfo *> *>(
      reinterpret_cast<std::byte *>(data.get()) + 0xA0);
  infos.clear();
  for (const auto info : remapped)
    infos.push_back(info);
  expected.affectedInfos = std::move(remapped);
  expected.affectedMeshes = std::move(ids);
  if (!Matches())
    throw std::runtime_error("Detached cut-data membership validation failed");
}
} // namespace SkyBuild::Navmesh
