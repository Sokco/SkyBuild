#include "SkyBuildPaths.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/cuts/CanonicalCutMembership.h"
#include "navmesh/cuts/CutReferenceConversion.h"
#include "navmesh/cuts/CutTransferIdentity.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/NativeCutOwnerTransfer.h"
#include "navmesh/cuts/NativeCutReferences.h"
#include "navmesh/diagnostics/CutReferenceEvidence.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/diagnostics/FailureArchive.h"
#include "navmesh/diagnostics/OwnedNavmeshEvidence.h"
#include "navmesh/diagnostics/TimingTrace.h"
#include "navmesh/generation/AuthoredTransfer.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeGeometry.h"
#include "navmesh/native/NativeSearchAudit.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include "navmesh/publication/PublicationMembership.h"
#include "navmesh/publication/RetirementEligibility.h"
#include "navmesh/publication/RouteConnectivity.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include <cstring>
namespace SkyBuild::Navmesh {
namespace {
template <class A, class B> bool SameMap(const A &a, const B &b) {
  if (a.size() != b.size())
    return false;
  for (const auto &entry : a) {
    const auto found = b.find(entry.first);
    if (found == b.end() || found->second != entry.second)
      return false;
  }
  return true;
}

} // namespace
bool NativePublication::ReadyForPublication() {
  using Coordinator = void *(*)();
  using Ready = bool (*)(void *);
  const REL::Relocation<Coordinator> coordinator{REL::RelocationID(29765, 30584)};
  const REL::Relocation<Ready> ready{REL::RelocationID(88948, 91403)};
  const auto audit = NativeSearchAudit::Snapshot();
  return audit.available && !audit.active && ready(coordinator());
}
NativePublication::~NativePublication() {
  if (installed) {
    // This is a programming/lifecycle failure, not permission to free live
    // navigation. Keep all native ownership until the process ends.
    spdlog::critical("Published navmesh owner destroyed without restoration; "
                     "quarantining transaction.");
    auto *retained = new NativePublication;
    retained->graph = std::move(graph);
    retained->cells = std::move(cells);
    retained->lists = std::move(lists);
    retained->cutLists = std::move(cutLists);
    retained->originalBindingTransfers = std::move(originalBindingTransfers);
    retained->verifiedDormantOriginals = std::move(verifiedDormantOriginals);
    retained->supersededForms = std::move(supersededForms);
    retained->originalIDs = std::move(originalIDs);
    retained->replacementMap = std::move(replacementMap);
    retained->expectedMap = std::move(expectedMap);
    retained->originalChanges = std::move(originalChanges);
    retained->publishedMap = std::move(publishedMap);
    retained->expectedRoutes = std::move(expectedRoutes);
    retained->publishedRoutes = std::move(publishedRoutes);
    retained->replacementRoutes = std::move(replacementRoutes);
    retained->expectedRouteIndex = std::move(expectedRouteIndex);
    retained->publishedRouteIndex = std::move(publishedRouteIndex);
    retained->replacementRouteIndex = std::move(replacementRouteIndex);
    retained->routeCopies = std::move(routeCopies);
    retained->installed = true;
    retained->exposed = exposed;
    retained->republished = republished;
  }
}
std::unique_ptr<NativePublication>
NativePublication::Prepare(std::unique_ptr<NativeGraphDraft> graph,
                           std::string &error) {
  TimingTrace timing{"NativePublication::Prepare"};
  nlohmann::json referenceEvidence;
  const char *stage = "publication-preparation";
  // Keep private ownership through failure capture. A try-local owner would
  // destroy the failed graph before the catch could record its actual state.
  std::unique_ptr<NativePublication> result;
  try {
    if (!graph)
      throw std::runtime_error("Missing detached graph");
    if (const auto reason = graph->ValidationError(); !reason.empty())
      throw std::runtime_error(reason);
    result = std::make_unique<NativePublication>();
    timing.Mark("staged graph validation");
    result->graph = std::move(graph);
    const REL::Relocation<RE::BSNavmeshInfoMap **> global{REL::RelocationID(515943, 402040)};
    result->registry = static_cast<RE::NavMeshInfoMap *>(*global.get());
    if (!result->registry)
      throw std::runtime_error("Missing native registry");
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
    const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
    const RE::BSWriteLockGuard publicationGuard(*publication.get());
    const RE::BSReadLockGuard listingGuard(*listing.get());
    const RE::BSReadLockGuard registryGuard(result->registry->mapLock);
    timing.Mark("acquire native publication locks");
    // The validation snapshot is owned plain data, not another engine hash
    // table. Only the replacement buffer needs Skyrim's allocator/layout.
    result->replacementMap.reserve(result->registry->infoMap.size());
    for (const auto &entry : result->registry->infoMap) {
      result->expectedMap.emplace(entry.first, entry.second);
      result->replacementMap.emplace(entry.first, entry.second);
    }
    timing.Mark("single-pass registry snapshot and replacement buffer");
    std::map<RE::NavMesh *, RE::NavMesh *> formReplacements;
    std::map<RE::BSNavmeshInfo *, RE::BSNavmeshInfo *> infoReplacements;
    std::map<RE::TESObjectCELL *, std::vector<NativeFormDraft *>> perCell;
    NativeLocationCache::Obsolete oldInfos, newInfos;
    for (const auto &[sourceID, draft] : result->graph->forms) {
      const auto parent = draft->form->GetSaveParentCell();
      if (!parent || !parent->IsAttached() ||
          !parent->GetRuntimeData().navMeshes)
        throw std::runtime_error(
            "Publication parent is not attached or has no native list");
      perCell[parent].push_back(draft.get());
      const auto old = result->expectedMap.find(sourceID);
      if (old == result->expectedMap.end() &&
          result->graph->originalTopology.contains(sourceID))
        throw std::runtime_error(
            "Original registry entry disappeared before publication");
      if (old != result->expectedMap.end()) {
        const auto source = RE::TESForm::LookupByID<RE::NavMesh>(sourceID);
        if (!source || source->unk110 != old->second ||
            old->second->navMesh != static_cast<RE::BSNavmesh *>(source))
          throw std::runtime_error(
              "Original native publication identity mismatch");
        if (source->GetSaveParentCell() != parent)
          throw std::runtime_error(
              "Replacement changed the source parent cell");
        formReplacements.emplace(source, draft->form.get());
        infoReplacements.emplace(old->second, draft->navigationInfo->info);
        oldInfos.insert(old->second);
        // An authored form remains engine-visible through retained cut data,
        // queued requests and TESForm lookups even after its active cell entry
        // is replaced. Preserve its own info identity in the registry for that
        // entire lifetime. Runtime traversal uses the replacement cell indexes
        // below; removing an original ID is not a safe retirement boundary.
      }
      newInfos.insert(draft->navigationInfo->info);
      if (!result->replacementMap
               .emplace(draft->ID(), static_cast<RE::NavMeshInfo *>(
                                         draft->navigationInfo->info))
               .second)
        throw std::runtime_error("Replacement registry ID already exists");
    }
    result->PrepareRoutes(infoReplacements);
    timing.Mark("prepare cached routes");
    result->PrepareCellLists(perCell, formReplacements, infoReplacements);
    std::map<RE::NiRefObject *, RE::NiRefObject *> cutOriginals;
    timing.Mark("cell publication arrays");
    CutMemberships registeredMembers, discoveredMembers, plannedMembers;
    std::set<std::uint64_t> oldCohort, newCohort;
    for (const auto *info : NativeLocationCache::ObsoleteGenerated())
      oldCohort.insert(reinterpret_cast<std::uintptr_t>(info));
    for (const auto &[oldInfo, newInfo] : infoReplacements)
      oldCohort.insert(reinterpret_cast<std::uintptr_t>(oldInfo));
    for (const auto &[id, draft] : result->graph->forms)
      newCohort.insert(
          reinterpret_cast<std::uintptr_t>(draft->navigationInfo->info));
    for (std::size_t index = 0; index < result->graph->sourceCutData.size();
         ++index) {
      const auto &source = result->graph->sourceCutData[index];
      const auto identity =
          reinterpret_cast<std::uintptr_t>(source.original.get());
      auto &registered = registeredMembers[identity];
      for (auto *info : source.affectedInfos)
        registered.insert(reinterpret_cast<std::uintptr_t>(info));
      auto &discovered = discoveredMembers[identity];
      for (auto *info : result->graph->cutInputs[index]->expected.affectedInfos)
        discovered.insert(reinterpret_cast<std::uintptr_t>(info));
    }
    std::string membershipError;
    if (!PrepareCanonicalCutMemberships(registeredMembers, oldCohort, newCohort,
                                        discoveredMembers, plannedMembers,
                                        membershipError))
      throw std::runtime_error("Discovered publication membership: " +
                               membershipError);
    std::set<RE::NiRefObject *> staged;
    for (std::size_t index = 0; index < result->graph->sourceCutData.size();
         ++index) {
      const auto &source = result->graph->sourceCutData[index];
      cutOriginals.emplace(result->graph->cutInputs[index]->data.get(),
                           source.original.get());
      if (!staged.insert(source.original.get()).second)
        continue;
      CutList list;
      list.owner = source.original;
      list.array = reinterpret_cast<InfoList *>(
          reinterpret_cast<std::byte *>(source.original.get()) + 0xA0);
      list.expected = *list.array;
      if (list.expected.size() != source.affectedInfos.size())
        throw std::runtime_error("Original cut membership changed");
      for (std::size_t i = 0; i < source.affectedInfos.size(); ++i) {
        if (list.expected[static_cast<std::uint32_t>(i)] !=
            source.affectedInfos[i])
          throw std::runtime_error("Original cut identity changed");
      }
      for (const auto info : plannedMembers.at(
               reinterpret_cast<std::uintptr_t>(source.original.get())))
        list.replacement.push_back(reinterpret_cast<RE::BSNavmeshInfo *>(info));
      list.published = list.replacement;
      result->cutLists.push_back(std::move(list));
    }
    // Plan conversion of every owning reference on the still-private graph.
    // Canonical registered objects and world membership arrays remain unchanged
    // here; their exact membership exchange is the publication transaction.
    // Explicit builder provenance supplies targets; no shape heuristic chooses
    // canonical inputs here. Saved maps and memberships must be represented.
    RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> cutCohort;
    for (const auto &[id, draft] : result->graph->forms)
      cutCohort.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{
          static_cast<RE::BSNavmesh *>(draft->form.get())});
    std::vector<RE::NiPointer<RE::NiRefObject>> retainedCutInputs;
    for (const auto &input : result->graph->cutInputs)
      retainedCutInputs.push_back(input->data);
    const auto cutReferences =
        CaptureNativeCutReferences(cutCohort, retainedCutInputs);
    std::map<std::uint64_t, std::uint64_t> substitution;
    for (const auto &[clone, source] : cutOriginals)
      substitution.emplace(reinterpret_cast<std::uintptr_t>(clone),
                           reinterpret_cast<std::uintptr_t>(source));
    stage = "cut-reference-validation";
    if (Diagnostics::Enabled()) {
      referenceEvidence =
          EncodeCutReferenceEvidence(cutReferences, substitution);
      referenceEvidence["publicationMemberships"] = nlohmann::json::object();
      referenceEvidence["preservedOutsideMemberships"] =
          nlohmann::json::object();
      for (const auto &list : result->cutLists) {
        auto members = nlohmann::json::array(),
             outside = nlohmann::json::array();
        for (const auto *info : list.published) {
          const auto token = reinterpret_cast<std::uintptr_t>(info);
          (newCohort.contains(token) ? members : outside).push_back(token);
        }
        const auto key = std::format(
            "0x{:x}", reinterpret_cast<std::uintptr_t>(list.owner.get()));
        referenceEvidence["publicationMemberships"][key] = std::move(members);
        referenceEvidence["preservedOutsideMemberships"][key] =
            std::move(outside);
      }
    }
    std::string referenceError;
    const auto referencePlan = CutReferenceConversion::Prepare(
        cutReferences, substitution, referenceError);
    if (!referencePlan)
      throw std::runtime_error("Complete staged cut references: " +
                               referenceError);
    const auto &converted = referencePlan->After();
    for (const auto &list : result->cutLists) {
      std::set<std::uint64_t> expected;
      for (const auto *info : list.published) {
        const auto token = reinterpret_cast<std::uintptr_t>(info);
        if (newCohort.contains(token))
          expected.insert(token);
      }
      const auto membership = converted.affectedInfos.find(
          reinterpret_cast<std::uintptr_t>(list.owner.get()));
      if (membership == converted.affectedInfos.end() ||
          membership->second != expected)
        throw std::runtime_error(
            "Complete staged cut membership differs from publication cohort");
    }
    spdlog::debug(
        "Complete staged cut reference plan: {} slots, {} substitutions, "
        "{} retained inputs. Private canonical adoption follows; world "
        "memberships remain staged.",
        cutReferences.references.size(), substitution.size(),
        converted.data.size());
    stage = "private-canonical-cut-adoption";
    for (const auto &reference : cutReferences.references)
      if (!cutOriginals.contains(
              reinterpret_cast<RE::NiRefObject *>(reference.data)))
        throw std::runtime_error(
            "Private cut owner has no canonical provenance");
    for (const auto &mesh : cutCohort)
      if (result->registry->infoMap.contains(mesh->QNavmeshID()))
        throw std::runtime_error(
            "Canonical adoption requires an unpublished cohort");
    NativeCutOwnerTransfer ownerTransfer(cutCohort, cutOriginals);
    ownerTransfer.Apply();
    const auto adopted = CaptureNativeCutReferences(
        cutCohort, {}, CutMembershipCapture::RetainExternalTokens);
    // A registered canonical input still has its native BEFORE memberships.
    // Compare geometry ownership now; memberships exchange only on Commit.
    std::set<std::uint64_t> referencedData;
    for (const auto &ref : converted.references)
      referencedData.insert(ref.data);
    if (adopted.references != converted.references ||
        adopted.data != referencedData)
      throw std::runtime_error(
          "Private canonical owner conversion differs from plan");
    result->graph->expectedCuts = CaptureNativeCuts(cutCohort);
    ownerTransfer.Commit();
    spdlog::debug(
        "Private canonical cut adoption: {} owning slots; undo inputs, "
        "current owners and saved owners converted together. World memberships "
        "unchanged.",
        ownerTransfer.Size());
    stage = "publication-journal";
    result->obsoleteOriginal = NativeLocationCache::Prepare(oldInfos, newInfos);
    timing.Mark("canonical cut memberships and private owner adoption");
    result->obsoleteReplacement =
        NativeLocationCache::Prepare(newInfos, oldInfos);
    BuildPublicationJournal(result->expectedMap, result->replacementMap,
                            result->originalChanges, result->publishedMap);
    timing.Mark("obsolete location sets and registry delta journal");
    return result;
  } catch (const std::exception &exception) {
    error = exception.what();
    // Read only the still-retained private graph. Do not chase world registry
    // entries or destroyed drafts while reporting a preparation failure.
    try {
      const auto directory = SkyBuild::Paths::DiagnosticsDirectory();
      if (Diagnostics::Enabled() && directory) {
        nlohmann::json failure{
            {"schema", 1},
            {"kind", "publication-preparation-failure"},
            {"stage", stage},
            {"error", error},
            {"referencePlan", referenceEvidence},
            {"nativePublicationAuthorized", false},
            {"nativeWriterExclusion", false},
            {"missingEvidence", {"complete world binding epoch"}},
            {"captureErrors", nlohmann::json::array()}};
        const auto *retained = result ? result->graph.get() : graph.get();
        if (retained) {
          failure["preparedGeometry"] =
              EncodeAuthoredEvidence(retained->expected);
          failure["preparedCuts"] = EncodeCutEvidence(retained->expectedCuts);
          failure["originalTopology"] =
              EncodeAuthoredEvidence(retained->originalTopology);
          failure["originalCuts"] = EncodeCutEvidence(retained->originalCuts);
          try {
            if (!ReadyForPublication())
              throw std::runtime_error(
                  "Failure capture lost the caller's native pause");
            const REL::Relocation<RE::BSReadWriteLock *> publication{
                REL::RelocationID(515948, 402045)};
            RE::BSWriteLockGuard captureGuard{*publication.get()};
            RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> cohort;
            for (const auto &[id, draft] : retained->forms)
              cohort.push_back(
                  RE::BSTSmartPointer<RE::BSNavmesh>{draft->form.get()});
            const auto capture = [&](const char *field, auto &&read) {
              try {
                failure[field] = read();
              } catch (const std::exception &captureError) {
                failure["captureErrors"].push_back(
                    {{"field", field}, {"error", captureError.what()}});
              }
            };
            capture("privateFailureGeometry", [&] {
              std::map<std::uint32_t, AuthoredMesh> geometry;
              for (const auto &mesh : cohort)
                geometry.emplace(
                    mesh->QNavmeshID(),
                    CaptureNativeTopology(*mesh, mesh->QNavmeshID()));
              return EncodeAuthoredEvidence(geometry);
            });
            capture("privateFailureCuts", [&] {
              return EncodeCutEvidence(CaptureNativeCuts(cohort));
            });
            capture("privateFailureReferences", [&] {
              return EncodeCutReferenceEvidence(
                  CaptureNativeCutReferences(
                      cohort, {}, CutMembershipCapture::RetainExternalTokens),
                  {});
            });
          } catch (const std::exception &captureError) {
            failure["captureErrors"].push_back(
                {{"field", "privateFailureEpoch"},
                 {"error", captureError.what()}});
          }
        }
        const auto path = ArchiveFailure(
            *directory / "SkyBuild-navmesh-failures", failure.dump());
        spdlog::warn("Archived publication preparation failure to {}",
                     path.string());
      }
    } catch (const std::exception &archiveError) {
      spdlog::warn("Publication evidence archive failed: {}",
                   archiveError.what());
    }
    return nullptr;
  }
}
void NativePublication::PrepareRoutes(
    const InfoReplacements &infoReplacements) {
  TimingTrace timing{"NativePublication::PrepareRoutes"};
  // Precomputed routes contain borrowed info pointers independently of the
  // registry. Retain their original buffers and stage independent copies.
  // Validate links actually used by each affected route. Changes to unused
  // adjacency do not invalidate cached routes.
  const REL::Relocation<RE::BSPrecomputedNavmeshInfoPathMap **> routeGlobal{
      REL::RelocationID(525737, 412218)};
  if (*routeGlobal.get() !=
      static_cast<RE::BSPrecomputedNavmeshInfoPathMap *>(registry))
    throw std::runtime_error(
        "Precomputed route singleton differs from registry");
  const auto linked = [](const RE::BSNavmeshInfo *from,
                         const RE::BSNavmeshInfo *to) {
    if (!from || !to || (from->adjacentMeshCount && !from->adjacentMeshesArray))
      return false;
    for (unsigned i = 0; i < from->adjacentMeshCount; ++i)
      if (from->adjacentMeshesArray[i] == to->navMeshID)
        return true;
    return false;
  };
  expectedRoutes = registry->allPaths;
  expectedRouteIndex = registry->infoToIndexMap;
  for (const auto &[info, index] : expectedRouteIndex) {
    const auto found =
        infoReplacements.find(const_cast<RE::BSNavmeshInfo *>(info));
    const auto mapped = found == infoReplacements.end() ? info : found->second;
    if (!replacementRouteIndex.emplace(mapped, index).second)
      throw std::runtime_error("Precomputed route index collision");
  }
  std::map<Route *, Route *> copies;
  timing.Mark("form/info mappings and route indexes");
  for (auto *route : expectedRoutes) {
    if (!route) {
      replacementRoutes.push_back(nullptr);
      continue;
    }
    if (const auto prior = copies.find(route); prior != copies.end()) {
      replacementRoutes.push_back(prior->second);
      continue;
    }
    const bool changed =
        std::any_of(route->begin(), route->end(), [&](const auto *info) {
          return infoReplacements.contains(
              const_cast<RE::BSNavmeshInfo *>(info));
        });
    if (!changed) {
      replacementRoutes.push_back(route);
      continue;
    }
    RouteCopy copy;
    copy.source = route;
    copy.expected = *route;
    auto *storage = RE::malloc<Route>();
    if (!storage)
      throw std::bad_alloc();
    copy.replacement.reset(std::construct_at(storage));
    for (const auto *info : *route) {
      if (!info)
        throw std::runtime_error(
            "Affected precomputed route contains null info");
      const auto found =
          infoReplacements.find(const_cast<RE::BSNavmeshInfo *>(info));
      copy.replacement->push_back(
          found == infoReplacements.end() ? info : found->second);
    }
    for (std::uint32_t i = 1; i < route->size(); ++i) {
      const auto *from = (*route)[i - 1], *to = (*route)[i];
      const auto *newFrom = (*copy.replacement)[i - 1];
      const auto *newTo = (*copy.replacement)[i];
      if ((from == newFrom && to == newTo) || from == to)
        continue;
      const bool forward = linked(from, to), backward = linked(to, from);
      const bool deferred =
          graph->HasDeferredBoundary(from->navMeshID, to->navMeshID);
      if (!CanRetainRouteLink(forward, backward, linked(newFrom, newTo),
                              linked(newTo, newFrom), deferred))
        throw std::runtime_error(std::format(
            "Cached route link {:08X} / {:08X} cannot be "
            "retained; native route regeneration required "
            "(original forward/backward {}/{}, replacement {}/{}, deferred {})",
            from->navMeshID, to->navMeshID, forward, backward,
            linked(newFrom, newTo), linked(newTo, newFrom), deferred));
    }
    copies.emplace(route, copy.replacement.get());
    replacementRoutes.push_back(copy.replacement.get());
    routeCopies.push_back(std::move(copy));
  }
  publishedRoutes = replacementRoutes;
  publishedRouteIndex = replacementRouteIndex;
  spdlog::debug("Runtime replacement checkpoint: precomputed routes verified; "
                "{} slots, {} copied routes, {} indexed infos.",
                expectedRoutes.size(), routeCopies.size(),
                expectedRouteIndex.size());
  timing.Mark("route copy and verification");
}
void NativePublication::PrepareCellLists(
    const CellDrafts &perCell, const FormReplacements &formReplacements,
    const InfoReplacements &infoReplacements) {
  TimingTrace timing{"NativePublication::PrepareCellLists"};
  for (const auto &[parent, drafts] : perCell) {
    const auto scope = timing.Measure("cell list and info index construction",
                                      parent->GetFormID());
    Cell cell;
    cell.owner = parent;
    cell.id = parent->GetFormID();
    cell.array = parent->GetRuntimeData().navMeshes;
    cell.expected = cell.array->navMeshes;
    for (const auto &form : cell.expected)
      if (!form || !form->GetFormID() ||
          !originalIDs.emplace(form.get(), form->GetFormID()).second)
        throw std::runtime_error(
            "Invalid original form identity in publication cohort");
    std::set<RE::NavMesh *> added;
    for (const auto &source : cell.expected) {
      const auto found = formReplacements.find(source.get());
      auto *replacement =
          found == formReplacements.end() ? source.get() : found->second;
      cell.replacement.push_back(RE::BSTSmartPointer<RE::NavMesh>{replacement});
      added.insert(replacement);
    }
    for (const auto *draft : drafts)
      if (!added.contains(draft->form.get()))
        cell.replacement.push_back(
            RE::BSTSmartPointer<RE::NavMesh>{draft->form.get()});
    cell.published = cell.replacement;
    cells.push_back(std::move(cell));
    const auto &key = static_cast<const RE::PathingCell *>(
                          drafts.front()->form->parentCell.get())
                          ->pathingCellInfo;
    const std::uint64_t packed =
        (std::uint64_t{key.worldSpaceID} << 32) | key.cellID.formID;
    const auto entry = registry->ckNavMeshInfoMap.find(packed);
    if (entry == registry->ckNavMeshInfoMap.end() || !entry->second)
      throw std::runtime_error("Native cell info index is missing");
    List list;
    list.array = entry->second;
    list.expected = *list.array;
    for (const auto *draft : drafts) {
      for (const auto &[originalForm, replacementForm] : formReplacements) {
        if (replacementForm != draft->form.get())
          continue;
        const auto info = expectedMap.find(originalForm->GetFormID());
        const auto &cellForms = cells.back().expected;
        if (std::count_if(cellForms.begin(), cellForms.end(),
                          [&](const auto &form) {
                            return form.get() == originalForm;
                          }) != 1 ||
            std::count(list.expected.begin(), list.expected.end(),
                       info->second) != 1)
          throw std::runtime_error(
              "Original mesh is not uniquely indexed in its cell");
      }
    }
    if (std::any_of(lists.begin(), lists.end(), [&](const auto &prior) {
          return prior.array == list.array;
        }))
      throw std::runtime_error("Multiple cells share a publication index; "
                               "explicit coalescing required");
    std::set<RE::BSNavmeshInfo *> inserted;
    for (const auto source : list.expected) {
      const auto found = infoReplacements.find(source);
      auto *replacement =
          found == infoReplacements.end() ? source : found->second;
      list.replacement.push_back(replacement);
      inserted.insert(replacement);
    }
    for (const auto *draft : drafts)
      if (!inserted.contains(draft->navigationInfo->info))
        list.replacement.push_back(draft->navigationInfo->info);
    list.published = list.replacement;
    lists.push_back(std::move(list));
  }
}
std::string NativePublication::Preflight(bool restoring) const {
  if (sealed)
    return "Retired publication cannot be republished";
  try {
    // Cut tasks must finish before either direction of a graph exchange.
    // Deferred borrowed records are retired separately after final handoff.
    const NativeDerivedWork idle;
  } catch (const std::exception &e) {
    return e.what();
  }
  if (installed != restoring)
    return "Publication state mismatch";
  if (!restoring) {
    if (!SameMap(registry->infoMap, expectedMap))
      return "Registry changed since transaction preparation";
  } else {
    // Cell streaming may add/remove unrelated infos. Only identities changed
    // by this transaction must still match; never overwrite unrelated changes.
    for (const auto &entry : publishedMap) {
      const auto prior = expectedMap.find(entry.first);
      if (prior != expectedMap.end() && prior->second == entry.second)
        continue;
      const auto live = registry->infoMap.find(entry.first);
      if (live == registry->infoMap.end() || live->second != entry.second)
        return std::format("Published registry entry {:08X} changed",
                           entry.first);
    }
    for (const auto &entry : originalChanges) {
      const auto published = publishedMap.find(entry.first);
      if (published != publishedMap.end() && published->second == entry.second)
        continue;
      const auto live = registry->infoMap.find(entry.first);
      if (live != registry->infoMap.end() &&
          (published == publishedMap.end() ||
           live->second != published->second))
        return std::format("Original registry ID {:08X} reused", entry.first);
    }
  }
  if (!std::ranges::equal(registry->allPaths,
                          restoring ? publishedRoutes : expectedRoutes) ||
      !SameMap(registry->infoToIndexMap,
               restoring ? publishedRouteIndex : expectedRouteIndex))
    return "Precomputed route cache changed";
  for (const auto &copy : routeCopies)
    if (!std::ranges::equal(*copy.source, copy.expected))
      return "Original precomputed route changed";
  for (const auto &cell : cells) {
    const auto id = cell.id;
    if (RE::TESForm::LookupByID<RE::TESObjectCELL>(id) != cell.owner)
      return "Native cell identity changed since preparation";
    if (!cell.owner->IsAttached())
      return std::format("Native cell {:08X} detached since preparation", id);
    if (cell.owner->GetRuntimeData().navMeshes != cell.array)
      return std::format(
          "Native cell {:08X} mesh array replaced since preparation", id);
    const auto &actual = cell.array->navMeshes;
    const auto &expected = restoring ? cell.published : cell.expected;
    if (std::ranges::equal(actual, expected))
      continue;
    // Preparation/commit remain exact. Once installed, native list order has
    // no significance to the pointer-based publication journal. Exchange
    // restores the retained original buffer, including its original order.
    if (restoring) {
      std::vector<RE::NavMesh *> live, planned, dormant;
      for (const auto &form : actual)
        live.push_back(form.get());
      for (const auto &form : expected)
        planned.push_back(form.get());
      for (const auto form : verifiedDormantOriginals)
        if (form->GetSaveParentCell() == cell.owner)
          dormant.push_back(form);
      if (MatchesPublicationWithDormantOriginals(live, planned, dormant))
        continue;
    }
    const auto missing =
        std::find_if(expected.begin(), expected.end(), [&](const auto &mesh) {
          return std::count(actual.begin(), actual.end(), mesh) !=
                 std::count(expected.begin(), expected.end(), mesh);
        });
    const auto added =
        std::find_if(actual.begin(), actual.end(), [&](const auto &mesh) {
          return std::count(actual.begin(), actual.end(), mesh) !=
                 std::count(expected.begin(), expected.end(), mesh);
        });
    return std::format(
        "Native cell {:08X} mesh membership changed: {} actual / {} expected; "
        "missing {:08X}, unexpected {:08X}{}",
        id, actual.size(), expected.size(),
        missing != expected.end() && *missing ? (*missing)->GetFormID() : 0,
        added != actual.end() && *added ? (*added)->GetFormID() : 0,
        actual.size() == expected.size() && missing == expected.end() &&
                added == actual.end()
            ? " (order changed before commit)"
            : "");
  }
  for (const auto &list : lists)
    if (!std::ranges::equal(*list.array,
                            restoring ? list.published : list.expected))
      return "Cell info index changed";
  for (const auto &list : cutLists)
    if (!std::ranges::equal(*list.array,
                            restoring ? list.published : list.expected))
      return "Native cut membership changed";
  for (const auto &source : graph->sourceCutData) {
    const auto *bytes =
        reinterpret_cast<const std::byte *>(source.original.get());
    std::array<std::byte, 0x80> current;
    std::memcpy(current.data(), bytes + 0x20, current.size());
    RE::NiRefObject *body{}, *auxiliary{};
    std::memcpy(&body, bytes + 0x18, sizeof(body));
    std::memcpy(&auxiliary, bytes + 0xB8, sizeof(auxiliary));
    if (body != source.rigidBody.get() || auxiliary != source.auxiliary.get())
      return "Native cut collision ownership changed since preparation";
    if (!SameCutTransferShape(current, source.values))
      return "Native cut shape moved since preparation";
  }
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> originals;
  for (const auto &[id, expected] : graph->originalTopology) {
    const auto found = expectedMap.find(id);
    const auto form = RE::TESForm::LookupByID<RE::NavMesh>(id);
    if (found == expectedMap.end() || !found->second || !form ||
        !OriginalBindingMatches(form, found->second))
      return "Original mesh ownership is missing";
    const auto mesh = found->second->navMesh;
    if (CaptureNativeTopology(*mesh, id) != expected)
      return "Original geometry changed since generation";
    originals.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{mesh});
  }
  if (CaptureNativeCuts(originals) != graph->originalCuts)
    return "Original cut stacks changed since generation";
  return {};
}
void NativePublication::Exchange() noexcept {
  // CommonLib's inherited-member move constructor is invalid under MSVC.
  // Its allocator-transfer move assignment is valid and performs no allocation.
  InfoMap temporary;
  temporary = std::move(registry->infoMap);
  registry->infoMap = std::move(replacementMap);
  replacementMap = std::move(temporary);
  std::swap(registry->allPaths, replacementRoutes);
  RouteIndex routeTemporary;
  routeTemporary = std::move(registry->infoToIndexMap);
  registry->infoToIndexMap = std::move(replacementRouteIndex);
  replacementRouteIndex = std::move(routeTemporary);
  for (auto &cell : cells)
    std::swap(cell.array->navMeshes, cell.replacement);
  for (auto &list : lists)
    std::swap(*list.array, list.replacement);
  for (auto &list : cutLists)
    std::swap(*list.array, list.replacement);
  installed = !installed;
}
bool NativePublication::Commit(std::string &error) {
  TimingTrace timing{"NativePublication::Commit"};
  if (!NativeLocationCache::Available()) {
    error = "Native location guards are unavailable";
    return false;
  }
  if (!ReadyForPublication()) {
    error = "Navigation is not paused and idle";
    return false;
  }
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  // A restored replacement has already been exposed to native cut updates.
  // Its immutable staging geometry is no longer a valid live invariant.
  if (!exposed) {
    error = graph->ValidationError(false);
    if (!error.empty())
      return false;
  }
  const RE::BSWriteLockGuard listingGuard(*listing.get());
  const RE::BSWriteLockGuard registryGuard(registry->mapLock);
  error = Preflight(false);
  timing.Mark("locks, graph validation and commit preflight");
  if (!error.empty())
    return false;
  NativeLocationCache::Obsolete arriving;
  for (const auto &[id, draft] : graph->forms)
    arriving.insert(draft->navigationInfo->info);
  obsoleteOriginal = NativeLocationCache::Prepare(*obsoleteOriginal, arriving);
  NativeLocationCache::RememberGenerated(arriving);
  NativeLocationCache::Publish(obsoleteOriginal);
  republished = exposed;
  Exchange();
  timing.Mark("location guards and native buffer exchange");
  exposed = true;
  return true;
}
bool NativePublication::NeedsMembershipReconciliation() const {
  if (!installed)
    return false;
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
  const RE::BSReadLockGuard publicationGuard(*publication.get());
  const RE::BSReadLockGuard listingGuard(*listing.get());
  for (const auto &cell : cells)
    if (!cell.owner->IsAttached() ||
        cell.owner->GetRuntimeData().navMeshes != cell.array ||
        !SamePublicationMembership(cell.array->navMeshes, cell.published))
      return true;
  return false;
}
bool NativePublication::ReconcileMembership(std::string &error) {
  if (!installed || !ReadyForPublication()) {
    error =
        "Membership reconciliation requires an installed graph and ready pause";
    return false;
  }
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  const RE::BSWriteLockGuard listingGuard(*listing.get());
  const RE::BSWriteLockGuard registryGuard(registry->mapLock);
  if (!ReconcileLiveForms(error) || !ReconcileLiveCuts(error))
    return false;
  error = Preflight(true);
  if (!error.empty()) {
    CaptureCutEvidence(error);
    return false;
  }
  std::vector<Forms> clean;
  clean.reserve(cells.size());
  for (const auto &cell : cells) {
    clean.emplace_back();
    clean.back() = cell.published;
  }
  // Current runtime forms/infos/geometry stay published. Verified dormant
  // originals and retired forms leave the lists; their owners retain storage.
  for (std::size_t i = 0; i < cells.size(); ++i)
    std::swap(cells[i].array->navMeshes, clean[i]);
  verifiedDormantOriginals.clear();
  spdlog::debug("Reconciled live navigation membership; {} authored bindings "
                "retained privately",
                originalBindingTransfers.size());
  return true;
}
bool NativePublication::Restore(std::string &error) {
  TimingTrace timing{"NativePublication::Restore"};
  if (!ReadyForPublication()) {
    error = "Navigation is not paused and idle";
    return false;
  }
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  // The published graph is now engine-owned mutable navigation: normal cut
  // processing can change its undo state or geometry. Restoration needs the
  // original graph unchanged and the publication journal intact, both checked
  // by Preflight below. The private staging validator is not a live invariant.
  const RE::BSWriteLockGuard listingGuard(*listing.get());
  const RE::BSWriteLockGuard registryGuard(registry->mapLock);
  if (!ReconcileLiveForms(error))
    return false;
  timing.Mark("locks and reconcile live forms");
  if (!ReconcileLiveCuts(error))
    return false;
  timing.Mark("reconcile live cuts");
  error = Preflight(true);
  timing.Mark("restoration preflight");
  if (!error.empty()) {
    CaptureCutEvidence(error);
    return false;
  }
  InfoMap restored;
  RestorePublicationEntries(restored, registry->infoMap, originalChanges,
                            publishedMap);
  replacementMap = std::move(restored);
  timing.Mark("prepare restored registry buffer");
  NativeLocationCache::Obsolete arriving;
  for (const auto &[id, draft] : graph->forms) {
    const auto original = expectedMap.find(id);
    if (original != expectedMap.end())
      arriving.insert(original->second);
  }
  obsoleteReplacement =
      NativeLocationCache::Prepare(*obsoleteReplacement, arriving);
  std::vector<Forms> outgoing;
  outgoing.reserve(cells.size());
  for (const auto &cell : cells) {
    outgoing.emplace_back();
    outgoing.back() = cell.published;
  }
  NativeLocationCache::Publish(obsoleteReplacement);
  for (const auto &[form, info] : originalBindingTransfers)
    form->unk110 = info;
  Exchange();
  // The outgoing list can include dormant authored entries reintroduced by
  // native cutting. Remove them from the private retired buffer, so rollback
  // republishes only runtime forms rather than duplicating authored navigation.
  for (std::size_t i = 0; i < cells.size(); ++i)
    cells[i].replacement = std::move(outgoing[i]);
  originalBindingTransfers.clear();
  verifiedDormantOriginals.clear();
  return true;
}
std::set<RE::TESObjectCELL *> NativePublication::AffectedCells() const {
  std::set<RE::TESObjectCELL *> result;
  for (const auto &cell : cells)
    result.insert(cell.owner);
  return result;
}
bool NativePublication::TracksMesh(const RE::NavMesh *mesh) const {
  if (!mesh)
    return false;
  return std::ranges::any_of(graph->forms, [&](const auto &entry) {
    return entry.second->form.get() == mesh ||
           entry.second->navigationInfo->info->navMesh ==
               static_cast<const RE::BSNavmesh *>(mesh);
  });
}
void NativePublication::AppendCutSources(
    std::vector<const RE::BSNavmesh *> &out) const {
  for (const auto &[id, draft] : graph->forms)
    if (draft->form)
      out.push_back(draft->form.get());
  for (const auto &form : supersededForms)
    if (form)
      out.push_back(form.get());
}
bool NativePublication::StillPublished() const {
  if (!installed || !ReadyForPublication())
    return false;
  const REL::Relocation<RE::BSNavmeshInfoMap **> global{REL::RelocationID(515943, 402040)};
  if (static_cast<RE::NavMeshInfoMap *>(*global.get()) != registry)
    return false;
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const REL::Relocation<RE::BSReadWriteLock *> listing{REL::RelocationID(515945, 402042)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  const RE::BSReadLockGuard listingGuard(*listing.get());
  const RE::BSReadLockGuard registryGuard(registry->mapLock);
  std::map<RE::NavMesh *, RE::NavMesh *> transfers;
  for (const auto &[sourceID, draft] : graph->forms) {
    const auto id = draft->ID();
    const auto entry = registry->infoMap.find(id);
    const auto form = RE::TESForm::LookupByID<RE::NavMesh>(id);
    const auto info = draft->navigationInfo->info;
    if (!form || entry == registry->infoMap.end() ||
        !MatchesPublicationIdentity(
            id, form->GetFormID(), info->navMeshID,
            static_cast<const RE::BSNavmesh *>(form),
            static_cast<const RE::BSNavmesh *>(info->navMesh),
            static_cast<const RE::BSNavmeshInfo *>(info),
            static_cast<const RE::BSNavmeshInfo *>(entry->second),
            static_cast<const RE::BSNavmeshInfo *>(form->unk110)))
      return false;
    transfers.emplace(draft->form.get(), form);
  }
  for (const auto &cell : cells) {
    if (RE::TESForm::LookupByID<RE::TESObjectCELL>(cell.id) != cell.owner ||
        !cell.owner->IsAttached() ||
        cell.owner->GetRuntimeData().navMeshes != cell.array)
      return false;
    std::vector<RE::NavMesh *> actual, planned, dormant;
    for (const auto &form : cell.published) {
      const auto transfer = transfers.find(form.get());
      planned.push_back(transfer == transfers.end() ? form.get()
                                                    : transfer->second);
    }
    for (const auto &form : cell.array->navMeshes) {
      actual.push_back(form.get());
      if (std::find(planned.begin(), planned.end(), form.get()) !=
          planned.end())
        continue;
      const auto info =
          form ? expectedMap.find(form->GetFormID()) : expectedMap.end();
      const auto topology =
          form ? graph->originalTopology.find(form->GetFormID())
               : graph->originalTopology.end();
      if (!form ||
          !std::ranges::any_of(cell.expected,
                               [&](const auto &prior) {
                                 const auto identity =
                                     originalIDs.find(prior.get());
                                 return identity != originalIDs.end() &&
                                        identity->second == form->GetFormID();
                               }) ||
          info == expectedMap.end() || !info->second ||
          !MatchesRetainedOriginalBinding(
              static_cast<RE::BSNavmesh *>(form.get()),
              static_cast<const RE::BSNavmeshInfo *>(info->second),
              info->second->navMesh,
              static_cast<const RE::BSNavmeshInfo *>(form->unk110)) ||
          topology == graph->originalTopology.end() ||
          RE::TESForm::LookupByID<RE::NavMesh>(form->GetFormID()) !=
              form.get() ||
          !graph->originalTopology.contains(form->GetFormID()))
        return false;
      dormant.push_back(form.get());
    }
    if (!MatchesPublicationWithDormantOriginals(actual, planned, dormant))
      return false;
  }
  return true;
}
std::string NativePublication::ValidateInstalled() const {
  if (!installed || !ReadyForPublication())
    return "Installed verification requires ready pause";
  for (const auto &[sourceID, draft] : graph->forms) {
    if (!graph->originalTopology.contains(sourceID))
      continue;
    const auto original = expectedMap.find(sourceID);
    const auto live = registry->infoMap.find(sourceID);
    const auto form = RE::TESForm::LookupByID<RE::NavMesh>(sourceID);
    if (original == expectedMap.end() || live == registry->infoMap.end() ||
        live->second != original->second || !form ||
        form->unk110 != original->second ||
        original->second->navMesh != static_cast<RE::BSNavmesh *>(form))
      return std::format(
          "Retained authored mesh/info identity {:08X} is not discoverable",
          sourceID);
  }
  if (!republished) {
    if (const auto reason = graph->ValidationError(true); !reason.empty())
      return reason;
  } else {
    for (const auto &[id, draft] : graph->forms)
      if (draft->form->unk110 != draft->navigationInfo->info ||
          draft->navigationInfo->info->navMesh !=
              static_cast<RE::BSNavmesh *>(draft->form.get()))
        return "Republished mesh/info ownership changed";
  }
  for (const auto &[id, info] : expectedMap) {
    if (!obsoleteOriginal->contains(info))
      continue;
    RE::BSPathingLocation probe(info->approxLocation, info->pathingCell);
    probe.navMeshInfo = info;
    probe.triangle = 0;
    RE::BSTSmartPointer<RE::BSNavmesh> mesh;
    std::uint16_t triangle = 0xFFFF;
    if (probe.GetNavMeshAndTriangle(mesh, triangle) || probe.navMeshInfo ||
        probe.triangle != 0xFFFF)
      return "Native cached-location guard did not invalidate retired info";
    break;
  }
  const auto pathing = RE::Pathing::GetSingleton();
  if (!pathing)
    return "Missing pathing manager";
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
  const RE::BSWriteLockGuard publicationGuard(*publication.get());
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> loaded;
  if (!pathing->GetAllLoadedNavmeshes5(loaded))
    return "Native loaded mesh enumeration failed";
  std::set<const RE::BSNavmesh *> actual;
  for (const auto &mesh : loaded)
    if (mesh)
      actual.insert(mesh.get());
  for (const auto &[sourceID, draft] : graph->forms)
    if (!actual.contains(static_cast<RE::BSNavmesh *>(draft->form.get())))
      return "Published mesh is absent from native loaded enumeration";
  const RE::BSWriteLockGuard listingGuard(
      *REL::Relocation<RE::BSReadWriteLock *>{REL::RelocationID(515945, 402042)}.get());
  const RE::BSReadLockGuard registryGuard(registry->mapLock);
  return Preflight(true);
}
bool NativePublication::CopyOriginalView(NativeOriginalView &out,
                                         std::string &error) const {
  if (!installed || sealed || !ReadyForPublication()) {
    error = "Original view requires an active publication and ready navigation "
            "pause";
    return false;
  }
  NativeOriginalView snapshot;
  snapshot.topology = graph->originalTopology;
  snapshot.cuts = graph->originalCuts;
  snapshot.cutData = graph->sourceCutData;
  for (const auto &cell : cells) {
    if (!cell.owner || !cell.owner->IsAttached() ||
        RE::TESForm::LookupByID<RE::TESObjectCELL>(cell.id) != cell.owner ||
        cell.owner->GetRuntimeData().navMeshes != cell.array) {
      error = "Retained original cell identity changed";
      return false;
    }
    auto &ids = snapshot.cellMeshes[cell.id];
    for (const auto &form : cell.expected) {
      const auto identity = originalIDs.find(form.get());
      if (!form || identity == originalIDs.end() ||
          !snapshot.topology.contains(identity->second)) {
        error = "Retained original mesh identity is missing";
        return false;
      }
      ids.push_back(identity->second);
    }
  }
  out = std::move(snapshot);
  return true;
}
} // namespace SkyBuild::Navmesh
