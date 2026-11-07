#include "SkyBuildPaths.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/generation/AuthoredTransfer.h"
#include "navmesh/cuts/CanonicalCutMembership.h"
#include "navmesh/cuts/CutReferenceConversion.h"
#include "navmesh/diagnostics/CutReferenceEvidence.h"
#include "navmesh/cuts/CutTransferIdentity.h"
#include "navmesh/diagnostics/FailureArchive.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/NativeCutOwnerTransfer.h"
#include "navmesh/cuts/NativeCutReferences.h"
#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeGeometry.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include "navmesh/diagnostics/OwnedNavmeshEvidence.h"
#include "navmesh/publication/PublicationMembership.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include <algorithm>
namespace SkyBuild::Navmesh {
bool NativePublication::ReconcileLiveForms(std::string &error) {
  const auto reject = [&]() {
    if (!Diagnostics::Enabled())
      return false;
    // The callers hold the native pause and all three publication locks. Keep
    // an owned before-image even when validation exits before cut capture.
    try {
      if (const auto directory = SkyBuild::Paths::DiagnosticsDirectory()) {
        const auto pointer = [](const auto *value) {
          return reinterpret_cast<std::uintptr_t>(value);
        };
        const auto describe = [&](RE::NavMesh *form) {
          nlohmann::json value{{"pointer", pointer(form)}};
          if (!form)
            return value;
          const auto id = form->GetFormID();
          const auto registered = registry->infoMap.find(id);
          const auto *info = registered == registry->infoMap.end()
                                 ? nullptr
                                 : registered->second;
          const auto parent = form->GetSaveParentCell();
          value.update(
              {{"id", id},
               {"cell", parent ? parent->GetFormID() : 0},
               {"binding", pointer(form->unk110)},
               {"registryInfo", pointer(info)},
               {"registryMesh", info ? pointer(info->navMesh) : 0},
               {"lookup", pointer(RE::TESForm::LookupByID<RE::NavMesh>(id))},
               {"vertices", form->vertices.size()},
               {"triangles", form->triangles.size()},
               {"ownedByThisGraph", TracksMesh(form)},
               {"ownedByRetiredGraph",
                NativeReplacement::TracksRetiredMesh(form)},
               {"ownedByAnyGraph", NativeReplacement::TracksMesh(form)}});
          return value;
        };
        const auto describeList = [&](const auto &forms) {
          auto result = nlohmann::json::array();
          for (const auto &form : forms)
            result.push_back(describe(form.get()));
          return result;
        };
        auto membership = nlohmann::json::array();
        for (const auto &cell : cells) {
          const auto current = cell.owner->GetRuntimeData().navMeshes;
          membership.push_back(
              {{"cell", cell.id},
               {"attached", cell.owner->IsAttached()},
               {"sameArray", current == cell.array},
               {"live", current ? describeList(current->navMeshes)
                                : nlohmann::json::array()},
               {"published", describeList(cell.published)},
               {"originals", describeList(cell.expected)}});
        }
        auto forms = nlohmann::json::array();
        for (const auto &[source, draft] : graph->forms)
          forms.push_back(
              {{"source", source},
               {"runtimeID", draft->ID()},
               {"ownedInfo", pointer(draft->navigationInfo->info)},
               {"retained", describe(draft->form.get())},
               {"current",
                describe(RE::TESForm::LookupByID<RE::NavMesh>(draft->ID()))}});
        const nlohmann::json evidence{
            {"schema", 1},
            {"kind", "live-form-reconciliation-failure"},
            {"error", error},
            {"nativeWriterExclusion", true},
            {"cells", membership},
            {"forms", forms},
            {"superseded", describeList(supersededForms)}};
        const auto path = ArchiveFailure(
            *directory / "SkyBuild-navmesh-failures", evidence.dump());
        spdlog::warn("Archived live form reconciliation failure: {}",
                     path.string());
      }
    } catch (const std::exception &failure) {
      spdlog::warn("Live form failure capture failed: {}", failure.what());
    }
    return false;
  };
  // Native obstacle updates can transfer a runtime ID/info to a new full form
  // and clear the old form. Reconcile private ownership, never native buffers.
  std::map<RE::NavMesh *, RE::NavMesh *> replacements;
  for (const auto &[sourceID, draft] : graph->forms) {
    auto *info = draft->navigationInfo->info;
    const auto id = draft->ID();
    auto *current = RE::TESForm::LookupByID<RE::NavMesh>(id);
    const auto registered = registry->infoMap.find(id);
    if (!current || current->GetFormID() != id || info->navMeshID != id ||
        current->unk110 != info || registered == registry->infoMap.end() ||
        registered->second != info ||
        info->navMesh != static_cast<RE::BSNavmesh *>(current)) {
      error = "Native replacement form/info transfer is inconsistent";
      return reject();
    }
    if (current == draft->form.get())
      continue;
    replacements.emplace(draft->form.get(), current);
  }
  std::map<RE::NavMesh *, RE::NavMeshInfo *> bindings;
  std::set<RE::NavMesh *> verifiedDormant;
  std::vector<Forms> updated;
  std::vector<Forms> updatedOriginals, originalBuffers;
  Forms supersededOriginals;
  Forms departingForms;
  std::map<RE::NavMesh *, RE::FormID> authoredIDs;
  std::map<std::uint32_t, AuthoredMesh> freshTopology;
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> freshOriginalMeshes;
  updated.reserve(cells.size());
  updatedOriginals.reserve(cells.size());
  originalBuffers.reserve(cells.size());
  std::set<RE::NavMesh *> found;
  for (const auto &cell : cells) {
    if (RE::TESForm::LookupByID<RE::TESObjectCELL>(cell.id) != cell.owner ||
        !cell.owner->IsAttached() ||
        cell.owner->GetRuntimeData().navMeshes != cell.array) {
      error = "Native form transfer requires the original attached cell array";
      return reject();
    }
    Forms originalList;
    for (const auto &prior : cell.expected) {
      const auto identity = originalIDs.find(prior.get());
      if (identity == originalIDs.end()) {
        error = "Retained authored form has no stable publication ID";
        return reject();
      }
      const auto id = identity->second;
      auto *current = RE::TESForm::LookupByID<RE::NavMesh>(id);
      const auto info = expectedMap.find(id);
      const auto topology = graph->originalTopology.find(id);
      if (!current || current->GetFormID() != id ||
          current->GetSaveParentCell() != cell.owner ||
          info == expectedMap.end() || !info->second ||
          info->second->navMesh != static_cast<RE::BSNavmesh *>(current) ||
          (current->unk110 && current->unk110 != info->second) ||
          topology == graph->originalTopology.end() ||
          !authoredIDs.emplace(current, id).second) {
        error = std::format("Authored mesh {:08X} transfer failed "
                            "ownership/topology validation",
                            id);
        return reject();
      }
      if (!current->unk110)
        bindings.emplace(current, info->second);
      freshTopology.emplace(id, CaptureNativeTopology(*current, id));
      freshOriginalMeshes.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{
          static_cast<RE::BSNavmesh *>(current)});
      if (current != prior.get())
        supersededOriginals.push_back(prior);
      originalList.push_back(RE::BSTSmartPointer<RE::NavMesh>{current});
    }
    Forms list;
    list = cell.published;
    for (auto &form : list) {
      const auto replacement = replacements.find(form.get());
      if (replacement == replacements.end())
        continue;
      if (replacement->second->GetSaveParentCell() != cell.owner ||
          !found.insert(replacement->first).second) {
        error = "Native form transfer changed parent or duplicated ownership";
        return reject();
      }
      form = RE::BSTSmartPointer<RE::NavMesh>{replacement->second};
    }
    std::vector<RE::NavMesh *> live, planned, dormant;
    for (const auto &form : list)
      planned.push_back(form.get());
    for (const auto &form : cell.array->navMeshes) {
      live.push_back(form.get());
      if (std::find(planned.begin(), planned.end(), form.get()) !=
          planned.end())
        continue;
      // A delayed native cut update can append a previous generated form.
      // Retention proves lifetime, not current publication. Only an exact
      // retired-owned, unbound and unregistered identity may leave this list;
      // acquire explicit retention below before removing the cell-list owner.
      const auto registration = form ? registry->infoMap.find(form->GetFormID())
                                     : registry->infoMap.end();
      if (form &&
          MatchesDormantRetiredForm(
              form.get(),
              RE::TESForm::LookupByID<RE::NavMesh>(form->GetFormID()),
              form->unk110,
              registration == registry->infoMap.end() ? nullptr
                                                      : registration->second,
              NativeReplacement::TracksRetiredMesh(form.get()),
              TracksMesh(form.get()),
              form->GetSaveParentCell() == cell.owner)) {
        dormant.push_back(form.get());
        verifiedDormant.insert(form.get());
        continue;
      }
      // Native cut processing can append the retained authored source as a
      // dormant entry alongside its transferred runtime form. Only our exact
      // retained source, with unchanged topology and reciprocal info ownership,
      // may be removed by the restoration exchange.
      const auto info =
          form ? expectedMap.find(form->GetFormID()) : expectedMap.end();
      const auto topology =
          form ? graph->originalTopology.find(form->GetFormID())
               : graph->originalTopology.end();
      if (!form ||
          std::count(originalList.begin(), originalList.end(), form) != 1 ||
          info == expectedMap.end() || !info->second ||
          !MatchesRetainedOriginalBinding(
              static_cast<RE::BSNavmesh *>(form.get()),
              static_cast<const RE::BSNavmeshInfo *>(info->second),
              info->second->navMesh,
              static_cast<const RE::BSNavmeshInfo *>(form->unk110)) ||
          topology == graph->originalTopology.end() ||
          RE::TESForm::LookupByID<RE::NavMesh>(form->GetFormID()) !=
              form.get() ||
          !freshTopology.contains(form->GetFormID())) {
        error = "Cell contains an unverified mesh alongside runtime "
                "navigation";
        return reject();
      }
      dormant.push_back(form.get());
      verifiedDormant.insert(form.get());
      if (!form->unk110)
        bindings.emplace(form.get(), info->second);
    }
    if (!MatchesPublicationWithDormantOriginals(live, planned, dormant)) {
      error = "Native form transfer changed cell mesh membership";
      return reject();
    }
    // Every extra has now passed the dormant identity/cell checks above.
    // Pin actual allocations before any cell-list or journal owner can drop
    // them. A retired info may name a native successor never owned by its
    // original draft, so that borrowed association is not a retention proof.
    RetainPublicationDepartures(departingForms, cell.array->navMeshes, list);
    updated.emplace_back();
    updated.back() = std::move(list);
    originalBuffers.emplace_back();
    originalBuffers.back() = originalList;
    updatedOriginals.emplace_back();
    updatedOriginals.back() = std::move(originalList);
  }
  if (found.size() != replacements.size()) {
    error = "Native transferred form is absent from its published cell";
    return reject();
  }
  auto freshCuts = CaptureNativeCuts(freshOriginalMeshes);
  const auto rejectTransfer = [&](std::string reason) {
    error = std::move(reason);
    if (!Diagnostics::Enabled())
      return reject();
    try {
      if (const auto directory = SkyBuild::Paths::DiagnosticsDirectory()) {
        std::map<std::uint32_t, AuthoredMesh> records;
        for (const auto &[id, record] : graph->recordLease.records)
          records.emplace(id, record->mesh);
        auto provenance = nlohmann::json::array();
        for (const auto &[id, record] : graph->recordLease.records)
          provenance.push_back({{"id", id},
                                {"plugin", record->plugin},
                                {"loadSequence", record->loadSequence}});
        const nlohmann::json failure{
            {"schema", 1},
            {"kind", "authored-transfer-failure"},
            {"error", error},
            {"before", EncodeAuthoredEvidence(graph->originalTopology)},
            {"after", EncodeAuthoredEvidence(freshTopology)},
            {"recordLoadBasis", EncodeAuthoredEvidence(records)},
            {"recordLoadProvenance", provenance},
            {"beforeCuts", EncodeCutEvidence(graph->originalCuts)},
            {"afterCuts", EncodeCutEvidence(freshCuts)},
            {"recordVersionsCurrent",
             NativeAuthoredBasis::Matches(graph->recordLease)},
            {"nativeWriterExclusion", false}};
        const auto path = ArchiveFailure(
            *directory / "SkyBuild-navmesh-failures", failure.dump());
        spdlog::warn("Archived authored transfer failure: {}", path.string());
      }
    } catch (const std::exception &archiveError) {
      spdlog::warn("Authored transfer evidence archive failed: {}",
                   archiveError.what());
    }
    return reject();
  };
  if (!NativeAuthoredBasis::Matches(graph->recordLease)) {
    return rejectTransfer(
        "Winning authored records changed during native transfer");
  }
  error = ValidateAuthoredTransfer(graph->recordLease, graph->originalTopology,
                                   freshTopology);
  if (!error.empty())
    return rejectTransfer(error);
  // Validate the actual retained native representation directly. Shared undo
  // status on an older snapshot is not proof that this form's arrays are
  // invertible. Reconciliation changes ownership journals, never geometry.
  for (const auto &mesh : freshOriginalMeshes) {
    NativeGeometry geometry(*mesh);
    if (!geometry.Valid()) {
      return rejectTransfer("Transferred native geometry invalid: " +
                            geometry.Error());
    }
  }
  // Allocate all retained ownership before changing any journal entry.
  supersededForms.reserve(supersededForms.size() +
                          static_cast<std::uint32_t>(replacements.size()) +
                          supersededOriginals.size() + departingForms.size());
  for (const auto &form : departingForms)
    if (std::find(supersededForms.begin(), supersededForms.end(), form) ==
        supersededForms.end())
      supersededForms.push_back(form);
  for (const auto &form : supersededOriginals)
    supersededForms.push_back(form);
  for (const auto &[sourceID, draft] : graph->forms) {
    const auto replacement = replacements.find(draft->form.get());
    if (replacement == replacements.end())
      continue;
    supersededForms.push_back(
        RE::BSTSmartPointer<RE::NavMesh>{draft->form.get()});
    replacement->second->IncRef();
    draft->form.reset(replacement->second);
  }
  for (std::size_t i = 0; i < cells.size(); ++i) {
    cells[i].published = std::move(updated[i]);
    cells[i].expected = std::move(updatedOriginals[i]);
    cells[i].replacement = std::move(originalBuffers[i]);
  }
  originalIDs = std::move(authoredIDs);
  graph->originalTopology = std::move(freshTopology);
  graph->originalCuts = std::move(freshCuts);
  originalBindingTransfers = std::move(bindings);
  verifiedDormantOriginals = std::move(verifiedDormant);
  if (!originalBindingTransfers.empty())
    spdlog::debug("Validated {} dormant authored mesh bindings for restoration",
                  originalBindingTransfers.size());
  for (const auto &[prior, current] : replacements)
    spdlog::debug("Retained native form transfer for info {:08X}: {} -> {}",
                  current->GetFormID(), static_cast<void *>(prior),
                  static_cast<void *>(current));
  return true;
}
bool NativePublication::OriginalBindingMatches(RE::NavMesh *form,
                                               RE::NavMeshInfo *info) const {
  if (!form || !info || info->navMesh != static_cast<RE::BSNavmesh *>(form))
    return false;
  if (form->unk110 == info)
    return true;
  const auto pending = originalBindingTransfers.find(form);
  return !form->unk110 && pending != originalBindingTransfers.end() &&
         pending->second == info;
}
bool NativePublication::ReconcileLiveCuts(std::string &error) {
  try {
    RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> live;
    for (const auto &[id, draft] : graph->forms)
      live.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{
          static_cast<RE::BSNavmesh *>(draft->form.get())});
    const auto registered = CaptureRegisteredCutData();
    const auto &captured = registered.inputs;
    const auto ownership = CaptureNativeCutReferences(
        live, {}, CutMembershipCapture::RetainExternalTokens);
    RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> originalMeshes;
    std::map<const RE::BSNavmesh *, const RE::BSNavmeshInfo *> retainedBindings;
    for (const auto &[id, topology] : graph->originalTopology) {
      const auto form = RE::TESForm::LookupByID<RE::NavMesh>(id);
      const auto expected = expectedMap.find(id);
      if (!form || expected == expectedMap.end() ||
          !OriginalBindingMatches(form, expected->second))
        throw std::runtime_error("Original cut transfer ownership changed");
      originalMeshes.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{form});
      retainedBindings.emplace(form, expected->second);
    }
    const auto originalOwnership = CaptureNativeCutReferences(
        originalMeshes, {}, CutMembershipCapture::RetainExternalTokens,
        &retainedBindings);
    std::set<std::uint64_t> registeredInputs, referencedInputs, originalInputs;
    for (const auto &input : captured)
      registeredInputs.insert(
          reinterpret_cast<std::uintptr_t>(input.original.get()));
    for (const auto &reference : ownership.references)
      referencedInputs.insert(reference.data);
    for (const auto &reference : originalOwnership.references)
      originalInputs.insert(reference.data);
    std::set<RE::NiRefObject *> retiredInputs;
    std::map<RE::NiRefObject *, const NativeCutDataSource *> successors;
    std::set<RE::NiRefObject *> used;
    std::vector<CutList> updated;
    updated.reserve(cutLists.size());
    for (const auto &list : cutLists) {
      const auto identity = reinterpret_cast<std::uintptr_t>(list.owner.get());
      if (CanDetachCutJournal(registeredInputs.contains(identity),
                              originalInputs.contains(identity),
                              list.array->empty())) {
        // The registry/membership transition has completed, but live undo may
        // still retain this input. Detach only its restoration journal; the
        // source moves to retiredCutData and remains strongly retained.
        retiredInputs.insert(list.owner.get());
        continue;
      }
      CutList copy;
      copy.owner = list.owner;
      copy.array = list.array;
      copy.expected = list.expected;
      copy.published = list.published;
      copy.replacement = list.replacement;
      if (!std::ranges::equal(*list.array, list.published) &&
          SameUniqueCutMembership(*list.array, list.published)) {
        // Membership is a set; retain the current native order as the exact
        // transaction before-image without changing any native storage.
        copy.published = *list.array;
      } else if (!std::ranges::equal(*list.array, list.published)) {
        // Native load transfer clears the predecessor's tracking list. A
        // partial change, moved body, new cut or ambiguous successor is
        // rejected.
        if (!list.array->empty())
          throw std::runtime_error(
              "Native cut membership changed without transfer");
        const auto source =
            std::ranges::find_if(graph->sourceCutData, [&](const auto &value) {
              return value.original == list.owner;
            });
        if (source == graph->sourceCutData.end() || !source->rigidBody)
          throw std::runtime_error("Native cut transfer has no retained body");
        const NativeCutDataSource *match{};
        for (const auto &candidate : captured) {
          if (candidate.original == list.owner || !candidate.owner ||
              candidate.rigidBody != source->rigidBody ||
              candidate.auxiliary != source->auxiliary ||
              !SameCutTransferShape(candidate.values, source->values) ||
              !SameUniqueCutMembership(candidate.affectedInfos, list.published))
            continue;
          if (match && match->original != candidate.original)
            throw std::runtime_error(
                "Native cut transfer has ambiguous successors");
          match = &candidate;
        }
        if (!match || !used.insert(match->original.get()).second)
          throw std::runtime_error(
              "Native cut transfer has no unique matching successor");
        successors.emplace(list.owner.get(), match);
        copy.owner = match->original;
        copy.array = reinterpret_cast<InfoList *>(
            reinterpret_cast<std::byte *>(match->original.get()) + 0xA0);
        copy.published = *copy.array;
      }
      updated.emplace_back(std::move(copy));
    }
    std::set<std::uint64_t> canonicalInputs;
    auto originals = graph->sourceCutData;
    auto retiredOwnership = graph->retiredCutData;
    std::erase_if(originals, [&](const auto &source) {
      if (!retiredInputs.contains(source.original.get()))
        return false;
      retiredOwnership.push_back(source);
      return true;
    });
    for (const auto &source : originals) {
      const auto successor = successors.find(source.original.get());
      canonicalInputs.insert(reinterpret_cast<std::uintptr_t>(
          successor == successors.end() ? source.original.get()
                                        : successor->second->original.get()));
    }
    // Detached journal owners remain valid native undo owners until Skyrim
    // removes their effects. Keeping their tokens here does not register or
    // reapply them, nor does it authorize freeing their retained storage.
    for (const auto &source : retiredOwnership)
      canonicalInputs.insert(
          reinterpret_cast<std::uintptr_t>(source.original.get()));
    std::set<std::uint64_t> publishedInfos;
    for (const auto &[id, draft] : graph->forms)
      publishedInfos.insert(
          reinterpret_cast<std::uintptr_t>(draft->navigationInfo->info));
    std::size_t newcomers = 0;
    for (const auto &source : captured) {
      const auto identity =
          reinterpret_cast<std::uintptr_t>(source.original.get());
      if (canonicalInputs.contains(identity))
        continue;
      const bool affectsPublished =
          std::ranges::any_of(source.affectedInfos, [&](auto *info) {
            return publishedInfos.contains(
                reinterpret_cast<std::uintptr_t>(info));
          });
      if (!affectsPublished && !referencedInputs.contains(identity))
        continue;
      if (!source.owner || !source.ownerGroup || !source.rigidBody)
        throw std::runtime_error(
            "New native cut has incomplete registered ownership");
      CutList list;
      list.owner = source.original;
      list.array = reinterpret_cast<InfoList *>(
          reinterpret_cast<std::byte *>(source.original.get()) + 0xA0);
      list.published = *list.array;
      if (!std::ranges::equal(list.published, source.affectedInfos))
        throw std::runtime_error(
            "New native cut membership changed during capture");
      std::vector<std::uint64_t> current, restored;
      for (auto *info : source.affectedInfos)
        current.push_back(reinterpret_cast<std::uintptr_t>(info));
      std::string membershipError;
      if (!PrepareNewCutMembership(current, publishedInfos, restored,
                                   membershipError))
        throw std::runtime_error(membershipError);
      auto retained = source;
      retained.affectedInfos.clear();
      for (const auto info : restored) {
        auto *pointer = reinterpret_cast<RE::BSNavmeshInfo *>(info);
        list.expected.push_back(pointer);
        retained.affectedInfos.push_back(pointer);
      }
      // Installed journals hold the restoration buffer in replacement.
      list.replacement = list.expected;
      updated.emplace_back(std::move(list));
      originals.emplace_back(std::move(retained));
      canonicalInputs.insert(identity);
      ++newcomers;
    }
    for (const auto &reference : ownership.references)
      if (!canonicalInputs.contains(reference.data))
        throw std::runtime_error(
            "Native cut update introduced an unregistered canonical owner");
    // Native load can transfer live inputs while dormant originals retain the
    // predecessor. Rebind every owning namespace together, using only the
    // unique body/shape/membership successor already validated above.
    std::map<std::uint64_t, std::uint64_t> originalSubstitutions;
    for (const auto &[prior, next] : successors)
      originalSubstitutions.emplace(
          reinterpret_cast<std::uintptr_t>(prior),
          reinterpret_cast<std::uintptr_t>(next->original.get()));
    for (const auto &reference : originalOwnership.references)
      if (!canonicalInputs.contains(reference.data) &&
          !originalSubstitutions.contains(reference.data))
        throw std::runtime_error(
            "Retained original cut has no verified successor");
    std::string referenceError;
    const auto referencePlan = CutReferenceConversion::Prepare(
        originalOwnership, originalSubstitutions, referenceError);
    if (!referencePlan)
      throw std::runtime_error("Original cut transfer: " + referenceError);
    NativeCutOwnerTransfer::Mapping ownerMapping;
    for (const auto &[prior, next] : successors)
      ownerMapping.emplace(prior, next->original.get());
    NativeCutOwnerTransfer ownerTransfer(originalMeshes, ownerMapping);
    for (auto &source : originals) {
      const auto successor = successors.find(source.original.get());
      if (successor == successors.end()) {
        const auto current =
            std::ranges::find_if(captured, [&](const auto &input) {
              return input.original == source.original;
            });
        const auto identity =
            reinterpret_cast<std::uintptr_t>(source.original.get());
        const auto journal =
            std::ranges::find_if(updated, [&](const auto &list) {
              return list.owner == source.original;
            });
        if (CanRefreshRuntimeCut(
                current != captured.end(), originalInputs.contains(identity),
                current != captured.end() && journal != updated.end() &&
                    SameUniqueCutMembership(current->affectedInfos,
                                            journal->published))) {
          if (!current->owner || !current->ownerGroup || !current->rigidBody)
            throw std::runtime_error(
                "Runtime cut refresh has incomplete registered ownership");
          // Keep the restoration membership before-image. Only the current
          // registered shape/ownership baseline changes, inside this capture.
          if (source.rigidBody != current->rigidBody ||
              source.auxiliary != current->auxiliary ||
              source.ownerGroup != current->ownerGroup)
            retiredOwnership.push_back(source);
          source.rigidBody = current->rigidBody;
          source.auxiliary = current->auxiliary;
          source.owner = current->owner;
          source.ownerGroup = current->ownerGroup;
          source.values = current->values;
          source.affectedAllocation = current->affectedAllocation;
          source.retainedOnly = current->retainedOnly;
          source.cutsGeometry = current->cutsGeometry;
        }
        continue;
      }
      source.original = successor->second->original;
      source.owner = successor->second->owner;
      source.ownerGroup = successor->second->ownerGroup;
      source.values = successor->second->values;
      source.affectedAllocation = successor->second->affectedAllocation;
    }
    const auto beforeRebindingCuts = CaptureNativeCuts(originalMeshes);
    if (beforeRebindingCuts != graph->originalCuts)
      throw std::runtime_error(
          "Original cut stacks changed before owner transfer");
    ownerTransfer.Apply();
    const auto rebound = CaptureNativeCutReferences(
        originalMeshes, {}, CutMembershipCapture::RetainExternalTokens,
        &retainedBindings);
    if (rebound.references != referencePlan->After().references)
      throw std::runtime_error(
          "Original cut transfer owning references differ from complete plan");
    auto reboundCuts = CaptureNativeCuts(originalMeshes);
    if (reboundCuts.records != beforeRebindingCuts.records ||
        reboundCuts.auxiliaryIndices != beforeRebindingCuts.auxiliaryIndices ||
        reboundCuts.obstacleEntries != beforeRebindingCuts.obstacleEntries)
      throw std::runtime_error(
          "Original cut structure changed during owner transfer");
    // All fallible capture and allocation work is complete. Publish the owned
    // journal with nonallocating swaps; no geometry or native membership moves.
    graph->sourceCutData.swap(originals);
    graph->retiredCutData.swap(retiredOwnership);
    cutLists.swap(updated);
    graph->originalCuts.records.swap(reboundCuts.records);
    graph->originalCuts.auxiliaryIndices.swap(reboundCuts.auxiliaryIndices);
    graph->originalCuts.obstacleEntries.swap(reboundCuts.obstacleEntries);
    graph->originalCuts.ownership.swap(reboundCuts.ownership);
    ownerTransfer.Commit();
    if (newcomers)
      spdlog::info(
          "Adopted {} registered native cut newcomers; retained original cuts unchanged",
          newcomers);
    if (!retiredInputs.empty())
      spdlog::info(
          "Detached {} removed cut journals; native undo owners remain retained",
          retiredInputs.size());
    spdlog::debug(
        "Reconciled {} native cut-data transfers and {} live owning references",
        successors.size(), ownership.references.size());
    spdlog::debug("Retained original cut transfer: {} owning slots rebound; "
                  "geometry and native tracking lists unchanged",
                  ownerTransfer.Size());
    return true;
  } catch (const std::exception &exception) {
    error = exception.what();
    CaptureCutEvidence(error);
    return false;
  }
}
void NativePublication::CaptureCutEvidence(
    std::string_view event) const noexcept {
  if (!Diagnostics::Enabled())
    return;
  try {
    if (const auto directory = SkyBuild::Paths::DiagnosticsDirectory()) {
      RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> live, originals;
      std::map<std::uint32_t, AuthoredMesh> liveTopology, originalTopology;
      for (const auto &[id, draft] : graph->forms) {
        live.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{draft->form.get()});
        liveTopology.emplace(draft->ID(),
                             CaptureNativeTopology(*draft->form, draft->ID()));
      }
      for (const auto &[id, previous] : graph->originalTopology) {
        const auto source = RE::TESForm::LookupByID<RE::NavMesh>(id);
        if (!source)
          continue;
        originals.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{source});
        originalTopology.emplace(id, CaptureNativeTopology(*source, id));
      }
      nlohmann::json failure{
          {"schema", 1},
          {"kind", "cut-lifecycle-reconciliation-failure"},
          {"stage", "retirement-input-closure"},
          {"error", event},
          {"event", event},
          {"authoredTopology", EncodeAuthoredEvidence(liveTopology)},
          {"originalTopology", EncodeAuthoredEvidence(originalTopology)},
          {"priorOriginalTopology",
           EncodeAuthoredEvidence(graph->originalTopology)},
          {"priorOriginalCuts", EncodeCutEvidence(graph->originalCuts)},
          {"captureErrors", nlohmann::json::array()},
          {"nativeWriterExclusion", false}};
      failure["publishedInfos"] = nlohmann::json::array();
      for (const auto &[id, draft] : graph->forms)
        failure["publishedInfos"].push_back(
            reinterpret_cast<std::uintptr_t>(draft->navigationInfo->info));
      failure["trackedInputs"] = nlohmann::json::array();
      for (const auto &source : graph->sourceCutData)
        failure["trackedInputs"].push_back(
            reinterpret_cast<std::uintptr_t>(source.original.get()));
      { // Failure-only evidence; successful reconciliations do not dump
        // geometry.
        failure["trackedCutStates"] = nlohmann::json::array();
        for (const auto &source : graph->sourceCutData) {
          const auto *bytes =
              reinterpret_cast<const std::byte *>(source.original.get());
          std::array<std::uint8_t, 0x80> expected{}, current{};
          std::memcpy(expected.data(), source.values.data(), expected.size());
          std::memcpy(current.data(), bytes + 0x20, current.size());
          RE::NiRefObject *body{}, *auxiliary{};
          std::memcpy(&body, bytes + 0x18, sizeof(body));
          std::memcpy(&auxiliary, bytes + 0xB8, sizeof(auxiliary));
          failure["trackedCutStates"].push_back(
              {{"identity",
                reinterpret_cast<std::uintptr_t>(source.original.get())},
               {"expectedBody",
                reinterpret_cast<std::uintptr_t>(source.rigidBody.get())},
               {"currentBody", reinterpret_cast<std::uintptr_t>(body)},
               {"expectedAuxiliary",
                reinterpret_cast<std::uintptr_t>(source.auxiliary.get())},
               {"currentAuxiliary",
                reinterpret_cast<std::uintptr_t>(auxiliary)},
               {"expectedValues", expected},
               {"currentValues", current}});
        }
      }
      failure["bindings"] = nlohmann::json::array();
      for (const auto &[sourceID, draft] : graph->forms) {
        auto *form = draft->form.get();
        auto *mesh = static_cast<RE::BSNavmesh *>(form);
        auto *info = draft->navigationInfo->info;
        const auto id = draft->ID();
        const auto found = registry->infoMap.find(id);
        form->IncRef();
        const auto references = form->DecRef();
        const auto pointer = [](const auto *value) {
          return reinterpret_cast<std::uintptr_t>(value);
        };
        failure["bindings"].push_back(
            {{"sourceID", sourceID},
             {"id", id},
             {"form", pointer(form)},
             {"mesh", pointer(mesh)},
             {"formVtable", *reinterpret_cast<const std::uintptr_t *>(form)},
             {"meshVtable", *reinterpret_cast<const std::uintptr_t *>(mesh)},
             {"references", references},
             {"info", pointer(info)},
             {"infoMesh", pointer(info->navMesh)},
             {"meshInfo", pointer(form->unk110)},
             {"lookup", pointer(RE::TESForm::LookupByID<RE::NavMesh>(id))},
             {"registeredInfo",
              found == registry->infoMap.end() ? 0 : pointer(found->second)}});
      }
      failure["supersededForms"] = nlohmann::json::array();
      for (const auto &form : supersededForms) {
        const auto *mesh = static_cast<const RE::BSNavmesh *>(form.get());
        std::uint32_t references{};
        // Native BSIntrusiveRefCounted stores its reference word at +8.
        // Read it without invoking a possibly damaged object's virtual table.
        std::memcpy(&references, reinterpret_cast<const std::byte *>(mesh) + 8,
                    sizeof(references));
        failure["supersededForms"].push_back(
            {{"form", reinterpret_cast<std::uintptr_t>(form.get())},
             {"mesh", reinterpret_cast<std::uintptr_t>(mesh)},
             {"id", form->GetFormID()},
             {"references", references},
             {"meshVtable", *reinterpret_cast<const std::uintptr_t *>(mesh)},
             {"meshInfo", reinterpret_cast<std::uintptr_t>(mesh->unk110)}});
      }
      // A failed sub-capture must not discard all the other owned evidence.
      // Missing native payload stays explicit rather than being guessed.
      const auto capture = [&](const char *field, auto &&read) {
        try {
          failure[field] = read();
        } catch (const std::exception &captureError) {
          failure["captureErrors"].push_back(
              {{"field", field}, {"error", captureError.what()}});
        }
      };
      capture("trackedMemberships", [&] {
        auto entries = nlohmann::json::array();
        for (const auto &list : cutLists) {
          nlohmann::json current = nlohmann::json::array();
          for (const auto *info : *list.array)
            current.push_back(reinterpret_cast<std::uintptr_t>(info));
          entries.push_back(
              {{"identity", reinterpret_cast<std::uintptr_t>(list.owner.get())},
               {"members", std::move(current)}});
        }
        return entries;
      });
      capture("nativeCuts",
              [&] { return EncodeCutEvidence(CaptureNativeCuts(live)); });
      capture("originalCuts",
              [&] { return EncodeCutEvidence(CaptureNativeCuts(originals)); });
      std::vector<RE::NiPointer<RE::NiRefObject>> inputs;
      capture("registeredInputs", [&] {
        const auto census = CaptureRegisteredCutData();
        auto registered = nlohmann::json::array();
        for (const auto &input : census.inputs) {
          inputs.push_back(input.original);
          registered.push_back(
              {{"identity",
                reinterpret_cast<std::uintptr_t>(input.original.get())},
               {"body",
                reinterpret_cast<std::uintptr_t>(input.rigidBody.get())},
               {"group",
                reinterpret_cast<std::uintptr_t>(input.ownerGroup.get())}});
        }
        return registered;
      });
      capture("referencePlan", [&] {
        return EncodeCutReferenceEvidence(
            CaptureNativeCutReferences(
                live, inputs, CutMembershipCapture::RetainExternalTokens),
            {});
      });
      std::map<const RE::BSNavmesh *, const RE::BSNavmeshInfo *>
          retainedBindings;
      for (const auto &mesh : originals) {
        const auto expected = expectedMap.find(mesh->QNavmeshID());
        if (expected != expectedMap.end())
          retainedBindings.emplace(mesh.get(), expected->second);
      }
      capture("originalReferencePlan", [&] {
        return EncodeCutReferenceEvidence(
            CaptureNativeCutReferences(
                originals, {}, CutMembershipCapture::RetainExternalTokens,
                &retainedBindings),
            {});
      });
      const auto path = ArchiveFailure(*directory / "SkyBuild-navmesh-failures",
                                       failure.dump());
      spdlog::warn("Archived cut lifecycle reconciliation failure: {}",
                   path.string());
    }
  } catch (const std::exception &archiveError) {
    spdlog::warn("Cut lifecycle evidence archive failed: {}",
                 archiveError.what());
  }
}
} // namespace SkyBuild::Navmesh
