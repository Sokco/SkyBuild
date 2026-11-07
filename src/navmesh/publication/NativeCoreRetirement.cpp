#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeGeometry.h"
#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/cuts/RetiredCutWork.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/publication/RetirementEligibility.h"
#include <nlohmann/json.hpp>
namespace SkyBuild::Navmesh {
NativeLocationCache::Obsolete NativePublication::EmptyRetiredInfos() const {
  NativeLocationCache::Obsolete result;
  for (const auto key : releasedCoreKeys)
    result.insert(graph->forms.at(key)->navigationInfo->info);
  return result;
}
std::size_t
NativePublication::ReclaimRetiredCore(const RetirementAuditContext &context) {
  const auto candidates = RetirementCandidates(context);
  struct Entry {
    std::uint32_t key, id;
    RE::NavMesh *form;
    RE::BSNavmeshInfo *info;
    std::unique_ptr<NativeGeometry> storage;
    std::size_t bytes;
  };
  std::vector<Entry> entries;
  entries.reserve(candidates.size());
  std::map<std::uintptr_t, std::uint32_t> selected;
  const auto validArray = [](const auto &array) {
    return array.size() <= array.capacity() && array.capacity() <= 65536 &&
           (!array.capacity() || array.data());
  };
  for (const auto key : candidates) {
    const auto &draft = graph->forms.at(key);
    auto *form = draft->form.get();
    const auto &grid = form->meshGrid;
    if (!validArray(form->vertices) || !validArray(form->triangles) ||
        grid.gridSize > 64 ||
        grid.gridData.size() != std::size_t(grid.gridSize) * grid.gridSize)
      continue;
    bool valid = true;
    std::size_t bytes =
        form->vertices.capacity() * sizeof(RE::BSNavmeshVertex) +
        form->triangles.capacity() * sizeof(RE::BSNavmeshTriangle) +
        grid.gridData.size() * sizeof(RE::BSTArray<std::uint16_t>);
    for (const auto &bucket : grid.gridData) {
      if (!validArray(bucket)) {
        valid = false;
        break;
      }
      bytes += bucket.capacity() * sizeof(std::uint16_t);
    }
    if (!valid)
      continue;
    entries.push_back({key, draft->ID(), form, draft->navigationInfo->info,
                       std::unique_ptr<NativeGeometry>(new NativeGeometry),
                       bytes});
    selected.emplace(
        reinterpret_cast<std::uintptr_t>(static_cast<RE::BSNavmesh *>(form)),
        draft->ID());
  }
  // Header validation can remove a candidate. Reclose the actual release set
  // so no retained mesh has a portal into a newly empty shell.
  const auto closed =
      CloseRetirementCandidates(selected, context.portalTargets);
  std::erase_if(entries,
                [&](const auto &entry) { return !closed.contains(entry.id); });
  if (entries.empty())
    return 0;
  auto updated = releasedCoreKeys;
  NativeLocationCache::Obsolete obsolete;
  std::size_t bytes{};
  for (const auto &entry : entries) {
    updated.insert(entry.key);
    obsolete.insert(entry.info);
    bytes += entry.bytes;
  }
  const auto guarded = NativeLocationCache::Prepare(obsolete);
  // All allocation and admission work precedes the irreversible exchange.
  // Forms, IDs, infos, cuts and auxiliary ownership stay pinned for stale
  // users.
  NativeLocationCache::Publish(guarded);
  releasedCoreKeys.swap(updated);
  for (auto &entry : entries) {
    entry.storage->Exchange(*entry.form);
    entry.storage.reset();
  }
  releasedCoreBytes += bytes;
  spdlog::info("Retired core buffers released: {} forms, {} known bytes; "
               "native identities and auxiliary ownership retained",
               entries.size(), bytes);
  return entries.size();
}
bool NativePublication::SealRetired(std::string &error) {
  if (sealed)
    return true;
  if (installed || !exposed || !ReadyForPublication()) {
    error = "Retirement requires a restored exposed graph and a ready pause";
    return false;
  }
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  const RE::BSReadLockGuard listingGuard(*listing.get());
  const RE::BSReadLockGuard registryGuard(registry->mapLock);
  // Partial registration is not enough to recover a complete graph, but it
  // still prevents discarding its recovery journal.
  std::set<const RE::BSNavmeshInfo *> ownedInfos;
  for (const auto &[id, draft] : graph->forms)
    ownedInfos.insert(draft->navigationInfo->info);
  for (const auto &entry : registry->infoMap)
    if (ownedInfos.contains(entry.second)) {
      error = "A retired graph info is still registered";
      return false;
    }
  // Once sealed this graph can never be republished. Its borrowed candidate
  // memberships and geometry-indexed requests must not create new cut tasks.
  // Keep every owning reference and native allocation; only remove dead tokens.
  try {
    NativeDerivedWork work; // Also rejects prepared/dispatched/completed tasks.
    const auto census = CaptureRegisteredCutData();
    std::set<InfoList *> memberships;
    const auto collect = [&](const auto &source) {
      if (source.original)
        memberships.insert(reinterpret_cast<InfoList *>(
            reinterpret_cast<std::byte *>(source.original.get()) + 0xA0));
    };
    for (const auto &source : census.inputs)
      collect(source);
    for (const auto &source : graph->sourceCutData)
      collect(source);
    for (const auto &source : graph->retiredCutData)
      collect(source);
    for (const auto &list : cutLists)
      memberships.insert(list.array);
    // Validate the entire write set before modifying any native storage.
    for (const auto *array : memberships)
      if (!array || array->size() > array->capacity() ||
          array->size() > 65536 || (array->size() && !array->data()))
        throw std::runtime_error("Invalid retired cut membership array");
    const auto requests = work.Retire(ownedInfos);
    std::size_t removed{};
    for (auto *array : memberships)
      removed += RemoveRetiredCutWork(
          *array, [&](auto *info) { return ownedInfos.contains(info); });
    spdlog::info(
        "Retired cut producers: {} memberships, {} deferred records removed; "
        "{} generated infos sealed",
        removed, requests, ownedInfos.size());
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  const auto journalEntries = expectedMap.size() + replacementMap.size();
  // Account only allocations whose dimensions we know. Undo bodies, shared
  // pathing cells, compressed bounds and allocator overhead are intentionally
  // excluded: this is an observed lower bound, not total process memory.
  RetainedMemory memory;
  try {
    std::set<const void *> counted;
    const auto account = [&](const auto &array, std::size_t elementSize) {
      if (array.data() && counted.insert(array.data()).second)
        memory.knownBytes += array.capacity() * elementSize;
    };
    const auto formMemory = [&](const RE::NavMesh *form) {
      if (!form || !counted.insert(form).second)
        return;
      ++memory.forms;
      memory.knownBytes += sizeof(RE::NavMesh);
      memory.vertices += form->vertices.size();
      memory.triangles += form->triangles.size();
      account(form->vertices, sizeof(RE::BSNavmeshVertex));
      account(form->triangles, sizeof(RE::BSNavmeshTriangle));
      account(form->extraEdgeInfo, sizeof(RE::BSNavmeshEdgeExtraInfo));
      account(form->doorPortals, sizeof(RE::BSNavmeshTriangleDoorPortal));
      account(form->closedDoors, sizeof(RE::BSNavmeshClosedDoorInfo));
      // Native cover storage contains uint16 indices, despite CommonLib's
      // BSNavmeshCoverEdge declaration. Match the native serializer.
      account(form->coverArray, sizeof(std::uint16_t));
      account(form->obstacles,
              sizeof(RE::NiPointer<RE::BSNavmeshObstacleUndoData>));
      account(form->unk0F0, sizeof(void *));
      const auto &grid = form->meshGrid.gridData;
      if (grid.data() && counted.insert(grid.data()).second)
        memory.knownBytes += grid.size() * sizeof(RE::BSTArray<std::uint16_t>);
      for (const auto &bucket : grid)
        account(bucket, sizeof(std::uint16_t));
    };
    for (const auto &[id, draft] : graph->forms) {
      formMemory(draft->form.get());
      const auto *info = draft->navigationInfo->info;
      if (info && counted.insert(info).second) {
        ++memory.infos;
        memory.knownBytes += sizeof(RE::BSNavmeshInfo);
        // Native variable arrays have additional headers; count payload only.
        if (info->connectedDoorsArray &&
            counted.insert(info->connectedDoorsArray).second)
          memory.knownBytes += info->connectedDoorCount *
                               sizeof(RE::BSTSmartPointer<RE::BSPathingDoor>);
        if (info->adjacentMeshesArray &&
            counted.insert(info->adjacentMeshesArray).second)
          memory.knownBytes += info->adjacentMeshCount * sizeof(std::uint32_t);
      }
    }
    for (const auto &form : supersededForms)
      formMemory(form.get());
    for (const auto &route : routeCopies)
      if (route.replacement)
        account(*route.replacement, sizeof(const RE::BSNavmeshInfo *));
    for (const auto &list : cutLists)
      account(list.replacement, sizeof(RE::BSNavmeshInfo *));
    memory.sampled = true;
  } catch (...) {
    memory = {};
  }
  retainedMemory = memory;
  sealed = true;
  const auto release = []<class T>(T &value) { value = T{}; };
  release(expectedMap);
  release(replacementMap);
  release(originalChanges);
  release(publishedMap);
  release(expectedRoutes);
  release(publishedRoutes);
  release(replacementRoutes);
  release(expectedRouteIndex);
  release(publishedRouteIndex);
  release(replacementRouteIndex);
  // A copied route was engine-visible too. Keep its allocation and only
  // discard the private comparison array; cached native consumers may hold
  // the route pointer beyond this pause.
  for (auto &route : routeCopies)
    release(route.expected);
  // These are comparison copies, never the native list allocation exchanged
  // with the obstacle owner. Preserve replacement backing and owner lifetime.
  for (auto &list : cutLists) {
    retainedMemory.comparisonBytesReleased +=
        (list.expected.capacity() + list.published.capacity()) *
        sizeof(RE::BSNavmeshInfo *);
    release(list.expected);
    release(list.published);
  }
  release(cells);
  release(lists);
  // These journals retain native obstacle owners. Keep those references
  // alongside the graph until native retirement can be proven.
  release(originalBindingTransfers);
  release(verifiedDormantOriginals);
  release(originalIDs);
  obsoleteOriginal.reset();
  obsoleteReplacement.reset();
  for (const auto &[id, array] : graph->expectedDoors)
    retainedMemory.comparisonBytesReleased +=
        array.capacity() * sizeof(RE::BSNavmeshTriangleDoorPortal);
  for (const auto &[id, array] : graph->expectedClosedDoors)
    retainedMemory.comparisonBytesReleased +=
        array.capacity() * sizeof(RE::BSNavmeshClosedDoorInfo);
  graph->ReleaseRetiredValidation();
  spdlog::debug(
      "Sealed retired navigation: released {} registry journal entries and "
      "validation buffers; {} native meshes/infos remain retained",
      journalEntries, graph->MeshCount());
  spdlog::debug(
      "Retired navigation footprint: {} forms, {} infos, {} vertices, {} "
      "triangles; at least {} known bytes retained; {} cut comparison bytes "
      "released. Shared/opaque native allocations excluded.",
      retainedMemory.forms, retainedMemory.infos, retainedMemory.vertices,
      retainedMemory.triangles, retainedMemory.knownBytes,
      retainedMemory.comparisonBytesReleased);
  return true;
}
std::vector<std::uint32_t> NativePublication::RetirementCandidates(
    const RetirementAuditContext &context) const {
  if (!sealed || installed || !ReadyForPublication())
    return {};
  std::vector<std::uint32_t> candidates;
  for (const auto &[key, draft] : graph->forms) {
    if (releasedCoreKeys.contains(key))
      continue;
    auto *form = draft->form.get();
    auto *info = draft->navigationInfo->info;
    form->IncRef();
    const auto refs = form->DecRef();
    const RetirementEligibility state{
        sealed,
        form->GetFormID() == draft->ID() && info->navMeshID == draft->ID() &&
            info->navMesh == form && form->unk110 == info &&
            RE::TESForm::LookupByID<RE::NavMesh>(draft->ID()) == form,
        !context.registeredInfos.contains(info),
        !context.loadedForms.contains(form),
        form->obstacles.empty() &&
            (!form->triangleToObstacleMap ||
             form->triangleToObstacleMap->empty()) &&
            form->unk0F0.empty() &&
            std::none_of(form->triangles.begin(), form->triangles.end(),
                         [](const auto &face) {
                           return (face.triangleFlags.underlying() & 32U) != 0;
                         }),
        form->doorPortals.empty() && form->closedDoors.empty() &&
            !info->connectedDoorCount,
        !context.undoTargets.contains(form),
        !context.cutMemberships.contains(info),
        !context.derivedWorkInfos.contains(info),
        refs};
    if (IsRetirementCandidate(state))
      candidates.push_back(key);
  }
  std::map<std::uintptr_t, std::uint32_t> eligible;
  for (auto key : candidates) {
    const auto &draft = graph->forms.at(key);
    eligible.emplace(reinterpret_cast<std::uintptr_t>(
                         static_cast<RE::BSNavmesh *>(draft->form.get())),
                     draft->ID());
  }
  const auto closed =
      CloseRetirementCandidates(eligible, context.portalTargets);
  std::erase_if(candidates, [&](auto key) {
    return !closed.contains(graph->forms.at(key)->ID());
  });
  spdlog::debug("Retired geometry eligibility after retained-portal closure: "
                "{} candidates",
                candidates.size());
  // Necessary ownership conditions do not establish FormID-consumer retirement.
  // Native TESForm destruction recycles FF IDs through 13739. Keep all owners.
  return candidates;
}
} // namespace SkyBuild::Navmesh
