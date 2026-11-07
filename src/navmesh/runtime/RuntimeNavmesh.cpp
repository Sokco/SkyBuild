#include "SkyBuildPaths.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/native/NativeDerivedWork.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "building/BuildingManager.h"
#include "navmesh/runtime/CachedGraph.h"
#include "navmesh/runtime/CellCache.h"
#include "navmesh/runtime/AttachmentRestoreSchedule.h"
#include "navmesh/cuts/CutGenerationBasis.h"
#include "navmesh/diagnostics/FailureArchive.h"
#include "navmesh/runtime/GenerationControl.h"
#include "navmesh/runtime/GenerationGameplayPause.h"
#include "navmesh/runtime/GenerationPausePolicy.h"
#include "navmesh/generation/MeshBuilder.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/cuts/NativeCutAudit.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/NativeCutData.h"
#include "navmesh/native/NativeFormDraft.h"
#include "navmesh/publication/NativeOriginalView.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/runtime/NativeStreaming.h"
#include "navmesh/runtime/NativeSynchronization.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include "navmesh/diagnostics/OwnedNavmeshEvidence.h"
#include "navmesh/generation/SurfaceOwnership.h"
#include "navmesh/generation/recast/RecastBuilder.h"
#include "navmesh/generation/recast/SceneCapture.h"
#include "navmesh/generation/TerrainGeometry.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
namespace SkyBuild::Navmesh {
namespace {
std::optional<std::chrono::steady_clock::time_point> generationDrainStarted;
// Explicit generation produces owned geometry before native publication.
// Baselines are copied by a ready-pause callback, before collision capture.
std::atomic_bool generationRequested{};
std::atomic_bool generationPresentation{};
bool generationSucceeded{};
std::shared_ptr<std::atomic_uint> workerProgress;
std::atomic_bool replacementAfterGeneration{};
bool generationEnabled{};
std::recursive_mutex runtimeMutex;
std::optional<RE::FormID> baselineCell;
constexpr float FOOTPRINT_SPACING = 16;
constexpr float AGENT_RADIUS = 16;
// Recast consumes polygons, not sampling rays. Cover a whole exterior cell at
// planning resolution; Recast separately bounds voxel and triangle resources.
constexpr std::size_t MAX_RECAST_FOOTPRINT_QUADS_PER_CELL = 256 * 256;
constexpr std::size_t MAX_GENERATED_CELLS = 16;
struct Tracked {
  RE::ObjectRefHandle handle;
  RE::FormID cellID{};
  RE::NiPoint3 center{};
  float radius{};
};
// Generation baselines are immutable values. Keeping a native mesh here served
// no consumer and could keep cells/retired geometry alive beyond the capture.
using Original = std::shared_ptr<const Baseline>;
struct Capture {
  RE::FormID cellID{};
  BuildInput input;
  AuthoredBasisSnapshot recordLease;
  std::shared_ptr<const std::map<std::uint32_t, AuthoredMesh>> topology;
  std::shared_ptr<const NativeCutSnapshot> cuts;
  std::size_t object{};
  std::shared_ptr<RecastSceneGeometry> recastScene;
  std::vector<Tracked> obstacles;
  std::chrono::steady_clock::time_point started{
      std::chrono::steady_clock::now()};
};
std::map<RE::FormID, Tracked> tracked;
std::map<RE::FormID, std::vector<Original>> originals;
std::map<RE::FormID, BuildResult> generated;
std::map<RE::FormID, std::vector<Baseline>> generatedBases;
std::map<RE::FormID,
         std::shared_ptr<const std::map<std::uint32_t, AuthoredMesh>>>
    baselineTopology;
std::map<RE::FormID, std::shared_ptr<const NativeCutSnapshot>> baselineCuts;
std::map<std::uint32_t, AuthoredMesh> validatedSessionBasis;
// Native ownership stays on the game thread; workers never receive these.
// Explicit scene/reset cleanup releases native ownership while Skyrim is alive.
auto &baselineCutData =
    *new std::map<RE::FormID, std::vector<NativeCutDataSource>>;
std::set<RE::FormID> dirtyCells;
std::map<RE::FormID,
         std::pair<RE::ObjectRefHandle, std::chrono::steady_clock::time_point>>
    pending;
std::optional<Capture> capture;
std::future<BuildResult> future;
AuthoredBasisSnapshot buildingRecordLease;
std::shared_ptr<std::atomic_bool> cancellation;
std::uint64_t revision{};
std::atomic_uint64_t sceneEpoch{};
std::uint64_t workerEpoch{};
RE::FormID buildingCell{};
bool sceneLoaded{};
RuntimeStatus status;
CellCache cellCache;
CellCache pendingBatchCaches;
std::set<RE::FormID> generationCells, remainingGenerationCells;
RE::FormID generationRoot{};
bool expandedGenerationCells{};
std::vector<BuildInput::Bounds> generationFootprints;
bool generationFootprintsPrepared{};
std::vector<Terrain::NavigationCell> generationTerrain;
std::vector<BuildInput::Bounds> generationTerrainFootprints;
std::vector<Baseline> buildingOriginals;
std::vector<PlacementStamp> buildingPlacements;
bool generationCommandActive{};
std::atomic_bool cachedApplyRequested{};
bool cacheOnlyCommand{};
std::set<RE::FormID> refreshBaselines;
bool restoringCommittedCaches{};
std::atomic_bool automaticRestoreSuppressed{};
std::set<RE::FormID> restoreAttempts;
std::set<RE::FormID> deferredAttempts;
std::atomic_bool streamingDetached{};
std::mutex attachmentEventMutex;
std::set<RE::FormID> attachmentRestoreCells;
std::atomic_bool attachmentEventsPending{};
AttachmentRestoreSchedule attachmentRestoreSchedule;
void QueueAttachmentRestore(RE::TESObjectCELL *cell) {
  if (!cell)
    return;
  const auto id = cell->GetFormID();
  const std::scoped_lock lock(attachmentEventMutex);
  attachmentRestoreCells.insert(id);
  attachmentEventsPending.store(true);
}
void QueueAttachedScene() {
  if (const auto world = RE::TES::GetSingleton())
    world->ForEachCell([](auto *cell) { QueueAttachmentRestore(cell); });
}
bool ContributesGenerationGeometry(RE::TESObjectREFR *reference) {
  if (!reference || !reference->GetBaseObject())
    return false;
  // Swinging/sliding doors are traversable connections, not permanent solid
  // geometry. Native cuts still follow their current state after publication.
  // Teleport doors retain their existing generation behavior.
  return !reference->GetBaseObject()->As<RE::TESObjectDOOR>() ||
         reference->extraList.HasType(RE::ExtraDataType::kTeleport);
}
std::vector<PlacementStamp> PlacementState(RE::FormID cellID) {
  std::vector<PlacementStamp> result;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID);
  for (const auto &object : BuildingManager::GetResolvedPlacedObjects()) {
    if (!object || !object->GetParentCell() || !object->GetBaseObject())
      continue;
    if (object->GetParentCell()->GetFormID() != cellID) {
      const auto model = object->Get3D();
      const auto parent = object->GetParentCell();
      const auto coordinates =
          cell && !cell->IsInteriorCell() ? cell->GetCoordinates() : nullptr;
      if (!coordinates || !model || parent->IsInteriorCell() ||
          parent->GetRuntimeData().worldSpace !=
              cell->GetRuntimeData().worldSpace)
        continue;
      const auto &b = model->worldBound;
      const auto radius = b.radius + AGENT_RADIUS;
      const auto x = coordinates->cellX * 4096.0F,
                 y = coordinates->cellY * 4096.0F;
      if (b.center.x + radius <= x || b.center.x - radius >= x + 4096 ||
          b.center.y + radius <= y || b.center.y - radius >= y + 4096)
        continue;
    }
    const auto position = object->GetPosition(), rotation = object->GetAngle();
    result.push_back({object->GetFormID(),
                      object->GetBaseObject()->GetFormID(),
                      {position.x, position.y, position.z, rotation.x,
                       rotation.y, rotation.z, object->GetScale()}});
  }
  std::ranges::sort(result, {}, &PlacementStamp::reference);
  return result;
}
void QueueCurrentCell() {
  generationEnabled = true;
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto cell = player ? player->GetParentCell() : nullptr;
  if (!cell || !cell->IsAttached())
    return;
  // A loaded cell owns one immutable native baseline. Scene reset/cell unload
  // releases it, so the first command after load captures current mod edits.
  // Subsequent commands reuse it, including while a replacement is active.
  if (!originals.contains(cell->GetFormID()))
    refreshBaselines.insert(cell->GetFormID());
  pendingBatchCaches.clear();
  generationCells = remainingGenerationCells = {cell->GetFormID()};
  generationRoot = cell->GetFormID();
  generationTerrain.clear();
  generationTerrainFootprints.clear();
  if (const auto world = cell->GetRuntimeData().worldSpace;
      world && !cell->IsInteriorCell()) {
    auto terrain = Terrain::CaptureNavigation(world->GetFormID());
    const auto xy = cell->GetCoordinates();
    if (xy) {
      const BuildInput::Bounds root{xy->cellX * 4096.F, xy->cellY * 4096.F,
                                    (xy->cellX + 1) * 4096.F,
                                    (xy->cellY + 1) * 4096.F};
      for (auto &entry : terrain) {
        const auto bounds = TerrainFootprints({entry}, AGENT_RADIUS);
        if (entry.cellID == generationRoot ||
            std::ranges::any_of(bounds, [&](const auto &b) {
              return b.right > root.left && b.left < root.right &&
                     b.top > root.bottom && b.bottom < root.top;
            })) {
          generationTerrainFootprints.insert(generationTerrainFootprints.end(),
                                             bounds.begin(), bounds.end());
          generationTerrain.push_back(std::move(entry));
        }
      }
    }
  }
  // Include published volumetric terrain in the collision scene.
  if (!generationTerrain.empty()) {
    spdlog::info("Navmesh generation: using published MC33 geometry from {} "
                 "terrain cells with Recast",
                 generationTerrain.size());
  }
  expandedGenerationCells = false;
  generationFootprints.clear();
  generationFootprintsPrepared = false;
  for (const auto &object : BuildingManager::GetResolvedPlacedObjects())
    if (object && object->GetParentCell() &&
        (object->GetParentCell() == cell ||
         (!cell->IsInteriorCell() &&
          !object->GetParentCell()->IsInteriorCell() &&
          object->GetParentCell()->GetRuntimeData().worldSpace ==
              cell->GetRuntimeData().worldSpace)))
      ReferenceChanged(object.get());
  dirtyCells.insert(cell->GetFormID());
}

// One overwritten diagnostic file, containing only owned geometry. The worker
// performs serialization; no file IO extends the native navigation pause.
void SaveGeneratedSnapshot(
    const BuildInput &input, const BuildResult &output,
    const SurfaceOwnership &ownership,
    const std::map<std::uint32_t, AuthoredMesh> &topology,
    const NativeCutSnapshot &cuts, const std::filesystem::path &path,
    std::string_view stage = "generation",
    const std::set<std::uint32_t> &selectedSources = {},
    const std::map<std::uint32_t, AuthoredMesh> *repairReference = nullptr,
    const AuthoredBasisSnapshot *generationRecords = nullptr) {
  if (!Diagnostics::Enabled())
    return;
  nlohmann::json document{
      {"schema", 2}, {"baselineSpace", "uncut"}, {"revision", output.revision}};
  document["valid"] = output.Valid();
  document["error"] = output.error;
  document["stage"] = stage;
  document["selectedSources"] = selectedSources;
  document["snapshotCoherence"] = "unproven-native-writer-exclusion";
  document["cutPayloadCompleteness"] =
      "decoded-raw-applied-and-observed-applied; "
      "other-historical-payload-omitted";
  if (stage == "cut-basis")
    document["baselineSpace"] = "observed-native-cut";
  if (input.sampledBounds) {
    const auto &b = *input.sampledBounds;
    document["sampledBounds"] = {b.left, b.bottom, b.right, b.top};
  }
  document["footprints"] = nlohmann::json::array();
  for (const auto &b : input.footprints)
    document["footprints"].push_back({b.left, b.bottom, b.right, b.top});
  auto encode = [](const Mesh &mesh) {
    nlohmann::json data;
    for (const auto &vertex : mesh.vertices)
      data["vertices"].push_back({vertex.x, vertex.y, vertex.z});
    for (const auto &triangle : mesh.triangles)
      data["triangles"].push_back(
          {{"vertices", triangle.vertices},
           {"neighbors", triangle.neighbors},
           {"flags", triangle.flags},
           {"sourceMesh", triangle.source.formID},
           {"sourceTriangle", triangle.source.triangle}});
    return data;
  };
  document["authoredTopology"] = EncodeAuthoredEvidence(topology);
  // Preserve the actual pinned generation records, not a later store epoch.
  // Cut-basis diagnostics without a generation lease capture a separate view.
  std::map<std::uint32_t, AuthoredMesh> recordBasis;
  std::set<std::uint32_t> requiredRecords;
  for (const auto &[id, source] : topology)
    requiredRecords.insert(id);
  const auto recordSnapshot =
      generationRecords ? *generationRecords
                        : NativeAuthoredBasis::Snapshot(requiredRecords);
  document["baselineSource"] =
      generationRecords ? "winning-authored-records" : "legacy-live-cut-replay";
  document["recordLoadMissing"] = recordSnapshot.missing;
  document["recordLoadProvenance"] = nlohmann::json::array();
  for (const auto &[id, record] : recordSnapshot.records) {
    recordBasis.emplace(id, record->mesh);
    document["recordLoadProvenance"].push_back(
        {{"id", id},
         {"plugin", record->plugin},
         {"loadSequence", record->loadSequence}});
  }
  document["recordLoadBasis"] = EncodeAuthoredEvidence(recordBasis);
  document["recordLoadVersionsStillCurrent"] =
      NativeAuthoredBasis::Matches(recordSnapshot);
  if (repairReference)
    document["validatedSessionBasis"] =
        EncodeAuthoredEvidence(*repairReference);
  document["generated"] = encode(output.mesh);
  document["generator"] = "recast";
  if (output.sourceGeometry)
    document["recastSource"] = encode(*output.sourceGeometry);
  document["nativeCuts"] = EncodeCutEvidence(cuts);
  if (generationRecords) {
    document["cutReplay"] = {{"usedForGeneration", false}};
  } else {
    const auto restored = ReplayCapturedCutUndo(topology, cuts.records);
    document["cutReplay"] = {
        {"valid", restored.Valid()},
        {"discardedUndoneCuts", restored.discardedUndoneCuts},
        {"error", restored.error},
        {"cuts", restored.restoredCuts},
        {"savedEdges", restored.restoredEdges},
        {"redirectedPortals", restored.redirectedPortals}};
  }
  document["owners"] = ownership.owners;
  document["newFormComponents"] = ownership.newFormComponents;
  for (const auto &original : input.originals)
    if (original)
      document["baselines"].push_back(
          {{"id", original->formID}, {"mesh", encode(original->mesh)}});
  const auto serialized = document.dump();
  if (!output.Valid()) {
    const auto archived = ArchiveFailure(
        path.parent_path() / "SkyBuild-navmesh-failures", serialized);
    spdlog::warn("Archived navmesh failure evidence: {}", archived.string());
  }
  std::ofstream stream(path, std::ios::trunc);
  stream << serialized;
  stream.close();
  if (stream.fail())
    throw std::runtime_error("Could not save generated topology snapshot");
}
void SaveBasisFailure(const std::map<std::uint32_t, AuthoredMesh> &topology,
                      const NativeCutSnapshot &cuts,
                      const std::set<std::uint32_t> &sources,
                      std::string_view error) noexcept {
  try {
    const auto directory = SkyBuild::Paths::DiagnosticsDirectory();
    if (!directory)
      return;
    BuildResult failed;
    failed.revision = revision;
    failed.error = error;
    SaveGeneratedSnapshot({}, failed, {}, topology, cuts,
                          *directory / "SkyBuild-cut-basis-failure.json",
                          "cut-basis", sources, &validatedSessionBasis);
  } catch (const std::exception &failure) {
    spdlog::warn("Cut basis failure capture failed: {}", failure.what());
  }
}
void Invalidate() {
  ++revision;
  if (expandedGenerationCells && generationCommandActive) {
    generationCommandActive = false;
    replacementAfterGeneration.store(false);
    remainingGenerationCells.clear();
    pendingBatchCaches.clear();
  }
  if (cancellation)
    cancellation->store(true);
  if (capture) {
    dirtyCells.insert(capture->cellID);
    capture.reset();
  }
}
bool CaptureOriginals(RE::TESObjectCELL *cell) {
  NativeOriginalView retained;
  const bool published = NativeReplacement::Installed();
  if (published) {
    std::string error;
    if (!NativeReplacement::CopyOriginalView(retained, error)) {
      spdlog::warn("Original publication view unavailable: {}", error);
      return false;
    }
    if (const auto owned = retained.cellMeshes.find(cell->GetFormID());
        owned != retained.cellMeshes.end()) {
      std::vector<Original> sources;
      for (const auto id : owned->second) {
        const auto &authored = retained.topology.at(id);
        auto baseline = std::make_shared<Baseline>();
        baseline->formID = id;
        baseline->mesh.vertices = authored.vertices;
        for (std::size_t index = 0; index < authored.triangles.size();
             ++index) {
          const auto &face = authored.triangles[index];
          Triangle triangle;
          triangle.source = {id, static_cast<std::uint32_t>(index)};
          triangle.flags = face.flags;
          for (unsigned side = 0; side < 3; ++side) {
            triangle.vertices[side] = face.vertices[side];
            triangle.neighbors[side] =
                (face.flags & (1U << side)) || face.edges[side] == 0xFFFF
                    ? -1
                    : face.edges[side];
          }
          baseline->mesh.triangles.push_back(triangle);
        }
        sources.push_back(std::move(baseline));
      }
      baselineTopology[cell->GetFormID()] =
          std::make_shared<std::map<std::uint32_t, AuthoredMesh>>(
              std::move(retained.topology));
      baselineCuts[cell->GetFormID()] =
          std::make_shared<NativeCutSnapshot>(std::move(retained.cuts));
      baselineCutData[cell->GetFormID()] = std::move(retained.cutData);
      originals.insert_or_assign(cell->GetFormID(), std::move(sources));
      refreshBaselines.erase(cell->GetFormID());
      spdlog::info(
          "Recovered retained original generation view for cell {:08X}",
          cell->GetFormID());
      return true;
    }
  }
  std::vector<Original> sources;
  const auto writersBefore = NativeCutAudit::Snapshot();
  if (writersBefore.available && writersBefore.observation.active)
    throw std::runtime_error(
        "Native original capture rejected: cut workers are active");
  if (cell->GetRuntimeData().navMeshes) {
    for (const auto &native : cell->GetRuntimeData().navMeshes->navMeshes) {
      if (!native)
        continue;
      if (NativeReplacement::TracksMesh(native.get())) {
        spdlog::error("Refusing to capture runtime navmesh {:08X} as an "
                      "original baseline.",
                      native->GetFormID());
        return false;
      }
      auto snapshot = std::make_shared<Baseline>();
      snapshot->formID = native->GetFormID();
      if (native->vertices.size() > 65535 || native->triangles.size() > 65535)
        return false;
      for (const auto &vertex : native->vertices)
        snapshot->mesh.vertices.push_back(
            {vertex.location.x, vertex.location.y, vertex.location.z});
      std::uint32_t sourceIndex{};
      for (const auto &triangle : native->triangles) {
        Triangle copied;
        copied.source = {native->GetFormID(), sourceIndex++};
        for (int i = 0; i < 3; ++i)
          copied.vertices[i] = triangle.vertices[i];
        // Preserve authored flags in the immutable baseline. Rebuilt local
        // topology clears external-edge flags; native portal stitching is
        // pending.
        std::memcpy(&copied.flags, &triangle.triangleFlags,
                    sizeof(copied.flags));
        snapshot->mesh.triangles.push_back(copied);
      }
      sources.push_back(std::move(snapshot));
    }
  }
  const auto pathing = RE::Pathing::GetSingleton();
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> loaded;
  if (!pathing || !pathing->GetAllLoadedNavmeshes5(loaded) ||
      loaded.size() > 1024)
    return false;
  auto graph = std::make_shared<std::map<std::uint32_t, AuthoredMesh>>(
      retained.topology);
  std::size_t triangleBudget{}, vertexBudget{};
  for (const auto &[id, original] : retained.topology) {
    triangleBudget += original.triangles.size();
    vertexBudget += original.vertices.size();
  }
  if (triangleBudget > 4000000 || vertexBudget > 4000000)
    return false;
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> eligible;
  for (const auto &mesh : loaded) {
    if (!mesh)
      continue;
    const auto fullMesh = skyrim_cast<RE::NavMesh *>(mesh.get());
    if ((fullMesh && NativeReplacement::TracksMesh(fullMesh)) ||
        retained.topology.contains(mesh->QNavmeshID()))
      continue;
    const auto full = skyrim_cast<RE::NavMesh *>(mesh.get());
    const auto owner = full ? full->GetSaveParentCell() : nullptr;
    if (!owner || !owner->IsAttached())
      continue;
    eligible.push_back(mesh);
    triangleBudget += mesh->triangles.size();
    vertexBudget += mesh->vertices.size();
    if (triangleBudget > 4000000 || vertexBudget > 4000000)
      return false;
    auto copied = CaptureNativeTopology(*mesh, mesh->QNavmeshID());
    if (!graph->emplace(copied.formID, std::move(copied)).second)
      return false;
  }
  for (const auto &source : sources)
    if (!graph->contains(source->formID))
      return false;
  auto cuts = std::make_shared<NativeCutSnapshot>(CaptureNativeCuts(eligible));
  auto cutData = CaptureNativeCutData(eligible, cuts.get());
  // Detect changed observations; this is not proof of native writer exclusion.
  for (const auto &mesh : eligible)
    if (CaptureNativeTopology(*mesh, mesh->QNavmeshID()) !=
        graph->at(mesh->QNavmeshID()))
      throw std::runtime_error(
          "Native geometry changed during original capture");
  if (CaptureNativeCuts(eligible) != *cuts)
    throw std::runtime_error(
        "Native cut payload or ownership changed during original capture");
  const auto writersAfter = NativeCutAudit::Snapshot();
  if (writersBefore.available &&
      WriterObserver::Changed(writersBefore.observation,
                              writersAfter.observation))
    throw std::runtime_error(
        "Native cut worker crossed original capture observations");
  spdlog::info(
      "Native original observation: cut audit available {}, epoch {} -> {}, "
      "active {} -> {}, {} total cut task calls. Writer exclusion unproven.",
      writersAfter.available, writersBefore.observation.epoch,
      writersAfter.observation.epoch, writersBefore.observation.active,
      writersAfter.observation.active, writersAfter.observation.calls);
  spdlog::info("{}", NativeAuthoredBasis::Status());
  cuts->records.insert(retained.cuts.records.begin(),
                       retained.cuts.records.end());
  cuts->auxiliaryIndices.insert(retained.cuts.auxiliaryIndices.begin(),
                                retained.cuts.auxiliaryIndices.end());
  cuts->obstacleEntries.insert(retained.cuts.obstacleEntries.begin(),
                               retained.cuts.obstacleEntries.end());
  cuts->ownership.insert(retained.cuts.ownership.begin(),
                         retained.cuts.ownership.end());
  cutData.insert(cutData.end(), retained.cutData.begin(),
                 retained.cutData.end());
  baselineTopology[cell->GetFormID()] = std::move(graph);
  baselineCuts[cell->GetFormID()] = std::move(cuts);
  baselineCutData[cell->GetFormID()] = std::move(cutData);
  originals.insert_or_assign(cell->GetFormID(), std::move(sources));
  refreshBaselines.erase(cell->GetFormID());
  return true;
}
bool BeginCapture(RE::FormID cellID) {
  auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID);
  if (!cell || !cell->IsAttached())
    return false;
  if (!originals.contains(cellID)) {
    return false;
  }
  Capture job;
  job.cellID = cellID;
  job.input.revision = revision;
  if (!cell->IsInteriorCell()) {
    const auto coordinates = cell->GetCoordinates();
    if (!coordinates)
      return false;
    constexpr float cellSize = 4096;
    const float x = coordinates->cellX * cellSize;
    const float y = coordinates->cellY * cellSize;
    job.input.sampledBounds =
        BuildInput::Bounds{x, y, x + cellSize, y + cellSize};
  }
  job.topology = baselineTopology.at(cellID);
  job.cuts = baselineCuts.at(cellID);
  std::set<std::uint32_t> sourceIDs;
  for (const auto &source : originals.at(cellID))
    sourceIDs.insert(source->formID);
  std::set<std::uint32_t> requiredRecords;
  for (const auto &[id, source] : *job.topology)
    requiredRecords.insert(id);
  const auto basis = NativeAuthoredBasis::PrepareGeneration(
      requiredRecords, sourceIDs, job.recordLease, *job.topology);
  if (!basis.Valid()) {
    status.stage = RuntimeStage::kFailed;
    status.message = basis.error;
    spdlog::warn("Navmesh generation basis rejected: {}", basis.error);
    SaveBasisFailure(*job.topology, *job.cuts, sourceIDs, basis.error);
    return false;
  }
  for (const auto &[id, baseline] : basis.baselines)
    job.input.originals.push_back(baseline);
  {
    job.recastScene = std::make_shared<RecastSceneGeometry>();
    job.recastScene->terrain = generationTerrain;
    for (const auto &[id, source] : basis.topology) {
      if (!sourceIDs.contains(id)) {
        const auto native = RE::TESForm::LookupByID<RE::NavMesh>(id);
        const auto owner = native ? native->GetSaveParentCell() : nullptr;
        if (!owner || !owner->IsAttached() || cell->IsInteriorCell() ||
            owner->IsInteriorCell() ||
            owner->GetRuntimeData().worldSpace !=
                cell->GetRuntimeData().worldSpace)
          continue;
      }
      auto ground = std::make_shared<Baseline>();
      ground->formID = id;
      ground->mesh.vertices = source.vertices;
      for (const auto &face : source.triangles)
        ground->mesh.triangles.push_back(
            {{face.vertices[0], face.vertices[1], face.vertices[2]},
             {-1, -1, -1},
             face.flags,
             {}});
      job.recastScene->ground.push_back(std::move(ground));
    }
    job.input.sceneGeometry = job.recastScene;
  }
  const auto cached = cellCache.find(cellID);
  const auto placements = PlacementState(cellID);
  if (cacheOnlyCommand && cached != cellCache.end() &&
      CacheOriginalsMatch(cached->second, job.input.originals,
                          basis.doorFaces)) {
    BuildResult result;
    result.revision = revision;
    result.mesh = cached->second.modified;
    status.vertices = result.mesh.vertices.size();
    status.triangles = result.mesh.triangles.size();
    status.stage = RuntimeStage::kGenerated;
    generationCommandActive = false;
    status.sampled = status.total = 0;
    status.message = std::format("Loaded cached navigation for cell {:08X}; "
                                 "preparing native publication.",
                                 cellID);
    generated.insert_or_assign(cellID, std::move(result));
    generatedBases.insert_or_assign(cellID, cached->second.originals);
    if (replacementAfterGeneration.exchange(false))
      NativeSynchronization::RequestReplacement();
    spdlog::info("{} Collision sampling and geometry generation skipped.",
                 status.message);
    return false;
  }
  if (cacheOnlyCommand) {
    generationCommandActive = false;
    replacementAfterGeneration.store(false);
    status.stage = RuntimeStage::kIdle;
    status.message =
        "No matching cached navigation. Press Generate Navmesh to rebuild.";
    return false;
  }
  if (!generationFootprintsPrepared) {
    generationFootprintsPrepared = true;
    generationFootprints = generationTerrainFootprints;
    for (const auto &[id, reference] : tracked) {
      const auto object = reference.handle.get();
      const auto parent = object ? object->GetParentCell() : nullptr;
      if (!parent || !object->Get3D() ||
          !ContributesGenerationGeometry(object.get()))
        continue;
      const float radius = reference.radius + AGENT_RADIUS;
      bool affects = false;
      for (const auto targetID : generationCells) {
        const auto target =
            RE::TESForm::LookupByID<RE::TESObjectCELL>(targetID);
        if (!target || !target->IsAttached())
          continue;
        if (target->IsInteriorCell()) {
          affects |= reference.cellID == targetID;
          continue;
        }
        if (parent->IsInteriorCell() || parent->GetRuntimeData().worldSpace !=
                                            target->GetRuntimeData().worldSpace)
          continue;
        const auto position = target->GetCoordinates();
        if (!position)
          continue;
        const auto x = position->cellX * 4096.0F, y = position->cellY * 4096.0F;
        affects |= reference.center.x + radius > x &&
                   reference.center.x - radius < x + 4096 &&
                   reference.center.y + radius > y &&
                   reference.center.y - radius < y + 4096;
      }
      if (affects)
        generationFootprints.push_back(
            {reference.center.x - radius, reference.center.y - radius,
             reference.center.x + radius, reference.center.y + radius});
    }
  }
  for (const auto &[id, reference] : tracked) {
    const auto object = reference.handle.get();
    if (!object || !object->Get3D() ||
        !ContributesGenerationGeometry(object.get()))
      continue;
    if (reference.cellID != cellID) {
      const auto parent = object->GetParentCell();
      if (!job.input.sampledBounds || !parent || parent->IsInteriorCell() ||
          parent->GetRuntimeData().worldSpace !=
              cell->GetRuntimeData().worldSpace)
        continue;
      const auto &b = *job.input.sampledBounds;
      const auto radius = reference.radius + AGENT_RADIUS;
      if (reference.center.x + radius <= b.left ||
          reference.center.x - radius >= b.right ||
          reference.center.y + radius <= b.bottom ||
          reference.center.y - radius >= b.top)
        continue;
    }
    job.obstacles.push_back(reference);
  }
  // Empty object bounds leave the original mesh unchanged on an explicit
  // command.
  auto plan =
      PlanFootprints(generationFootprints, job.input.sampledBounds,
                     FOOTPRINT_SPACING, MAX_RECAST_FOOTPRINT_QUADS_PER_CELL);
  if (!plan.Valid()) {
    status.stage = RuntimeStage::kFailed;
    status.message = plan.error;
    spdlog::warn("Navmesh footprint planning rejected cell {:08X}: {}", cellID,
                 plan.error);
    return false;
  }
  job.input.footprints = std::move(plan.regions);
  status.stage = RuntimeStage::kCapturing;
  status.sampled = 0;
  status.total = job.obstacles.size();
  status.message = "Collecting placed-object collision geometry for Recast.";
  capture = std::move(job);
  return true;
}
bool ExpandGenerationCells() {
  if (expandedGenerationCells || cacheOnlyCommand)
    return true;
  const auto root = RE::TESForm::LookupByID<RE::TESObjectCELL>(generationRoot);
  if (!root || !originals.contains(generationRoot))
    return false;
  expandedGenerationCells = true;
  if (root->IsInteriorCell())
    return true;
  const auto coordinates = root->GetCoordinates();
  if (!coordinates)
    return false;
  const BuildInput::Bounds rootBounds{
      coordinates->cellX * 4096.0F, coordinates->cellY * 4096.0F,
      (coordinates->cellX + 1) * 4096.0F, (coordinates->cellY + 1) * 4096.0F};
  const auto overlaps = [](const Tracked &object, const BuildInput::Bounds &b) {
    const auto r = object.radius + AGENT_RADIUS;
    return object.center.x + r > b.left && object.center.x - r < b.right &&
           object.center.y + r > b.bottom && object.center.y - r < b.top;
  };
  std::vector<Tracked> affecting;
  for (const auto &[id, reference] : tracked) {
    const auto object = reference.handle.get();
    const auto parent = object ? object->GetParentCell() : nullptr;
    if (parent && ContributesGenerationGeometry(object.get()) &&
        !parent->IsInteriorCell() &&
        parent->GetRuntimeData().worldSpace ==
            root->GetRuntimeData().worldSpace &&
        overlaps(reference, rootBounds))
      affecting.push_back(reference);
  }
  const auto graph = baselineTopology.at(generationRoot);
  std::map<RE::FormID, std::vector<Original>> candidates;
  for (const auto &[id, mesh] : *graph) {
    const auto form = RE::TESForm::LookupByID<RE::NavMesh>(id);
    const auto parent = form ? form->GetSaveParentCell() : nullptr;
    if (!parent || !parent->IsAttached() || parent->IsInteriorCell() ||
        parent->GetRuntimeData().worldSpace !=
            root->GetRuntimeData().worldSpace)
      continue;
    const auto position = parent->GetCoordinates();
    if (!position)
      continue;
    const BuildInput::Bounds bounds{
        position->cellX * 4096.0F, position->cellY * 4096.0F,
        (position->cellX + 1) * 4096.0F, (position->cellY + 1) * 4096.0F};
    const bool committedNeighbor = cellCache.contains(generationRoot) &&
                                   cellCache.contains(parent->GetFormID()) &&
                                   (((bounds.right == rootBounds.left ||
                                      bounds.left == rootBounds.right) &&
                                     bounds.bottom == rootBounds.bottom) ||
                                    ((bounds.top == rootBounds.bottom ||
                                      bounds.bottom == rootBounds.top) &&
                                     bounds.left == rootBounds.left));
    const bool terrainNeighbor =
        std::ranges::any_of(generationTerrainFootprints, [&](const auto &b) {
          return b.right > bounds.left && b.left < bounds.right &&
                 b.top > bounds.bottom && b.bottom < bounds.top;
        });
    if (!committedNeighbor && !terrainNeighbor &&
        !std::ranges::any_of(affecting, [&](const auto &object) {
          return overlaps(object, bounds);
        }))
      continue;
    auto baseline = std::make_shared<Baseline>();
    baseline->formID = id;
    baseline->mesh.vertices = mesh.vertices;
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
      const auto &face = mesh.triangles[index];
      Triangle triangle;
      triangle.source = {id, static_cast<std::uint32_t>(index)};
      triangle.flags = face.flags;
      for (unsigned side = 0; side < 3; ++side) {
        triangle.vertices[side] = face.vertices[side];
        triangle.neighbors[side] =
            (face.flags & (1U << side)) || face.edges[side] == 0xFFFF
                ? -1
                : face.edges[side];
      }
      baseline->mesh.triangles.push_back(triangle);
    }
    candidates[parent->GetFormID()].push_back(std::move(baseline));
  }
  if (candidates.size() > MAX_GENERATED_CELLS)
    return false;
  for (auto &[id, sources] : candidates) {
    if (!originals.contains(id)) {
      originals[id] = std::move(sources);
      baselineTopology[id] = graph;
      baselineCuts[id] = baselineCuts.at(generationRoot);
      baselineCutData[id] = baselineCutData.at(generationRoot);
    }
    generationCells.insert(id);
    remainingGenerationCells.insert(id);
  }
  spdlog::info("Explicit navmesh generation covers {} affected cells",
               generationCells.size());
  return true;
}
class AttachmentSink final
    : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>,
      public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESCellFullyLoadedEvent *event,
               RE::BSTEventSource<RE::TESCellFullyLoadedEvent> *) override {
    if (event)
      QueueAttachmentRestore(event->cell);
    return RE::BSEventNotifyControl::kContinue;
  }
  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESCellAttachDetachEvent *event,
               RE::BSTEventSource<RE::TESCellAttachDetachEvent> *) override {
    // The event retains its reference. Copy only its parent cell ID; never
    // retain native pointers beyond this callback or acquire runtimeMutex.
    if (event && event->reference)
      QueueAttachmentRestore(event->reference->GetParentCell());
    return RE::BSEventNotifyControl::kContinue;
  }
};
class LoadedSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESObjectLoadedEvent *event,
               RE::BSTEventSource<RE::TESObjectLoadedEvent> *) override {
    if (event) {
      const auto id = event->formID;
      const bool loaded = event->loaded;
      const auto epoch = sceneEpoch.load();
      if (const auto tasks = SKSE::GetTaskInterface())
        tasks->AddTask([id, loaded, epoch] {
          if (!sceneLoaded || sceneEpoch.load() != epoch ||
              !BuildingManager::IsPlacedObject(id))
            return;
          if (loaded)
            ReferenceChanged(RE::TESForm::LookupByID<RE::TESObjectREFR>(id));
          else
            ReferenceRemoved(id);
        });
    }
    return RE::BSEventNotifyControl::kContinue;
  }
};
} // namespace
void RequestGeneration() {
  const std::scoped_lock lock(runtimeMutex);
  if (GenerationGameplayPause::Owned() || generationCommandActive || capture ||
      future.valid() || baselineCell || NativeFormPreparation::Pending() ||
      NativeSynchronization::PendingAction())
    return;
  automaticRestoreSuppressed.store(false);
  cachedApplyRequested.store(false);
  replacementAfterGeneration.store(true);
  generationPresentation = true;
  generationSucceeded = false;
  workerProgress.reset();
  status.stage = RuntimeStage::kIdle;
  status.sampled = status.total = 0;
  generationRequested.store(true);
}
void RequestCachedApply() {
  const std::scoped_lock lock(runtimeMutex);
  if (GenerationGameplayPause::Owned() || generationCommandActive || capture ||
      future.valid() || baselineCell || NativeFormPreparation::Pending() ||
      NativeSynchronization::PendingAction())
    return;
  automaticRestoreSuppressed.store(false);
  cachedApplyRequested.store(true);
  replacementAfterGeneration.store(true);
  generationRequested.store(true);
}
void SuppressCommittedRestoration() { automaticRestoreSuppressed.store(true); }
bool CopyGeneratedForPreparation(
    Mesh &mesh, SurfaceOwnership &ownership,
    std::map<std::uint32_t, AuthoredMesh> *topology, NativeCutSnapshot *cuts,
    std::vector<NativeCutDataSource> *cutData,
    std::vector<RE::FormID> *islandCells, std::set<RE::FormID> *replacedCells) {
  const std::scoped_lock lock(runtimeMutex);
  const bool committed = restoringCommittedCaches;
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto cell = player ? player->GetParentCell() : nullptr;
  if (!sceneLoaded || !cell || !cell->IsAttached() || future.valid() ||
      capture || baselineCell || (!committed && !pending.empty())) {
    status.message = "Native preparation is waiting for an attached cell and "
                     "completed capture/worker state.";
    return false;
  }
  // Native preparation runs after opening the original view in a ready pause.
  // Recapture current portals/cuts, including newly streamed neighboring cells.
  {
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
    const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
    const RE::BSWriteLockGuard publicationGuard(*publication.get());
    const RE::BSReadLockGuard listingGuard(*listing.get());
    if (!CaptureOriginals(cell)) {
      status.message = "Fresh original navigation capture rejected its mesh "
                       "identities or ownership.";
      return false;
    }
  }
  const auto result = generated.find(cell->GetFormID());
  const auto sources = originals.find(cell->GetFormID());
  if (sources == originals.end() ||
      (!committed && (result == generated.end() || !result->second.Valid() ||
                      result->second.revision != revision))) {
    status.message = "Native preparation has no current original or generated "
                     "geometry for this cell.";
    return false;
  }
  if (!baselineTopology.contains(cell->GetFormID()) ||
      !baselineCuts.contains(cell->GetFormID())) {
    status.message =
        "Fresh original navigation topology or cut snapshot is unavailable.";
    return false;
  }
  if (topology)
    *topology = *baselineTopology.at(cell->GetFormID());
  if (cuts)
    *cuts = *baselineCuts.at(cell->GetFormID());
  if (cutData)
    *cutData = baselineCutData.at(cell->GetFormID());
  std::map<std::uint32_t, std::size_t> counts;
  std::set<std::uint32_t> sourceIDs;
  std::map<RE::FormID, std::set<RE::FormID>> cellSources;
  for (const auto &[id, authored] : *baselineTopology.at(cell->GetFormID())) {
    const auto form = RE::TESForm::LookupByID<RE::NavMesh>(id);
    const auto parent = form ? form->GetSaveParentCell() : nullptr;
    if (parent && parent->IsAttached()) {
      cellSources[parent->GetFormID()].insert(id);
      sourceIDs.insert(id);
    }
  }
  std::set<std::uint32_t> requiredRecords;
  for (const auto &[id, source] : *baselineTopology.at(cell->GetFormID()))
    requiredRecords.insert(id);
  AuthoredBasisSnapshot recordLease;
  const auto basis = NativeAuthoredBasis::PrepareGeneration(
      requiredRecords, sourceIDs, recordLease,
      *baselineTopology.at(cell->GetFormID()));
  if (!basis.Valid()) {
    status.message =
        "Winning authored basis preparation failed: " + basis.error;
    SaveBasisFailure(*baselineTopology.at(cell->GetFormID()),
                     *baselineCuts.at(cell->GetFormID()), sourceIDs,
                     basis.error);
    return false;
  }
  for (const auto &[id, authored] : basis.topology)
    // Keep the first verified original as the scene's repair reference.
    // A later streamed capture must not redefine missing cut-edge metadata
    // as authored topology. Reset clears this before a different save loads.
    validatedSessionBasis.try_emplace(id, authored);
  for (const auto &[id, baseline] : basis.baselines)
    counts.emplace(id, baseline->mesh.triangles.size());
  if (!committed) {
    for (const auto generatedCell : generationCells) {
      const auto savedBasis = generatedBases.find(generatedCell);
      const auto generatedSources = cellSources.find(generatedCell);
      if (savedBasis == generatedBases.end() ||
          generatedSources == cellSources.end())
        return false;
      std::vector<std::shared_ptr<const Baseline>> fresh;
      for (const auto id : generatedSources->second)
        fresh.push_back(basis.baselines.at(id));
      CachedCell validation;
      validation.originals = savedBasis->second;
      if (!CacheOriginalsMatch(validation, fresh, basis.doorFaces)) {
        status.message = "Original navigation changed while generating; "
                         "Generate Navmesh again.";
        return false;
      }
    }
  }
  std::map<RE::FormID, Mesh> meshes;
  for (const auto &[id, cached] : cellCache) {
    if (!committed && generationCells.contains(id))
      continue;
    const auto source = cellSources.find(id);
    if (source == cellSources.end())
      continue;
    std::vector<std::shared_ptr<const Baseline>> fresh;
    for (const auto formID : source->second) {
      const auto found = basis.baselines.find(formID);
      if (found == basis.baselines.end())
        return false;
      fresh.push_back(found->second);
    }
    const auto mismatch =
        CacheOriginalsMismatch(cached, fresh, basis.doorFaces);
    if (mismatch.empty())
      meshes.emplace(id, cached.modified);
    else {
      restoreAttempts.insert(id);
      spdlog::warn("Cached navmesh {:08X} has an incompatible original basis; "
                   "native navigation retained. Explicit Generate required: {}",
                   id, mismatch);
    }
  }
  if (!committed)
    for (const auto id : generationCells) {
      const auto found = generated.find(id);
      if (found == generated.end())
        return false;
      meshes.insert_or_assign(id, found->second.mesh);
    }
  if (meshes.empty()) {
    status.message =
        "No compatible committed navigation; Generate Navmesh to rebuild.";
    return false;
  }
  std::map<RE::FormID, BuildInput::Bounds> domains;
  for (const auto &[id, geometry] : meshes) {
    const auto parent = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (!parent || parent->IsInteriorCell() ||
        parent->GetRuntimeData().worldSpace !=
            cell->GetRuntimeData().worldSpace)
      continue;
    const auto coordinates = parent->GetCoordinates();
    if (coordinates)
      domains[id] = {coordinates->cellX * 4096.0F, coordinates->cellY * 4096.0F,
                     (coordinates->cellX + 1) * 4096.0F,
                     (coordinates->cellY + 1) * 4096.0F};
  }
  auto batch = AssembleCachedGraph(meshes, counts, domains);
  if (!batch.Valid()) {
    status.message = batch.ownership.error;
    return false;
  }
  mesh = std::move(batch.mesh);
  ownership = std::move(batch.ownership);
  if (islandCells)
    *islandCells = std::move(batch.islandCells);
  if (replacedCells)
    *replacedCells = batch.cells;
  spdlog::info(
      "Prepared navigation batch for {} cached/generated cells; "
      "saved cells require no collision sampling or triangle generation.",
      batch.cells.size());
  return true;
}
bool RestoreCommittedNavigation(void *) noexcept {
  const std::scoped_lock lock(runtimeMutex);
  restoringCommittedCaches = true;
  const bool result = NativeReplacement::Install(nullptr);
  restoringCommittedCaches = false;
  status.installedInEngine = NativeReplacement::Installed();
  status.message = NativeFormPreparation::Pending()
                       ? "Preparing committed navigation asynchronously."
                   : result
                       ? "Restored committed navigation without regeneration."
                       : "Committed navigation could not be restored: " +
                             NativeReplacement::Status();
  spdlog::info("{}", status.message);
  return result;
}
bool CapturePendingBaseline(void *) noexcept {
  const std::scoped_lock lock(runtimeMutex);
  if (!baselineCell)
    return false;
  const auto id = *baselineCell;
  baselineCell.reset();
  try {
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (!sceneLoaded || !generationEnabled || !cell || !cell->IsAttached())
      throw std::runtime_error("Baseline cell is no longer attached");
    const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
    const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
    const RE::BSWriteLockGuard publicationGuard(*publication.get());
    const RE::BSReadLockGuard listingGuard(*listing.get());
    if (!CaptureOriginals(cell))
      throw std::runtime_error("Baseline exceeds native triangle index limits");
    status.message = std::format("Captured {} native meshes for cell {:08X}.",
                                 originals.at(id).size(), id);
    spdlog::info("{} Detached generation only; no mesh writes.",
                 status.message);
    return true;
  } catch (const std::exception &error) {
    dirtyCells.erase(id);
    generationCommandActive = false;
    status.stage = RuntimeStage::kFailed;
    status.message = error.what();
    spdlog::warn("Native baseline capture failed: {}", error.what());
    return false;
  } catch (...) {
    dirtyCells.erase(id);
    generationCommandActive = false;
    status.stage = RuntimeStage::kFailed;
    status.message = "Native baseline capture failed.";
    return false;
  }
}
void Initialize() {
  GenerationGameplayPause::Initialize();
  NativeSynchronization::Initialize();
  if (!NativeStreaming::Initialize())
    spdlog::warn("Pre-detach cell lifecycle audit is unavailable.");
  static LoadedSink sink;
  static AttachmentSink attachmentSink;
  static bool initialized{};
  if (initialized)
    return;
  initialized = true;
  if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
    events->AddEventSink(&sink);
    events->AddEventSink<RE::TESCellFullyLoadedEvent>(&attachmentSink);
    events->AddEventSink<RE::TESCellAttachDetachEvent>(&attachmentSink);
  }
  spdlog::info(
      "Navmesh generation pipeline initialized. Native installation "
      "supports repeated generation and per-save cell geometry caching.");
}
void ReferenceChanged(RE::TESObjectREFR *reference) {
  const std::scoped_lock lock(runtimeMutex);
  if (!generationEnabled || !sceneLoaded || !reference)
    return;
  const auto id = reference->GetFormID();
  Invalidate();
  if (const auto found = tracked.find(id); found != tracked.end())
    dirtyCells.insert(found->second.cellID);
  pending.insert_or_assign(
      id, std::pair{reference->GetHandle(), std::chrono::steady_clock::now() +
                                                std::chrono::seconds(5)});
}
void ReferenceRemoved(RE::FormID id) {
  const std::scoped_lock lock(runtimeMutex);
  pending.erase(id);
  if (const auto found = tracked.find(id); found != tracked.end()) {
    Invalidate();
    dirtyCells.insert(found->second.cellID);
    tracked.erase(found);
  }
}
void Reset() {
  const std::scoped_lock lock(runtimeMutex);
  GenerationGameplayPause::End();
  NativeSynchronization::SetSceneLoaded(false);
  Invalidate();
  ++sceneEpoch;
  sceneLoaded = false;
  tracked.clear();
  pending.clear();
  dirtyCells.clear();
  originals.clear();
  baselineTopology.clear();
  baselineCuts.clear();
  validatedSessionBasis.clear();
  baselineCutData.clear();
  generated.clear();
  generatedBases.clear();
  buildingRecordLease = {};
  cellCache.clear();
  buildingOriginals.clear();
  pendingBatchCaches.clear();
  generationCells.clear();
  remainingGenerationCells.clear();
  generationRoot = 0;
  generationTerrain.clear();
  generationTerrainFootprints.clear();
  expandedGenerationCells = false;
  generationFootprints.clear();
  generationFootprintsPrepared = false;
  buildingPlacements.clear();
  refreshBaselines.clear();
  generationCommandActive = false;
  cachedApplyRequested.store(false);
  cacheOnlyCommand = false;
  restoringCommittedCaches = false;
  automaticRestoreSuppressed.store(false);
  restoreAttempts.clear();
  deferredAttempts.clear();
  {
    const std::scoped_lock events(attachmentEventMutex);
    attachmentRestoreCells.clear();
    attachmentEventsPending.store(false);
  }
  attachmentRestoreSchedule = {};
  baselineCell.reset();
  generationEnabled = false;
  generationPresentation = false;
  generationSucceeded = false;
  workerProgress.reset();
  generationRequested.store(false);
  replacementAfterGeneration.store(false);
  status = {};
}
void SetSceneLoaded(bool loaded) {
  const std::scoped_lock lock(runtimeMutex);
  if (!loaded) {
    Reset();
    return;
  }
  sceneLoaded = true;
  QueueAttachedScene();
  generationEnabled = false;
  NativeSynchronization::SetSceneLoaded(true);
  for (const auto &object : BuildingManager::GetResolvedPlacedObjects())
    if (object && object->GetParentCell() &&
        object->GetParentCell()->IsAttached())
      ReferenceChanged(object.get());
}
std::map<std::uint32_t, AuthoredMesh> CopyValidatedSessionBasis() {
  const std::scoped_lock lock(runtimeMutex);
  return validatedSessionBasis;
}
void NotifyStreamingDetach() { streamingDetached.store(true); }
void AbortGeneration(std::string_view reason) {
  const std::scoped_lock lock(runtimeMutex);
  if (cancellation)
    cancellation->store(true);
  ++revision; // Reject this worker result; the loaded scene and caches stay
              // valid.
  generationCommandActive = false;
  generationPresentation = false;
  generationRequested.store(false);
  replacementAfterGeneration.store(false);
  cachedApplyRequested.store(false);
  capture.reset();
  baselineCell.reset();
  remainingGenerationCells.clear();
  pendingBatchCaches.clear();
  NativeSynchronization::CancelPending();
  GenerationGameplayPause::End();
  status.stage = RuntimeStage::kFailed;
  status.message = "Navigation generation aborted: " + std::string(reason);
  spdlog::error("{}", status.message);
}
void Update() {
  const std::scoped_lock lock(runtimeMutex);
  const auto finishPause = [&] {
    const GenerationWork work{generationCommandActive,
                              capture.has_value(),
                              baselineCell.has_value(),
                              future.valid(),
                              NativeFormPreparation::Pending(),
                              NativeSynchronization::PendingAction(),
                              generationRequested.load()};
    if (ReleaseGenerationPause(GenerationGameplayPause::Owned(), sceneLoaded,
                               work)) {
      GenerationGameplayPause::End();
      generationPresentation = false;
    }
  };
  struct FinishPause {
    const decltype(finishPause) &finish;
    ~FinishPause() { finish(); }
  } finish{finishPause};
  // Detach hooks may run outside this scheduler. Preserve committed meshes,
  // but permit a fresh original capture and cache publication on re-entry.
  if (streamingDetached.exchange(false)) {
    restoreAttempts.clear();
    deferredAttempts.clear();
    QueueAttachedScene();
  }
  if (attachmentEventsPending.exchange(false)) {
    std::set<RE::FormID> attached;
    {
      const std::scoped_lock events(attachmentEventMutex);
      attached.swap(attachmentRestoreCells);
    }
    for (const auto id : attached)
      attachmentRestoreSchedule.Wake(id);
  }
  // A cancelled worker from the preceding save must not block cache restore
  // just because the generator is inactive in the new scene.
  if (future.valid() && workerEpoch != sceneEpoch &&
      future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      (void)future.get();
    } catch (const std::exception &error) {
      spdlog::debug("Discarded preceding-scene navmesh worker: {}",
                    error.what());
    }
    cancellation.reset();
  }
  if (!generationRequested.load())
    generationDrainStarted.reset();
  if (generationRequested.load() && sceneLoaded) {
    const auto ui = RE::UI::GetSingleton();
    // Wait for the invoking settings menu to close. Cached installation never
    // acquires this gameplay pause, including automatic load/stream
    // restoration.
    if (!cachedApplyRequested.load() && ui && ui->GameIsPaused())
      return;
    // Drain existing cuts before the gameplay pause stops ordinary updates.
    // Keep this request queued; do not resample or start a partial transaction.
    if (!NativeDerivedWork::QueuesIdle()) {
      const auto now = std::chrono::steady_clock::now();
      if (!generationDrainStarted) {
        generationDrainStarted = now;
        status.message = "Waiting for Skyrim's navigation work to finish.";
      }
      if (now - *generationDrainStarted >= std::chrono::seconds(10)) {
        generationRequested.store(false);
        cachedApplyRequested.store(false);
        replacementAfterGeneration.store(false);
        generationPresentation = false;
        generationDrainStarted.reset();
        status.stage = RuntimeStage::kFailed;
        status.message =
            "Timed out waiting for Skyrim's navigation work to finish.";
      }
      return;
    }
    if (Terrain::NavigationBusy()) {
      status.message = "Waiting for edited terrain geometry to finish.";
      return;
    }
    generationDrainStarted.reset();
    generationRequested.store(false);
    spdlog::info("Explicit navmesh generation: Recast");
    generationCommandActive = true;
    cacheOnlyCommand = cachedApplyRequested.exchange(false);
    if (RequiresGameplayPause(cacheOnlyCommand ? NavigationCommand::ApplyCache
                                               : NavigationCommand::Generate) &&
        !GenerationGameplayPause::Begin()) {
      generationCommandActive = false;
      replacementAfterGeneration.store(false);
      status.stage = RuntimeStage::kFailed;
      generationPresentation = false;
      status.message = "Could not pause gameplay for navigation generation.";
      return;
    }
    QueueCurrentCell();
  }
  if (GenerationGameplayPause::Owned() &&
      !GenerationGameplayPause::OnlyGameplayPause())
    return;
  NativeSynchronization::Update();
  // Restoration is independent of building state and placement revisions.
  // Never start collision sampling or a worker from a load/attach event.
  if (sceneLoaded &&
      attachmentRestoreSchedule.Due(std::chrono::steady_clock::now()) &&
      !automaticRestoreSuppressed.load() && !NativeFormPreparation::Pending() &&
      !generationCommandActive && !generationRequested.load() &&
      !future.valid() && !capture && !baselineCell &&
      NativeSynchronization::Available() &&
      NativeSynchronization::ProbeState() != PauseProbeState::kWaiting) {
    const auto ui = RE::UI::GetSingleton();
    const auto player = RE::PlayerCharacter::GetSingleton();
    const auto cell = player ? player->GetParentCell() : nullptr;
    if (cell && cell->IsAttached() && ui && !ui->GameIsPaused() &&
        !ui->IsMenuOpen("Loading Menu")) {
      const auto candidates = attachmentRestoreSchedule.Cells();
      attachmentRestoreSchedule.Scanned(std::chrono::steady_clock::now());
      const auto ready =
          NativeReplacement::ReadyDeferredConnections(candidates);
      bool needed = false;
      for (const auto id : ready)
        needed |= deferredAttempts.insert(id).second;
      for (const auto id : candidates) {
        if (!cellCache.contains(id))
          continue;
        const auto cachedCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
        if (!cachedCell || !cachedCell->IsAttached())
          restoreAttempts.erase(id);
        else if ((!NativeReplacement::Installed() ||
                  !NativeReplacement::DependsOnCell(id)) &&
                 restoreAttempts.insert(id).second)
          needed = true;
      }
      if (needed) {
        spdlog::info(
            "Restaging committed navigation for newly attached neighbors: {} "
            "available deferred targets; cached geometry reused",
            ready.size());
        NativeSynchronization::RequestCommittedRestore();
      }
    }
  }
  if (!generationEnabled)
    return;
  if (future.valid() &&
      future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      auto result = future.get();
      if (!NativeAuthoredBasis::Matches(buildingRecordLease))
        result.error = "Winning authored records changed during generation; "
                       "retry Generate.";
      buildingRecordLease = {};
      if (sceneLoaded && result.revision == revision && !result.cancelled) {
        if (result.Valid()) {
          status.vertices = result.mesh.vertices.size();
          status.triangles = result.mesh.triangles.size();
          status.pendingNativeForms = result.pendingNativeForms;
          status.message =
              std::format("Generated cell {:08X}: {} vertices, {} triangles. "
                          "Not installed.",
                          buildingCell, status.vertices, status.triangles);
          spdlog::info("Generated navmesh for cell {:08X}: {} "
                       "vertices, {} triangles. Not installed in Skyrim.",
                       buildingCell, result.mesh.vertices.size(),
                       result.mesh.triangles.size());
          if (generated.size() >= MAX_GENERATED_CELLS &&
              !generated.contains(buildingCell)) {
            const auto obsolete = std::find_if(
                generated.begin(), generated.end(), [&](const auto &entry) {
                  return !generationCells.contains(entry.first);
                });
            if (obsolete != generated.end()) {
              generatedBases.erase(obsolete->first);
              generated.erase(obsolete);
            }
          }
          if (buildingPlacements == PlacementState(buildingCell))
            pendingBatchCaches.insert_or_assign(
                buildingCell,
                CachedCell{buildingOriginals, result.mesh, buildingPlacements});
          generated.insert_or_assign(buildingCell, std::move(result));
          generatedBases.insert_or_assign(buildingCell, buildingOriginals);
          status.stage = RuntimeStage::kGenerated;
          remainingGenerationCells.erase(buildingCell);
          generationCommandActive = !remainingGenerationCells.empty();
          if (!generationCommandActive &&
              replacementAfterGeneration.exchange(false))
            NativeSynchronization::RequestReplacement();
        } else {
          spdlog::warn("Navmesh generation rejected cell {:08X}: {}",
                       buildingCell, result.error);
          status.stage = RuntimeStage::kFailed;
          replacementAfterGeneration.store(false);
          generationCommandActive = false;
          pendingBatchCaches.clear();
          status.message = result.error;
        }
      } else if (sceneLoaded && workerEpoch == sceneEpoch) {
        dirtyCells.insert(buildingCell);
        generationCommandActive = false;
        replacementAfterGeneration.store(false);
        pendingBatchCaches.clear();
      }
    } catch (const std::exception &error) {
      spdlog::error("Navmesh worker failed: {}", error.what());
      status.stage = RuntimeStage::kFailed;
      replacementAfterGeneration.store(false);
      generationCommandActive = false;
      pendingBatchCaches.clear();
      status.message = error.what();
    }
    cancellation.reset();
  }
  if (!sceneLoaded)
    return;
  if (baselineCell) {
    if (NativeSynchronization::ProbeState() != PauseProbeState::kWaiting) {
      dirtyCells.erase(*baselineCell);
      baselineCell.reset();
      status.stage = RuntimeStage::kFailed;
      status.message = "Baseline capture was cancelled or did not complete.";
    }
    return;
  }
  for (auto iterator = originals.begin(); iterator != originals.end();) {
    const auto cell =
        RE::TESForm::LookupByID<RE::TESObjectCELL>(iterator->first);
    if (!cell || !cell->IsAttached()) {
      generated.erase(iterator->first);
      generatedBases.erase(iterator->first);
      baselineTopology.erase(iterator->first);
      baselineCuts.erase(iterator->first);
      baselineCutData.erase(iterator->first);
      iterator = originals.erase(iterator);
    } else
      ++iterator;
  }
  for (auto iterator = pending.begin(); iterator != pending.end();) {
    const auto object = iterator->second.first.get();
    const auto model = object ? object->Get3D() : nullptr;
    if (!object || std::chrono::steady_clock::now() > iterator->second.second) {
      iterator = pending.erase(iterator);
      continue;
    }
    if (!model) {
      ++iterator;
      continue;
    }
    const auto cell = object->GetParentCell();
    const auto &bound = model->worldBound;
    if (!cell || !cell->IsAttached()) {
      ++iterator;
      continue;
    }
    if (!std::isfinite(bound.radius) || !std::isfinite(bound.center.x) ||
        !std::isfinite(bound.center.y) || !std::isfinite(bound.center.z) ||
        bound.radius <= 0 || bound.radius > 8192) {
      spdlog::warn("Navmesh skipped {:08X}: invalid collision sampling bounds.",
                   iterator->first);
      iterator = pending.erase(iterator);
      continue;
    }
    tracked.insert_or_assign(iterator->first,
                             Tracked{object->GetHandle(), cell->GetFormID(),
                                     bound.center, bound.radius});
    dirtyCells.insert(cell->GetFormID());
    iterator = pending.erase(iterator);
  }
  // Avoid capturing obsolete collision while a placement is rebuilding its 3D.
  if (!GenerationMayAdvance(generationCommandActive, capture.has_value(),
                            !pending.empty(), future.valid()))
    return;
  if (!capture && generationCommandActive &&
      !remainingGenerationCells.empty()) {
    const auto player = RE::PlayerCharacter::GetSingleton();
    const auto current = player ? player->GetParentCell() : nullptr;
    if (!current || current->GetFormID() != generationRoot) {
      generationCommandActive = false;
      replacementAfterGeneration.store(false);
      pendingBatchCaches.clear();
      status.message =
          "Generation cancelled: the player left the command cell.";
      return;
    }
    if (!originals.contains(generationRoot)) {
      baselineCell = generationRoot;
      if (!NativeSynchronization::RequestBaselineCapture())
        baselineCell.reset();
      return;
    }
    if (!ExpandGenerationCells()) {
      generationCommandActive = false;
      replacementAfterGeneration.store(false);
      pendingBatchCaches.clear();
      status.stage = RuntimeStage::kFailed;
      status.message = "Affected cells could not be prepared for generation.";
      return;
    }
    const auto cell = *remainingGenerationCells.begin();
    if (!originals.contains(cell) || refreshBaselines.contains(cell)) {
      baselineCell = cell;
      if (!NativeSynchronization::RequestBaselineCapture()) {
        baselineCell.reset();
        return;
      }
      status.stage = RuntimeStage::kCapturing;
      status.message = "Waiting for navigation pause to capture native meshes.";
      return;
    }
    dirtyCells.erase(cell);
    if (!BeginCapture(cell)) {
      generationCommandActive = false;
      pendingBatchCaches.clear();
      return;
    }
  }
  if (!capture)
    return;
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto playerCell = player ? player->GetParentCell() : nullptr;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(capture->cellID);
  if (!cell || !cell->IsAttached() || !playerCell ||
      playerCell->GetbhkWorld() != cell->GetbhkWorld()) {
    capture.reset();
    generationCommandActive = false;
    replacementAfterGeneration.store(false);
    pendingBatchCaches.clear();
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
  do {
    if (capture->object < capture->obstacles.size()) {
      const auto reference = capture->obstacles[capture->object].handle.get();
      std::string error;
      if (!CaptureRecastCollision(reference.get(), *capture->recastScene,
                                  error)) {
        status.stage = RuntimeStage::kFailed;
        status.message = error;
        spdlog::warn("Recast collision capture rejected: {}", error);
        capture.reset();
        generationCommandActive = false;
        replacementAfterGeneration.store(false);
        pendingBatchCaches.clear();
        return;
      }
      ++capture->object;
      ++status.sampled;
    }
    if (capture->object == capture->obstacles.size()) {
      buildingCell = capture->cellID;
      workerEpoch = sceneEpoch;
      spdlog::info("Recast capture: {} placed references, {} collision "
                   "shapes, {} ms elapsed",
                   capture->object, capture->recastScene->collisions.size(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - capture->started)
                       .count());
      buildingOriginals.clear();
      for (const auto &source : capture->input.originals)
        buildingOriginals.push_back(*source);
      buildingPlacements = PlacementState(capture->cellID);
      auto input = std::move(capture->input);
      buildingRecordLease = std::move(capture->recordLease);
      const auto topology = std::move(capture->topology);
      const auto cuts = std::move(capture->cuts);
      capture.reset();
      cancellation = std::make_shared<std::atomic_bool>(false);
      const auto token = cancellation;
      workerProgress = std::make_shared<std::atomic_uint>(0);
      const auto progress = workerProgress;
      status.stage = RuntimeStage::kBuilding;
      const auto directory = SkyBuild::Paths::DiagnosticsDirectory();
      const auto snapshotPath =
          directory ? *directory / "SkyBuild-generated-navmesh.json"
                    : std::filesystem::path{};
      future = std::async(std::launch::async, [input = std::move(input), token,
                                               snapshotPath, topology, cuts,
                                               records = buildingRecordLease,
                                               progress] {
        const auto started = std::chrono::steady_clock::now();
        const auto stop = [token] { return token->load(); };
        auto result = Build(input, stop, {}, [progress](unsigned completed) {
          progress->store(completed, std::memory_order_relaxed);
        });
        const auto built = std::chrono::steady_clock::now();
        if (result.sourceGeometry)
          spdlog::info("Recast direct geometry: {} vertices, {} input "
                       "triangles; {} generated triangles",
                       result.sourceGeometry->vertices.size(),
                       result.sourceGeometry->triangles.size(),
                       result.mesh.triangles.size());
        SurfaceOwnership ownership;
        if (result.Valid() && !token->load()) {
          std::map<std::uint32_t, std::size_t> sourceCounts;
          for (const auto &source : input.originals)
            if (source)
              sourceCounts.emplace(source->formID,
                                   source->mesh.triangles.size());
          ownership = AssignConnectedOwners(result.mesh, sourceCounts);
          if (!ownership.Valid())
            result.error = ownership.error;
          else
            result.pendingNativeForms = ownership.newFormComponents.size();
        }
        if (result.Valid())
          progress->store(5, std::memory_order_relaxed);
        const auto owned = std::chrono::steady_clock::now();
        if (!token->load() && !result.cancelled && !result.Valid() &&
            !snapshotPath.empty()) {
          try {
            SaveGeneratedSnapshot(input, result, ownership, *topology, *cuts,
                                  snapshotPath, "generation", {}, nullptr,
                                  &records);
            spdlog::debug("Saved rejected generated topology snapshot to {}",
                          snapshotPath.string());
          } catch (const std::exception &error) {
            spdlog::warn("Generated topology snapshot failed: {}",
                         error.what());
          }
        }
        const auto finished = std::chrono::steady_clock::now();
        spdlog::debug(
            "Navmesh worker stages: geometry {:.3f} ms, ownership {:.3f} ms, "
            "diagnostic snapshot {:.3f} ms",
            std::chrono::duration<double, std::milli>(built - started).count(),
            std::chrono::duration<double, std::milli>(owned - built).count(),
            std::chrono::duration<double, std::milli>(finished - owned)
                .count());
        result.sourceGeometry.reset();
        spdlog::debug("Navmesh geometry/ownership/snapshot worker: {} ms",
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count());
        return result;
      });
      break;
    }
  } while (std::chrono::steady_clock::now() < deadline);
  status.generatedCells = generated.size();
}
void ConfirmInstalledCache() {
  const std::scoped_lock lock(runtimeMutex);
  // Restoring the prior graph must not commit a newer rejected batch.
  if (restoringCommittedCaches)
    return;
  if (generationPresentation)
    generationSucceeded = true;
  if (!pendingBatchCaches.empty()) {
    for (const auto &[id, cached] : pendingBatchCaches)
      if (cached.placements != PlacementState(id)) {
        pendingBatchCaches.clear();
        return;
      }
    for (auto &[id, cached] : pendingBatchCaches)
      cellCache.insert_or_assign(id, std::move(cached));
    pendingBatchCaches.clear();
  }
}
void SaveCache(SKSE::SerializationInterface *serialization) {
  const std::scoped_lock lock(runtimeMutex);
  if (!serialization)
    return;
  try {
    const auto bytes = EncodeCellCache(cellCache);
    if (!serialization->OpenRecord(CELL_CACHE_RECORD, 1) ||
        !serialization->WriteRecordData(
            bytes.data(), static_cast<std::uint32_t>(bytes.size())))
      throw std::runtime_error("Could not write navmesh save cache");
    spdlog::info(
        "Saved {} navmesh cell caches ({} bytes), owned geometry only.",
        cellCache.size(), bytes.size());
  } catch (const std::exception &error) {
    spdlog::error("Navmesh cache save failed: {}", error.what());
  }
}
void LoadCache(SKSE::SerializationInterface *serialization,
               std::uint32_t version, std::uint32_t length) {
  const std::scoped_lock lock(runtimeMutex);
  if (!serialization || version != 1 || length > MAX_CELL_CACHE_BYTES)
    return;
  try {
    std::vector<std::uint8_t> bytes(length);
    if (serialization->ReadRecordData(bytes.data(), length) != length)
      throw std::runtime_error("Truncated navmesh save cache");
    cellCache = DecodeCellCache(
        bytes,
        [serialization](std::uint32_t id) -> std::optional<std::uint32_t> {
          std::uint32_t resolved{};
          if (!serialization->ResolveFormID(id, resolved))
            return std::nullopt;
          return resolved;
        });
    spdlog::info("Loaded {} navmesh cell caches; native publication waits for "
                 "attached cells.",
                 cellCache.size());
  } catch (const std::exception &error) {
    spdlog::error("Navmesh cache load rejected: {}", error.what());
  }
}
bool GenerationPresentationActive() noexcept {
  return generationPresentation.load(std::memory_order_relaxed);
}
RuntimeStatus GetStatus() {
  const std::scoped_lock lock(runtimeMutex);
  auto current = status;
  current.generating = generationPresentation;
  current.generationSucceeded = generationSucceeded;
  if (generationPresentation && GenerationGameplayPause::Owned()) {
    current.commandCells = generationCells.size();
    current.completedCells =
        current.commandCells - remainingGenerationCells.size();
    // Each cell has collection and building steps; publication is the last
    // command step. Substeps measure actual completed work, not time weights.
    double completed = 2.0 * current.completedCells;
    if (capture && status.total)
      completed += std::min(1.0, double(status.sampled) / status.total);
    else if (future.valid())
      completed +=
          1.0 + (workerProgress
                     ? workerProgress->load(std::memory_order_relaxed) / 5.0
                     : 0.0);
    current.progress =
        static_cast<float>(completed / (2.0 * current.commandCells + 1.0));
  }
  if (generationSucceeded)
    current.progress = 1.0F;
  current.generatedCells = generated.size();
  current.cachedCells = cellCache.size();
  return current;
}
} // namespace SkyBuild::Navmesh
