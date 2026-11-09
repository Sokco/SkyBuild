#include "SkyBuildPaths.h"
#include "navmesh/native/NativeGraphDraft.h"
#include "navmesh/cuts/CanonicalCutMembership.h"
#include "navmesh/cuts/CutEffectDomain.h"
#include "navmesh/cuts/CutEpochPlan.h"
#include "navmesh/cuts/CutOperationState.h"
#include "navmesh/cuts/CutPortalRepair.h"
#include "navmesh/cuts/FreshCutComparison.h"
#include "navmesh/cuts/NativeCellQueryView.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/NativeCutEffects.h"
#include "navmesh/cuts/NativeCutReferences.h"
#include "navmesh/diagnostics/CutReferenceEvidence.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/diagnostics/FailureArchive.h"
#include "navmesh/diagnostics/OwnedNavmeshEvidence.h"
#include "navmesh/diagnostics/TimingTrace.h"
#include "navmesh/generation/GeneratedTopology.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeGeometry.h"
#include "navmesh/native/NativeInfoResolver.h"
#include "navmesh/native/NativeLocationCache.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include <bit>
#include <cstring>
namespace SkyBuild::Navmesh {
void NativeGraphDraft::ReleaseRetiredValidation() {
  // Published forms, infos and complete cut-input ownership stay alive,
  // including newcomers with no staging clone to retain their bodies/groups.
  // Only detached comparison copies are discarded.
  const auto release = []<class T>(T &value) { value = T{}; };
  release(originalTopology);
  release(originalCuts);
  release(recordLease);
  release(expectedCuts);
  release(freshCutPhase);
  release(cutOperationEvidence);
  release(discoveryEvidence);
  release(expected);
  release(expectedDoors);
  release(expectedClosedDoors);
  for (auto &[id, draft] : forms)
    draft->expected.reset();
}

namespace {
struct RetainedLinks {
  RE::BSTSmartPointer<RE::BSPathingCell> parent;
  RE::BSTArray<RE::BSNavmeshTriangleDoorPortal> doors;
  RE::BSTArray<RE::BSNavmeshClosedDoorInfo> closedDoors;
};
} // namespace
std::unique_ptr<NativeGraphDraft>
NativeGraphDraft::Create(const PartitionedMesh &graph,
                         const RE::BSTSmartPointer<RE::BSPathingCell> &fallback,
                         const std::map<std::uint32_t, AuthoredMesh> &originals,
                         const NativeCutSnapshot &originalCuts,
                         const AuthoredBasisSnapshot &recordLease,
                         std::string &error) {
  TimingTrace timing{"NativeGraphDraft::Create"};
  const auto audit = AuditAuthoredTopology(graph.meshes);
  if (!graph.Valid() || !audit.Valid() || audit.unloadedTargets ||
      graph.meshes.empty() || graph.meshes.size() > 1024) {
    error = "Invalid complete graph for native staging";
    return nullptr;
  }
  auto result = std::make_unique<NativeGraphDraft>();
  for (const auto &portal : graph.deferredPortals) {
    result->deferredTargets.insert(portal.destination.targetMesh);
    if (portal.destination.type == 0)
      result->deferredBoundaries.insert(
          std::minmax(portal.sourceMesh, portal.destination.targetMesh));
  }
  if (!result->deferredTargets.empty())
    spdlog::debug("Navigation has {} deferred boundary targets; native edges "
                  "remain closed until neighbor attachment",
                  result->deferredTargets.size());
  result->originalTopology = originals;
  result->originalCuts = originalCuts;
  result->recordLease = recordLease;
  if (!NativeAuthoredBasis::Matches(result->recordLease)) {
    error = "Winning authored records changed before native staging";
    return nullptr;
  }
  std::size_t vertices{}, triangles{};
  for (const auto &[id, mesh] : graph.meshes) {
    vertices += mesh.vertices.size();
    triangles += mesh.triangles.size();
    if (vertices > 1'000'000 || triangles > 1'000'000) {
      error = "Complete native graph staging budget exceeded";
      return nullptr;
    }
  }
  std::map<std::uint32_t, RetainedLinks> links;
  timing.Mark("audit and graph budgets");
  {
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::RelocationID(515948, 402045)};
    const RE::BSWriteLockGuard guard(*publication.get());
    for (const auto &[id, mesh] : graph.meshes) {
      if (mesh.nativeDependencies ||
          std::any_of(
              mesh.triangles.begin(), mesh.triangles.end(),
              [](const auto &face) { return (face.flags & 32U) != 0; })) {
        error = "Connected graph staging requires cut-free portable geometry";
        return nullptr;
      }
      auto &retained = links[id];
      const auto source = RE::TESForm::LookupByID<RE::NavMesh>(id);
      if (source) {
        retained.parent = source->parentCell;
        retained.doors = source->doorPortals;
        retained.closedDoors = source->closedDoors;
      } else {
        retained.parent = fallback;
      }
      if (const auto mapping = graph.doorRecordSources.find(id);
          mapping != graph.doorRecordSources.end()) {
        decltype(retained.doors) doors;
        decltype(retained.closedDoors) closed;
        for (const auto record : mapping->second.doors) {
          if (record >= retained.doors.size()) {
            error = "Invalid original door record mapping";
            return nullptr;
          }
          doors.push_back(retained.doors[record]);
        }
        for (const auto record : mapping->second.closedDoors) {
          if (record >= retained.closedDoors.size()) {
            error = "Invalid original closed door record mapping";
            return nullptr;
          }
          closed.push_back(retained.closedDoors[record]);
        }
        retained.doors = std::move(doors);
        retained.closedDoors = std::move(closed);
      }
      if (!retained.parent ||
          retained.doors.size() != mesh.doorTriangles.size() ||
          retained.closedDoors.size() != mesh.closedDoorTriangles.size()) {
        error = "Missing native graph parent or retained door records";
        return nullptr;
      }
    }
  }
  timing.Mark("retain native parents and door records");
  for (const auto &[id, mesh] : graph.meshes) {
    Mesh isolated;
    const auto meshTiming =
        timing.Measure("per mesh: geometry and native form creation", id);
    isolated.vertices = mesh.vertices;
    for (const auto &face : mesh.triangles) {
      Triangle triangle;
      triangle.flags = face.flags & ~7U;
      for (unsigned side = 0; side < 3; ++side) {
        triangle.vertices[side] = face.vertices[side];
        triangle.neighbors[side] =
            (face.flags & (1U << side)) || face.edges[side] == 0xFFFF
                ? -1
                : face.edges[side];
      }
      isolated.triangles.push_back(triangle);
    }
    auto form = NativeFormDraft::Create(isolated, links.at(id).parent, error);
    if (!form)
      return nullptr;
    result->forms.emplace(id, std::move(form));
  }
  for (const auto &[id, mesh] : graph.meshes) {
    auto &draft = *result->forms.at(id);
    const auto meshTiming =
        timing.Measure("per mesh: native links and cover", id);
    auto &native = *draft.form;
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
      const auto &face = mesh.triangles[index];
      auto &target = native.triangles[static_cast<std::uint32_t>(index)];
      std::memcpy(&target.triangleFlags, &face.flags, sizeof(face.flags));
      std::memcpy(&target.traversalFlags, &face.traversal,
                  sizeof(face.traversal));
      for (unsigned side = 0; side < 3; ++side)
        target.triangles[side] = face.edges[side];
    }
    auto expected = mesh;
    expected.formID = draft.ID();
    for (auto &portal : expected.portals) {
      const auto target = result->forms.find(portal.targetMesh);
      if (target == result->forms.end()) {
        error = "Unresolved detached portal destination";
        return nullptr;
      }
      portal.targetMesh = target->second->ID();
      RE::BSNavmeshEdgeExtraInfo record{};
      record.type = static_cast<RE::EDGE_EXTRA_INFO_TYPE>(portal.type);
      record.portal.otherMeshID = portal.targetMesh;
      record.portal.triangle = portal.targetTriangle;
      std::memcpy(&record.portal.edgeIndex, &portal.auxiliary,
                  sizeof(portal.auxiliary));
      native.extraEdgeInfo.push_back(record);
    }
    native.doorPortals = links.at(id).doors;
    native.closedDoors = links.at(id).closedDoors;
    for (std::size_t index = 0; index < expected.doorTriangles.size(); ++index)
      native.doorPortals[static_cast<std::uint32_t>(index)]
          .owningTriangleIndex = expected.doorTriangles[index];
    for (std::size_t index = 0; index < expected.closedDoorTriangles.size();
         ++index)
      native.closedDoors[static_cast<std::uint32_t>(index)].triangleIndex =
          expected.closedDoorTriangles[index];
    using BuildCover = void (*)(RE::BSNavmesh *);
    const REL::Relocation<BuildCover> buildCover{REL::RelocationID(88641, 91071)};
    {
      const auto scope = timing.Measure("native BuildCover 91071", id);
      buildCover(&native);
    }
    expected.coverTriangles = BuildAuthoredCover(expected.triangles);
    draft.expected = std::make_unique<NativeGeometry>(native);
    result->expectedDoors.emplace(expected.formID, native.doorPortals);
    result->expectedClosedDoors.emplace(expected.formID, native.closedDoors);
    result->expected.emplace(expected.formID, std::move(expected));
  }
  std::map<std::uint32_t, RE::BSNavmeshInfo *> infos;
  timing.Mark("create forms, copy links, build cover");
  for (const auto &[sourceID, draft] : result->forms) {
    if (NativeDraftInfo::Registered(draft->ID()) || !draft->navigationInfo ||
        !draft->navigationInfo->info ||
        draft->form->unk110 != draft->navigationInfo->info) {
      error = "Detached graph info ownership preflight failed";
      return nullptr;
    }
    for (const auto &face : draft->form->triangles) {
      std::uint16_t flags{};
      std::memcpy(&flags, &face.triangleFlags, sizeof(flags));
      for (unsigned side = 0; side < 3; ++side)
        if ((flags & (1U << side)) &&
            face.triangles[side] >= draft->form->extraEdgeInfo.size()) {
          error = "Detached graph external edge index is invalid";
          return nullptr;
        }
    }
    infos.emplace(draft->ID(), draft->navigationInfo->info);
  }
  try {
    std::map<std::uint32_t, RE::BSNavmeshInfo *> sourceInfos;
    for (const auto &[sourceID, draft] : result->forms)
      sourceInfos.emplace(sourceID, draft->navigationInfo->info);
    NativeInfoResolver resolver(infos);
    for (const auto &[sourceID, draft] : result->forms) {
      const auto scope =
          timing.Measure("PopulateConnected before cuts", sourceID);
      draft->navigationInfo->PopulateConnected(*draft->form);
    }
    if (resolver.Misses()) {
      error =
          std::format("Detached connected info requested unknown mesh {:08X}",
                      resolver.LastMissingID());
      return nullptr;
    }
  } catch (const std::exception &exception) {
    error = std::string("Detached connected info failed: ") + exception.what();
    return nullptr;
  }
  timing.Mark("clone cut inputs and connect native infos");
  error = result->ValidationError();
  timing.Mark("graph validation before cuts");
  if (!error.empty())
    return nullptr;
  const char *preparationStage = "native-cut-materialization";
  try {
    result->MaterializeCuts();
    timing.Mark("native cut materialization");
  } catch (const std::exception &exception) {
    error = std::string("Native graph preparation failed (") +
            preparationStage + "): " + exception.what();
    try {
      if (const auto directory = SkyBuild::Paths::DiagnosticsDirectory();
          Diagnostics::Enabled() && directory) {
        nlohmann::json evidence{
            {"schema", 1},
            {"kind", "publication-preparation-failure"},
            {"stage", preparationStage},
            {"freshCutPhase", result->freshCutPhase},
            {"discoveryEvidence",
             result->discoveryEvidence.empty()
                 ? nlohmann::json{}
                 : nlohmann::json::parse(result->discoveryEvidence)},
            {"cutOperation",
             result->cutOperationEvidence.empty()
                 ? nlohmann::json{}
                 : nlohmann::json::parse(result->cutOperationEvidence)},
            {"error", error},
            {"nativePublicationAuthorized", false},
            {"missingEvidence",
             {"failure-time native geometry and complete binding epoch"}}};
        // Capture the still-owned failure state before destroying the private
        // graph. A diagnostic failure must never replace the original error.
        try {
          std::map<std::uint32_t, AuthoredMesh> failedGeometry;
          RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> failedCohort;
          evidence["privateBindings"] = nlohmann::json::array();
          for (const auto &[sourceID, draft] : result->forms) {
            failedGeometry.emplace(
                draft->ID(), CaptureNativeTopology(*draft->form, draft->ID()));
            failedCohort.push_back(
                RE::BSTSmartPointer<RE::BSNavmesh>{draft->form.get()});
            evidence["privateBindings"].push_back(
                {{"source", sourceID},
                 {"expectedID", draft->ID()},
                 {"actualID", draft->form->GetFormID()},
                 {"mesh", reinterpret_cast<std::uintptr_t>(
                              static_cast<RE::BSNavmesh *>(draft->form.get()))},
                 {"info", reinterpret_cast<std::uintptr_t>(
                              draft->navigationInfo->info)}});
          }
          evidence["privateUncutGeometry"] =
              EncodeAuthoredEvidence(result->expected);
          evidence["privateFailureGeometry"] =
              EncodeAuthoredEvidence(failedGeometry);
          evidence["privateFailureCuts"] =
              EncodeCutEvidence(CaptureNativeCuts(failedCohort));
          evidence["privateFailureReferences"] = EncodeCutReferenceEvidence(
              CaptureNativeCutReferences(failedCohort), {});
        } catch (const std::exception &captureError) {
          evidence["privateFailureCaptureError"] = captureError.what();
        }
        const auto path = ArchiveFailure(
            *directory / "SkyBuild-navmesh-failures", evidence.dump());
        spdlog::warn("Archived native cut preparation failure to {}",
                     path.string());
      }
    } catch (const std::exception &archiveError) {
      spdlog::warn("Cut preparation evidence archive failed: {}",
                   archiveError.what());
    }
    return nullptr;
  }
  error = result->ValidationError();
  timing.Mark("graph validation after cuts");
  return error.empty() ? std::move(result) : nullptr;
}
void NativeGraphDraft::MaterializeCuts() {
  TimingTrace timing{"NativeGraphDraft::MaterializeCuts"};
  std::map<std::uint32_t, RE::BSNavmeshInfo *> infos;
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> borrowed;
  for (const auto &[sourceID, draft] : forms) {
    auto *mesh = static_cast<RE::BSNavmesh *>(draft->form.get());
    infos.emplace(draft->ID(), draft->navigationInfo->info);
    borrowed.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{mesh});
  }
  using Operation =
      bool (*)(RE::BSNavmesh *, const RE::NiPointer<RE::NiRefObject> &,
               NativeCutEffects &);
  const REL::Relocation<Operation> apply{REL::RelocationID(88706, 91150)};
  auto resolver = std::make_unique<NativeInfoResolver>(infos);
  const auto captureDomains = [&] {
    std::map<const RE::BSNavmesh *, std::uint32_t> result;
    for (const auto &[id, draft] : forms)
      result.emplace(static_cast<const RE::BSNavmesh *>(draft->form.get()),
                     draft->form->triangles.size());
    return result;
  };
  auto executeOperation = [&](Operation operation, RE::BSNavmesh *current,
                              const RE::NiPointer<RE::NiRefObject> &input,
                              NativeCutEffects &effects) {
    const auto before = captureDomains();
    const bool accepted = operation(current, input, effects);
    const auto after = captureDomains();
    unsigned listIndex{};
    for (const auto *list : {&effects.first, &effects.second}) {
      if (list->size() > 8192)
        throw std::runtime_error("Connected cut edge budget exceeded");
      for (const auto &edge : *list) {
        const auto mesh = edge.mesh ? edge.mesh : current;
        const auto prior = before.find(mesh), next = after.find(mesh);
        const auto oldCount =
            prior == before.end() ? std::nullopt : std::optional{prior->second};
        const auto newCount =
            next == after.end() ? std::nullopt : std::optional{next->second};
        const auto domain =
            ClassifyCutEffect(edge.triangle, edge.edge, oldCount, newCount);
        if (domain == CutEffectDomain::Invalid)
          throw std::runtime_error(std::format(
              "Cut {} effect outside operation domains: list {}, mesh {}, "
              "triangle {}, edge {}, before {}, after {}",
              "apply", listIndex, static_cast<const void *>(mesh),
              edge.triangle, edge.edge,
              oldCount ? std::to_string(*oldCount) : "outside cohort",
              newCount ? std::to_string(*newCount) : "outside cohort"));
        spdlog::debug("Detached cut {} list {}: mesh {:08X}, triangle {}, edge "
                      "{}, transition domain {}",
                      "apply", listIndex, mesh->QNavmeshID(), edge.triangle,
                      edge.edge, static_cast<unsigned>(domain));
      }
      ++listIndex;
    }
    return accepted;
  };
  auto finishResolution = [&] {
    if (resolver->Misses())
      throw std::runtime_error(
          std::format("Connected cut requested unknown mesh {:08X}",
                      resolver->LastMissingID()));
    resolver.reset();
  };
  auto connectivity = [&](const std::map<std::uint32_t, AuthoredMesh> &graph) {
    const auto scope = timing.Measure("copy cut connectivity");
    for (const auto &[sourceID, draft] : forms) {
      auto &native = *draft->form;
      const auto &mesh = graph.at(draft->ID());
      if (native.triangles.size() != mesh.triangles.size() ||
          mesh.portals.size() >= 65535)
        throw std::runtime_error("Cut connectivity journal dimensions changed");
      native.extraEdgeInfo.resize(
          static_cast<std::uint32_t>(mesh.portals.size()));
      for (std::size_t index = 0; index < mesh.portals.size(); ++index) {
        const auto &portal = mesh.portals[index];
        auto &target = native.extraEdgeInfo[static_cast<std::uint32_t>(index)];
        target.type = static_cast<RE::EDGE_EXTRA_INFO_TYPE>(portal.type);
        target.portal.otherMeshID = portal.targetMesh;
        target.portal.triangle = portal.targetTriangle;
        std::memcpy(&target.portal.edgeIndex, &portal.auxiliary,
                    sizeof(portal.auxiliary));
      }
      for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        auto &target = native.triangles[static_cast<std::uint32_t>(index)];
        const auto &face = mesh.triangles[index];
        std::memcpy(&target.triangleFlags, &face.flags, sizeof(face.flags));
        for (unsigned side = 0; side < 3; ++side)
          target.triangles[side] = face.edges[side];
      }
    }
  };
  // Operations and input identities are separate. One registered input can
  // discover several meshes, including candidates which create no undo record.
  std::vector<std::pair<std::uint32_t, std::size_t>> operations;
  {
    freshCutPhase = "discover-registered-runtime-inputs";
    std::map<RE::TESObjectCELL *, RE::NavMeshArray> cells;
    std::map<RE::NavMesh *, std::uint32_t> owners;
    std::map<std::uint32_t, RE::BSNavmeshInfo *> candidateInfos;
    std::set<const void *> originalInfos;
    for (const auto &[sourceID, draft] : forms) {
      auto *parent = draft->form->GetSaveParentCell();
      if (!parent)
        throw std::runtime_error("Runtime cut discovery has no parent cell");
      cells[parent].navMeshes.push_back(
          RE::BSTSmartPointer<RE::NavMesh>{draft->form.get()});
      owners.emplace(draft->form.get(), sourceID);
      candidateInfos.emplace(sourceID, draft->navigationInfo->info);
      if (const auto source = RE::TESForm::LookupByID<RE::NavMesh>(sourceID))
        if (source->unk110)
          originalInfos.insert(source->unk110);
    }
    const auto census = CaptureRegisteredCutData();
    const auto obsoleteGenerated = NativeLocationCache::ObsoleteGenerated();
    NativeCellQueryView view(cells);
    const bool diagnostics = Diagnostics::Enabled();
    nlohmann::json ignored;
    nlohmann::json discovery;
    if (diagnostics)
      discovery = nlohmann::json{{"kind", "registered-runtime-cut-discovery"},
                                 {"registeredCount", census.inputs.size()},
                                 {"inputs", nlohmann::json::array()},
                                 {"complete", false}};
    // Retain owned discovery evidence even if input cloning, discovery or the
    // coverage check rejects. Serialize once when leaving this scope, rather
    // than repeatedly writing checkpoints inside the registry scan.
    struct SaveDiscovery {
      nlohmann::json &evidence;
      NativeCellQueryView &view;
      std::string &destination;
      ~SaveDiscovery() noexcept {
        if (!Diagnostics::Enabled())
          return;
        try {
          evidence["hits"] = view.Hits();
          evidence["cohortMisses"] = view.CohortMisses();
          evidence["outsideQueries"] = view.OutsideQueries();
          destination = evidence.dump();
        } catch (...) {
          // Diagnostic allocation failure must not replace the native error.
        }
      }
    } saveDiscovery{discovery, view, discoveryEvidence};
    std::vector<std::unique_ptr<NativeCutDataDraft>> inputs;
    std::vector<NativeCutDataSource> canonical;
    std::vector<CutEpochInput> discovered;
    std::map<std::uint64_t, std::size_t> inputIndexes;
    std::set<std::uint32_t> newCohort;
    for (const auto &[id, draft] : forms)
      newCohort.insert(id);
    operations.clear();
    using Results = RE::BSScrapArray<RE::BSTSmartPointer<RE::NavMesh>>;
    using Discover =
        bool (*)(const RE::NiPointer<RE::NiRefObject> &, Results *);
    const REL::Relocation<Discover> discover{REL::RelocationID(29755, 30578)};
    for (const auto &source : census.inputs) {
      auto &observation =
          diagnostics
              ? discovery["inputs"].emplace_back(nlohmann::json{
                    {"input",
                     reinterpret_cast<std::uintptr_t>(source.original.get())},
                    {"eligible", !source.retainedOnly &&
                                     source.values[0x78] != std::byte{}},
                    {"retainedOnly", source.retainedOnly},
                    {"cutsGeometry", source.cutsGeometry},
                    {"previousInfos", nlohmann::json::array()},
                    {"discovered", false}})
              : ignored;
      if (diagnostics)
        for (const auto *info : source.affectedInfos)
          observation["previousInfos"].push_back(
              reinterpret_cast<std::uintptr_t>(info));
      std::string error;
      auto input = NativeCutDataDraft::Create(source, error);
      if (!input)
        throw std::runtime_error("Runtime cut input: " + error);
      input->ClearAffectedInfos();
      Results candidates;
      // Mirror the native eligibility gate. A declined native discovery must
      // not be represented as a successful empty rediscovery.
      if (!source.retainedOnly && source.values[0x78] != std::byte{} &&
          !discover(input->data, &candidates))
        throw std::runtime_error("Registered runtime cut discovery declined");
      std::set<std::uint32_t> selected;
      for (const auto &mesh : candidates) {
        const auto owner = mesh ? owners.find(mesh.get()) : owners.end();
        if (owner == owners.end())
          throw std::runtime_error(
              "Runtime cut discovery escaped staged cells");
        selected.insert(owner->second);
      }
      const bool previouslyAffected =
          std::ranges::any_of(source.affectedInfos, [&](auto *info) {
            return originalInfos.contains(info) ||
                   obsoleteGenerated.contains(info);
          });
      if (diagnostics) {
        observation["candidates"] = selected;
        observation["previouslyInCohort"] = previouslyAffected;
        observation["discovered"] = true;
      }
      if (selected.empty() && !previouslyAffected)
        continue; // Preserve unrelated registered inputs without journaling
                  // them.
      if (inputs.size() >= 4096)
        throw std::runtime_error(
            "Relevant registered cut input budget exceeded");
      input->expected.affectedMeshes.assign(selected.begin(), selected.end());
      input->RemapAffectedInfos(candidateInfos);
      const auto index = inputs.size();
      const auto identity = reinterpret_cast<std::uintptr_t>(input->data.get());
      discovered.push_back({identity, {}, {}, selected});
      inputIndexes.emplace(identity, index);
      canonical.push_back(source);
      inputs.push_back(std::move(input));
    }
    if (view.CohortMisses())
      throw std::runtime_error(
          "Registered runtime cut discovery has stale cohort cell identities");
    // This is a clean detached epoch. World retirement is a separate
    // publication obligation; it is not represented as already completed here.
    const auto plan = PlanCutEpochs(discovered, {}, newCohort, true);
    if (!plan.Valid())
      throw std::runtime_error("Runtime cut plan: " + plan.error);
    for (const auto &[identity, mesh] : plan.applyCandidates) {
      const auto index = inputIndexes.at(identity);
      // Discovery is also used by native closed-door access metadata. Its
      // membership must survive publication, but its operation is not 91150.
      // Door records have already been carried/remapped with graph topology.
      if (canonical[index].cutsGeometry)
        operations.emplace_back(mesh, index);
    }
    if (diagnostics) {
      discovery["operations"] = operations;
      discovery["complete"] = true;
    }
    cutInputs = std::move(inputs);
    sourceCutData = std::move(canonical);
    spdlog::debug(
        "Runtime fresh-cut plan: {} registered inputs, {} relevant inputs, "
        "{} candidate operations, {} outside-cohort cell queries excluded. "
        "Candidates are independent of old undo membership.",
        census.inputs.size(), cutInputs.size(), operations.size(),
        view.OutsideQueries());
  }
  std::map<std::uint32_t, AuthoredMesh> cutState;
  NativeCutSnapshot captured;
  CutPortalRepair repaired;
  {
    const auto cycle = timing.Measure("materialize cuts");
    std::size_t materialized{};
    for (std::size_t operation = 0; operation < operations.size();
         ++operation) {
      const auto [sourceID, index] = operations[operation];
      const auto found = forms.find(sourceID);
      if (found == forms.end() || index >= cutInputs.size())
        throw std::runtime_error("Connected cut owner/input is missing");
      freshCutPhase = std::format("apply input {} of {} to source {:08X}",
                                  operation + 1, operations.size(), sourceID);
      auto &mesh = *found->second->form;
      const auto beforeCount = mesh.obstacles.size();
      std::map<std::uint32_t, AuthoredMesh> before;
      NativeCutSnapshot beforeCuts;
      {
        for (const auto &[id, draft] : forms)
          before.emplace(draft->ID(),
                         CaptureNativeTopology(*draft->form, draft->ID()));
        beforeCuts = CaptureNativeCuts(borrowed);
      }
      NativeCutEffects effects;
      bool applied;
      {
        const auto scope = timing.Measure("native apply cut 91150", sourceID);
        applied = executeOperation(apply.get(), &mesh, cutInputs[index]->data,
                                   effects);
      }
      if (applied && mesh.obstacles.size() == beforeCount + 1) {
        ++materialized;
      } else if (mesh.obstacles.size() == beforeCount &&
                 effects.first.empty() && effects.second.empty()) {
        std::map<std::uint32_t, AuthoredMesh> after;
        for (const auto &[id, draft] : forms)
          after.emplace(draft->ID(),
                        CaptureNativeTopology(*draft->form, draft->ID()));
        const auto afterCuts = CaptureNativeCuts(borrowed);
        const bool geometryChanged = !SameCutOperationGeometry(before, after);
        if (geometryChanged || afterCuts != beforeCuts) {
          // A no-append result does not prove an atomic no-op. Preserve the
          // complete transition before the private graph is destroyed; never
          // infer which flags or ownership changes are safe to discard.
          if (Diagnostics::Enabled())
            cutOperationEvidence = nlohmann::json{
                {"index", operation},
                {"sourceID", sourceID},
                {"runtimeID", found->second->ID()},
                {"returned", applied},
                {"input", reinterpret_cast<std::uintptr_t>(
                              sourceCutData.at(index).original.get())},
                {"privateInput", reinterpret_cast<std::uintptr_t>(
                                     cutInputs[index]->data.get())},
                {"retainedOnly", sourceCutData.at(index).retainedOnly},
                {"effectsFirst", effects.first.size()},
                {"effectsSecond", effects.second.size()},
                {"geometryChanged", geometryChanged},
                {"cutStateChanged", afterCuts != beforeCuts},
                {"beforeGeometry", EncodeAuthoredEvidence(before)},
                {"afterGeometry", EncodeAuthoredEvidence(after)},
                {"beforeCuts", EncodeCutEvidence(beforeCuts)},
                {"afterCuts",
                 EncodeCutEvidence(
                     afterCuts)}}.dump();
          throw std::runtime_error(std::format(
              "No-append cut changed staged state: source {:08X}, input {}, "
              "returned {}, geometry {}, cuts {}",
              sourceID, index, applied, geometryChanged,
              afterCuts != beforeCuts));
        }
      } else {
        throw std::runtime_error(
            "Connected native cut did not append its undo record");
      }
    }
    // Repair portal connectivity against the newly materialized cut records
    // before the detached graph can be published.
    {
      const auto scope = timing.Measure("CaptureNativeCuts");
      captured = CaptureNativeCuts(borrowed);
    }
    std::size_t records{};
    for (const auto &[id, stack] : captured.records) {
      records += stack.size();
      for (const auto &record : stack)
        for (const auto &link : record.links)
          spdlog::debug("Detached cut undo link: owner {:08X}, {:08X}/{}/{} -> "
                        "{:08X}/{}/{}",
                        id, link.sourceMesh, link.sourceTriangle,
                        link.sourceEdge, link.targetMesh, link.targetTriangle,
                        link.targetEdge);
    }
    if (records != materialized)
      throw std::runtime_error("Connected native cut undo count mismatch");
    cutState.clear();
    for (const auto &[sourceID, draft] : forms)
      cutState.emplace(draft->ID(),
                       CaptureNativeTopology(*draft->form, draft->ID()));
    {
      const auto scope = timing.Measure("RepairCutPortals");
      repaired = RepairCutPortals(expected, cutState, captured.records);
    }
    if (!repaired.Valid())
      throw std::runtime_error("Cut edge repair rejected: " + repaired.error);
    connectivity(repaired.meshes);
    for (const auto &[sourceID, draft] : forms) {
      const auto scope = timing.Measure(
          "cut topology/geometry verification and connected info", sourceID);
      if (CaptureNativeTopology(*draft->form, draft->ID()) !=
          repaired.meshes.at(draft->ID()))
        throw std::runtime_error(
            "Native cut connectivity differs from repaired graph");
      NativeGeometry geometry(*draft->form);
      if (!geometry.Valid())
        throw std::runtime_error(geometry.Error());
      {
        const auto connected =
            timing.Measure("PopulateConnected after cuts", sourceID);
        draft->navigationInfo->PopulateConnected(*draft->form);
      }
    }
  }
  finishResolution();
  expected = std::move(repaired.meshes);
  expectedCuts = std::move(captured);
  for (const auto &[sourceID, draft] : forms) {
    draft->expected = std::make_unique<NativeGeometry>(*draft->form);
    draft->navigationInfo->PrepareBounds(*draft->form);
  }
}
std::string NativeGraphDraft::ValidationError(bool published) const {
  TimingTrace timing{published ? "NativeGraphDraft::ValidationError published"
                               : "NativeGraphDraft::ValidationError staged"};
  if (!NativeAuthoredBasis::Matches(recordLease))
    return "Winning authored records changed since graph preparation";
  for (const auto &cut : cutInputs)
    if (!cut->Matches())
      return "Detached graph cut input changed";
  std::map<std::uint32_t, AuthoredMesh> actual;
  timing.Mark("validate cut input clones");
  for (const auto &[sourceID, draft] : forms) {
    const auto scope =
        timing.Measure("per mesh: geometry/info/door validation", sourceID);
    if (const auto reason = draft->ValidationError(published); !reason.empty())
      return std::format("Native graph mesh {:08X}: {}", sourceID, reason);
    actual.emplace(draft->ID(),
                   CaptureNativeTopology(*draft->form, draft->ID()));
    const auto &doors = expectedDoors.at(draft->ID());
    const auto &closed = expectedClosedDoors.at(draft->ID());
    if (draft->form->doorPortals.size() != doors.size() ||
        draft->form->closedDoors.size() != closed.size())
      return "Detached graph door ownership count changed";
    for (std::uint32_t index = 0; index < doors.size(); ++index)
      if (draft->form->doorPortals[index].door.get() != doors[index].door.get())
        return "Detached graph door ownership changed";
    for (std::uint32_t index = 0; index < closed.size(); ++index)
      if (draft->form->closedDoors[index].door.get() !=
          closed[index].door.get())
        return "Detached graph closed-door ownership changed";
  }
  timing.Mark("mesh validation and topology/door capture");
  if (actual != expected)
    return "Detached native portal/door/cover graph changed";
  const auto audit = AuditAuthoredTopology(actual);
  timing.Mark("authored topology audit");
  if (!audit.Valid() || audit.unloadedTargets)
    return "Detached native graph has unresolved or invalid links";
  if (!expectedCuts.records.empty()) {
    RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> borrowed;
    for (const auto &[sourceID, draft] : forms)
      borrowed.push_back(RE::BSTSmartPointer<RE::BSNavmesh>{
          static_cast<RE::BSNavmesh *>(draft->form.get())});
    if (CaptureNativeCuts(borrowed) != expectedCuts)
      return "Detached connected native cut stacks changed";
  }
  return {};
}
std::vector<RE::FormID> NativeGraphDraft::IDs() const {
  std::vector<RE::FormID> ids;
  for (const auto &[sourceID, form] : forms)
    ids.push_back(form->ID());
  return ids;
}
} // namespace SkyBuild::Navmesh
