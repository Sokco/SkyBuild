#include "SkyBuildPaths.h"
#include "navmesh/native/NativeFormDraft.h"
#include "navmesh/cuts/CutTransferIdentity.h"
#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/NativeCutData.h"
#include "navmesh/diagnostics/FailureArchive.h"
#include "navmesh/diagnostics/OwnedNavmeshEvidence.h"
#include "navmesh/diagnostics/TimingTrace.h"
#include "navmesh/diagnostics/TopologySnapshot.h"
#include "navmesh/generation/GeneratedTopology.h"
#include "navmesh/generation/IslandDraft.h"
#include "navmesh/generation/SurfaceOwnership.h"
#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeGeometry.h"
#include "navmesh/native/NativeGraphDraft.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include "navmesh/native/WorkCopyContract.h"
#include "navmesh/publication/NativePublication.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
namespace SkyBuild::Navmesh {
NativeFormDraft::~NativeFormDraft() {
  if (!form)
    return;
  const bool registered = NativeDraftInfo::Registered(ID());
  // The form owns one intrusive reference. Observe the count through native
  // atomic operations while that reference keeps it alive. Absence of another
  // strong reference is not a certificate against untracked raw borrowers.
  form->IncRef();
  const auto references = form->DecRef();
  if (registered || references != 1) {
    // Quarantine the FORM AND INFO together. Letting member destruction free
    // the info while another owner retains its form leaves a dangling mesh
    // backlink, even when the form was never navigation-registered.
    spdlog::critical("Native draft {:08X} cleanup retains its form/info pair: "
                     "registered {}, "
                     "{} strong references. Quarantined until restart.",
                     ID(), registered, references);
    navigationInfo.release();
    form.release();
  }
}
void NativeFormDraft::FormDeleter::operator()(RE::NavMesh *value) const {
  // Release this draft's one intrusive reference. During native form adoption
  // the superseded-form list deliberately retains the previous allocation;
  // that ordinary release is not a cleanup failure or a quarantine. Final
  // draft destruction checks joint form/info ownership above, before members
  // release either object.
  ReleaseNativeFormReference(value);
}
std::unique_ptr<NativeFormDraft>
NativeFormDraft::Create(const Mesh &mesh,
                        const RE::BSTSmartPointer<RE::BSPathingCell> &parent,
                        std::string &error) {
  NativeGeometry buffers(mesh);
  if (!buffers.Valid()) {
    error = buffers.Error();
    return nullptr;
  }
  auto draft = std::make_unique<NativeFormDraft>();
  draft->expected = std::make_unique<NativeGeometry>(mesh);
  if (!draft->expected->Valid()) {
    error = draft->expected->Error();
    return nullptr;
  }
  draft->form.reset(RE::IFormFactory::Create<RE::NavMesh>());
  if (!draft->form) {
    error = "Native NavMesh factory returned no form";
    return nullptr;
  }
  draft->form->IncRef();
  draft->runtimeID = draft->form->GetFormID();
  // Runtime navigation is persisted as owned geometry in our SKSE record;
  // factory forms and their FF IDs belong only to the current engine scene.
  draft->form->formFlags |= RE::TESForm::RecordFlags::kTemporary;
  const auto id = draft->ID();
  if (id < 0xFF000000 || RE::TESForm::LookupByID(id) != draft->form.get()) {
    error = "Native factory did not create a registered runtime form";
    return nullptr;
  }
  auto &target = static_cast<RE::BSNavmesh &>(*draft->form);
  if (!target.vertices.empty() || !target.triangles.empty() ||
      !target.extraEdgeInfo.empty() || !target.doorPortals.empty() ||
      !target.closedDoors.empty() || !target.coverArray.empty() ||
      !target.obstacles.empty() || !target.unk0F0.empty() ||
      (target.triangleToObstacleMap &&
       !target.triangleToObstacleMap->empty()) ||
      target.parentCell || target.unk110) {
    error = "Native factory form already has navigation data";
    return nullptr;
  }
  // This exchange touches only the just-created draft, never a loaded mesh.
  // NativeGeometry now owns the factory's empty buffers for normal destruction.
  buffers.Exchange(target);
  target.parentCell = parent;
  draft->navigationInfo = NativeDraftInfo::Create(*draft->form, error);
  if (!draft->navigationInfo)
    return nullptr;
  if (!draft->Matches()) {
    error = "Native draft geometry does not match";
    return nullptr;
  }
  return draft;
}
bool NativeFormDraft::Matches() const { return ValidationError().empty(); }
std::string NativeFormDraft::ValidationError(bool published) const {
  if (!form || RE::TESForm::LookupByID(ID()) != form.get())
    return "Detached form missing from general lookup";
  if (!expected || !expected->Valid())
    return "Missing or invalid detached geometry baseline";
  if (!expected->Matches(*form))
    return "Detached geometry or spatial grid changed";
  if (!navigationInfo)
    return "Missing detached navigation info";
  return navigationInfo->ValidationError(*form, published);
}
namespace NativeFormPreparation {
namespace {
// Explicit Finish/Clear owns cleanup while the engine is alive. Avoid native
// destructors during C++ static teardown after engine singleton destruction.
auto &drafts = *new std::vector<std::unique_ptr<NativeFormDraft>>;
auto &graphDraft = *new std::unique_ptr<NativeGraphDraft>;
std::vector<RE::FormID> ids;
std::set<RE::FormID> loadedBefore;
RE::FormID cellID{};
bool preparationStarted{};
PartitionedMesh preparedTopology;
std::map<std::uint32_t, AuthoredMesh> expectedTopology;
NativeCutSnapshot expectedCuts;
auto &expectedCutData = *new std::vector<NativeCutDataSource>;
// Workers own geometry only. Native retained inputs remain on the game thread.
struct TopologyWork {
  std::atomic_bool ready{};
  PartitionedMesh result;
  TopologyProfile profile;
  std::string error;
};
struct PendingPreparation {
  AuthoredBasisSnapshot recordLease;
  unsigned freshnessRestarts{};
  std::shared_ptr<TopologyWork> work;
  Mesh input;
  NativeDoorAnchors anchors;
  std::map<std::uint32_t, AuthoredMesh> portableOriginals;
  SurfaceOwnership ownership;
  std::vector<RE::FormID> islandCells;
  std::set<RE::FormID> replacedCells;
  RE::BSTSmartPointer<RE::BSPathingCell> parent;
};
auto &pendingPreparation = *new std::unique_ptr<PendingPreparation>;
class StalePreparation : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
constexpr unsigned MAX_FRESHNESS_RESTARTS = 3;

std::mutex statusMutex;
std::string status{"Navigation is ready."};
NativeDoorAnchors
CaptureDoorAnchors(const std::map<std::uint32_t, AuthoredMesh> &originals) {
  NativeDoorAnchors doorAnchors;

  const auto sameCell = [](const RE::BSPathingCell *a,
                           const RE::BSPathingCell *b) {
    if (!a || !b)
      return false;
    if (a == b)
      return true;
    const auto left = skyrim_cast<const RE::PathingCell *>(a);
    const auto right = skyrim_cast<const RE::PathingCell *>(b);
    return left && right && left->pathingCellInfo == right->pathingCellInfo;
  };
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
  const RE::BSReadLockGuard guard(*publication.get());
  for (const auto &[id, original] : originals) {
    if (original.doorTriangles.empty() && original.closedDoorTriangles.empty())
      continue;
    const auto source = RE::TESForm::LookupByID<RE::NavMesh>(id);
    if (!source)
      continue;
    auto capture = [&](const auto &records, const auto &indices, auto &points,
                       auto &unbound) {
      if (records.size() != indices.size())
        return;
      for (std::size_t index = 0; index < indices.size(); ++index) {
        std::optional<Point> point;
        const auto door = skyrim_cast<RE::PathingDoor *>(
            records[static_cast<std::uint32_t>(index)].door.get());
        if (door) {
          if (!door->refHandle && !door->forwardLocation.pathingCell &&
              !door->backwardLocation.pathingCell &&
              !door->forwardLocation.navMeshInfo &&
              !door->backwardLocation.navMeshInfo &&
              door->forwardLocation.triangle == 0xFFFF &&
              door->backwardLocation.triangle == 0xFFFF)
            unbound.push_back(static_cast<std::uint16_t>(index));
          for (const auto *location :
               {&door->forwardLocation, &door->backwardLocation}) {
            // Cached info may still name the retired runtime graph during
            // a handoff. The native door record authorizes this authored
            // face; use its cell and access position, not that stale ID.
            if (!sameCell(location->pathingCell.get(),
                          source->parentCell.get()))
              continue;
            const Point candidate{location->location.x, location->location.y,
                                  location->location.z};
            // Pathing locations may be above the triangle surface. Keep
            // XY fixed and project only onto the door's authored face.
            const auto projected =
                ProjectDoorAnchor(original, indices[index], candidate);
            if (!projected)
              continue;
            if (point && *point != *projected) {
              point.reset();
              break;
            }
            point = projected;
          }
        }
        if (!point && door) {
          spdlog::debug(
              "Door anchor unavailable {:08X}/{}: forward ({}, {}, {}) "
              "cell match {}; backward ({}, {}, {}) cell match {}",
              id, indices[index], door->forwardLocation.location.x,
              door->forwardLocation.location.y,
              door->forwardLocation.location.z,
              sameCell(door->forwardLocation.pathingCell.get(),
                       source->parentCell.get()),
              door->backwardLocation.location.x,
              door->backwardLocation.location.y,
              door->backwardLocation.location.z,
              sameCell(door->backwardLocation.pathingCell.get(),
                       source->parentCell.get()));
          const auto reference =
              RE::TESObjectREFR::LookupByHandle(door->refHandle);
          if (reference) {
            const auto position = reference->GetPosition();
            spdlog::debug(
                "Door reference {:08X}/{}: form {:08X}, cell {:08X}, "
                "position ({}, {}, {}), authored projection available {}",
                id, indices[index], reference->GetFormID(),
                reference->GetParentCell()
                    ? reference->GetParentCell()->GetFormID()
                    : 0,
                position.x, position.y, position.z,
                ProjectDoorAnchor(original, indices[index],
                                  {position.x, position.y, position.z})
                    .has_value());
          }
        }
        points.push_back(point);
      }
    };
    auto &anchors = doorAnchors[id];
    capture(source->doorPortals, original.doorTriangles, anchors.doors,
            anchors.unboundDoors);
    capture(source->closedDoors, original.closedDoorTriangles,
            anchors.closedDoors, anchors.unboundClosedDoors);
  }
  return doorAnchors;
}
void Publish(std::string text) {
  const std::scoped_lock lock(statusMutex);
  status = std::move(text);
  spdlog::debug("Native navmesh form preparation: {}", status);
}
std::set<RE::FormID> LoadedIDs() {
  std::set<RE::FormID> result;
  const auto pathing = RE::Pathing::GetSingleton();
  if (!pathing)
    throw std::runtime_error("No native pathing instance");
  const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
  const RE::BSWriteLockGuard guard(*publication.get());
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> loaded;
  if (!pathing->GetAllLoadedNavmeshes5(loaded))
    throw std::runtime_error("Could not inspect loaded navigation");
  for (const auto &mesh : loaded)
    if (mesh)
      result.insert(mesh->QNavmeshID());
  return result;
}

} // namespace
void Clear() {
  pendingPreparation.reset();
  graphDraft.reset();
  drafts.clear();
  preparedTopology = {};
  expectedTopology.clear();
  expectedCuts = {};
  expectedCutData.clear();
  ids.clear();
  loadedBefore.clear();
  cellID = 0;
  preparationStarted = false;
}
bool Pending() { return bool(pendingPreparation); }
bool PendingReady() {
  return pendingPreparation &&
         pendingPreparation->work->ready.load(std::memory_order_acquire);
}
bool Prepare() {
  TimingTrace timing{"NativeFormPreparation::Prepare"};
  if (pendingPreparation) {
    if (!PendingReady())
      return false;
    try {
      const auto player = RE::PlayerCharacter::GetSingleton();
      const auto cell = player ? player->GetParentCell() : nullptr;
      if (!cell || !cell->IsAttached() || cell->GetFormID() != cellID)
        throw std::runtime_error(
            "Preparation cell changed; asynchronous candidate discarded");
      Mesh freshMesh;
      SurfaceOwnership freshOwnership;
      std::map<std::uint32_t, AuthoredMesh> freshTopology;
      NativeCutSnapshot freshCuts;
      std::vector<NativeCutDataSource> freshCutData;
      std::vector<RE::FormID> freshIslands;
      std::set<RE::FormID> freshCells;
      if (!CopyGeneratedForPreparation(
              freshMesh, freshOwnership, &freshTopology, &freshCuts,
              &freshCutData, &freshIslands, &freshCells))
        throw std::runtime_error(
            "Async navigation inputs are no longer available");
      const auto &pending = *pendingPreparation;
      if (!NativeAuthoredBasis::Matches(pending.recordLease) ||
          freshMesh != pending.input ||
          freshOwnership.owners != pending.ownership.owners ||
          freshOwnership.newFormComponents !=
              pending.ownership.newFormComponents ||
          freshIslands != pending.islandCells ||
          freshCells != pending.replacedCells ||
          freshTopology != expectedTopology || freshCuts != expectedCuts ||
          LoadedIDs() != loadedBefore ||
          freshCutData.size() != expectedCutData.size())
        throw StalePreparation("Navigation changed during asynchronous "
                               "preparation; candidate discarded");
      for (std::size_t i = 0; i < freshCutData.size(); ++i) {
        const auto &a = freshCutData[i], &b = expectedCutData[i];
        if (a.meshID != b.meshID || a.original != b.original ||
            a.rigidBody != b.rigidBody || a.auxiliary != b.auxiliary ||
            a.owner != b.owner || !SameCutTransferShape(a.values, b.values) ||
            a.affectedMeshes != b.affectedMeshes ||
            a.affectedInfos != b.affectedInfos)
          throw StalePreparation(
              "Native cut inputs changed during asynchronous preparation");
      }
      if (CaptureDoorAnchors(pending.portableOriginals) != pending.anchors)
        throw StalePreparation(
            "Door access points changed during asynchronous preparation");
      timing.Mark("revalidate asynchronous preparation inputs");
      if (!pending.work->error.empty())
        throw std::runtime_error(pending.work->error);
      preparedTopology = std::move(pending.work->result);
      if (!preparedTopology.Valid())
        throw std::runtime_error("Complete replacement graph rejected: " +
                                 preparedTopology.error);
      std::string error;
      graphDraft = NativeGraphDraft::Create(preparedTopology, pending.parent,
                                            expectedTopology, expectedCuts,
                                            pending.recordLease, error);
      if (!graphDraft)
        throw std::runtime_error(error);
      const auto graphIDs = graphDraft->IDs();
      ids.insert(ids.end(), graphIDs.begin(), graphIDs.end());
      pendingPreparation.reset();
      timing.Mark("complete native graph from asynchronous topology");
      return true;
    } catch (const StalePreparation &error) {
      const auto restarts = pendingPreparation->freshnessRestarts;
      Clear();
      if (restarts >= MAX_FRESHNESS_RESTARTS) {
        Publish("Native navigation did not stabilize after three preparation "
                "restarts.");
        return false;
      }
      spdlog::debug("Discarding stale private topology: {}; recapturing native "
                    "inputs ({}/{})",
                    error.what(), restarts + 1, MAX_FRESHNESS_RESTARTS);
      const auto ready = Prepare();
      if (pendingPreparation)
        pendingPreparation->freshnessRestarts = restarts + 1;
      return ready;
    } catch (const std::exception &error) {
      Publish(error.what());
      pendingPreparation.reset();
      return false;
    }
  }
  Clear();
  try {
    Mesh mesh;
    SurfaceOwnership ownership;
    std::vector<RE::FormID> islandCells;
    std::set<RE::FormID> replacedCells;
    if (!CopyGeneratedForPreparation(mesh, ownership, &expectedTopology,
                                     &expectedCuts, &expectedCutData,
                                     &islandCells, &replacedCells)) {
      const auto status = GetStatus();
      Publish(status.message.empty()
                  ? "No navigation batch available. Generate Navmesh first."
                  : status.message);
      return false;
    }
    Mesh capturedInput = mesh;
    SurfaceOwnership capturedOwnership = ownership;
    const auto player = RE::PlayerCharacter::GetSingleton();
    timing.Mark("fresh originals, cached batch assembly and ownership");
    const auto cell = player ? player->GetParentCell() : nullptr;
    if (!cell || !cell->IsAttached())
      throw std::runtime_error("No attached cell");
    cellID = cell->GetFormID();
    loadedBefore = LoadedIDs();
    preparationStarted = true;
    RE::BSTSmartPointer<RE::BSPathingCell> parent;
    timing.Mark("capture native parent context");
    {
      const REL::Relocation<RE::BSReadWriteLock *> publication{REL::ID(402045)};
      const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
      const RE::BSWriteLockGuard publicationGuard(*publication.get());
      const RE::BSReadLockGuard listingGuard(*listing.get());
      if (cell->GetRuntimeData().navMeshes)
        for (const auto &source : cell->GetRuntimeData().navMeshes->navMeshes)
          if (source && source->parentCell &&
              source->GetSaveParentCell() == cell) {
            parent = source->parentCell;
            break;
          }
    }
    if (!parent &&
        (!ownership.newFormComponents.empty() || !expectedCutData.empty()))
      throw std::runtime_error("No native pathing cell for island drafts");
    if (ownership.newFormComponents.size() > 64)
      throw std::runtime_error("Native draft budget exceeded");
    for (std::size_t componentIndex = 0;
         componentIndex < ownership.newFormComponents.size();
         ++componentIndex) {
      const auto &component = ownership.newFormComponents[componentIndex];
      RE::BSTSmartPointer<RE::BSPathingCell> islandParent;
      const auto islandCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(
          islandCells.at(componentIndex));
      if (!islandCell || !islandCell->IsAttached())
        throw std::runtime_error("Cached island cell is no longer attached");
      const REL::Relocation<RE::BSReadWriteLock *> listing{REL::ID(402042)};
      {
        const RE::BSReadLockGuard guard(*listing.get());
        if (islandCell->GetRuntimeData().navMeshes)
          for (const auto &source :
               islandCell->GetRuntimeData().navMeshes->navMeshes)
            if (source && source->parentCell) {
              islandParent = source->parentCell;
              break;
            }
      }
      if (!islandParent)
        throw std::runtime_error("Cached island lacks its native pathing cell");
      auto input = ExtractIslandDraft(mesh, component);
      std::string error;
      auto draft = NativeFormDraft::Create(input, islandParent, error);
      if (!draft)
        throw std::runtime_error(error);
      for (const auto index : component)
        ownership.owners.at(index) = draft->ID();
      ids.push_back(draft->ID());
      drafts.push_back(std::move(draft));
    }
    std::set<std::uint32_t> replaced;
    timing.Mark("island native forms");
    for (const auto &face : mesh.triangles)
      if (face.source.Authored())
        replaced.insert(face.source.formID);
    // Include wholly removed source meshes too; they may have incoming links.
    for (const auto &[id, original] : expectedTopology) {
      const auto source = RE::TESForm::LookupByID<RE::NavMesh>(id);
      if (source && source->GetSaveParentCell() &&
          replacedCells.contains(source->GetSaveParentCell()->GetFormID()))
        replaced.insert(id);
    }
    std::set<std::uint32_t> requiredRecords;
    for (const auto &[id, source] : expectedTopology)
      requiredRecords.insert(id);
    AuthoredBasisSnapshot recordLease;
    const auto basis = NativeAuthoredBasis::PrepareGeneration(
        requiredRecords, replaced, recordLease, expectedTopology);
    if (!basis.Valid())
      throw std::runtime_error(basis.error);
    timing.Mark("uncut generation basis");
    auto doorAnchors = CaptureDoorAnchors(basis.topology);

    auto pending = std::make_unique<PendingPreparation>();
    pending->recordLease = std::move(recordLease);
    pending->work = std::make_shared<TopologyWork>();
    pending->input = std::move(capturedInput);
    pending->ownership = std::move(capturedOwnership);
    pending->islandCells = std::move(islandCells);
    pending->replacedCells = std::move(replacedCells);
    pending->parent = parent;
    pending->anchors = doorAnchors;
    pending->portableOriginals = basis.topology;
    // No native pointer, retained form, cut object, or engine call crosses
    // this boundary. Cancellation drops the game-thread owner without
    // waiting.
    const auto logDirectory = SkyBuild::Paths::DiagnosticsDirectory();
    const auto snapshotPath =
        logDirectory ? *logDirectory / "SkyBuild-topology-batch.json"
                     : std::filesystem::path{};
    std::thread worker([work = pending->work, snapshotPath,
                        mesh = std::move(mesh),
                        owners = std::move(ownership.owners),
                        originals = std::move(basis.topology),
                        replaced = std::move(replaced),
                        anchors = std::move(doorAnchors)]() mutable {
      try {
        work->result = PreserveGeneratedTopology(
            mesh, owners, originals, replaced, anchors, &work->profile);
        if (!work->result.Valid() && !snapshotPath.empty()) {
          try {
            SaveTopologySnapshot(mesh, owners, originals, replaced, anchors,
                                 work->result.error, snapshotPath);
            spdlog::debug("Saved complete rejected topology batch to {}",
                          snapshotPath.string());
          } catch (const std::exception &error) {
            spdlog::warn("Topology diagnostic capture failed: {}",
                         error.what());
          }
        }
        for (const auto &[stage, milliseconds] : work->profile.stages)
          spdlog::debug("Navmesh asynchronous topology stage: {}: {:.3f} ms",
                        stage, milliseconds);
      } catch (const std::exception &error) {
        work->error = error.what();
      } catch (...) {
        work->error = "Asynchronous topology preparation failed";
      }
      work->ready.store(true, std::memory_order_release);
    });
    worker.detach();
    pendingPreparation = std::move(pending);
    Publish("Preparing replacement topology asynchronously; current "
            "navigation retained.");
    timing.Mark("capture asynchronous topology inputs");
    return false;
  } catch (const std::exception &error) {
    Publish(error.what());
    return false;
  }
}
void Finish() {
  if (pendingPreparation)
    return;
  if (!preparationStarted)
    return;
  const auto created = ids;
  const auto before = loadedBefore;
  graphDraft.reset();
  drafts.clear();
  try {
    for (const auto id : created)
      if (RE::TESForm::LookupByID(id))
        throw std::runtime_error("Native form lookup cleanup failed");
    if (LoadedIDs() != before)
      throw std::runtime_error("Navigation changed during native form cleanup");
  } catch (const std::exception &error) {
    Publish(error.what());
  }
  Clear();
}
std::string Status() {
  const std::scoped_lock lock(statusMutex);
  return status;
}
std::unique_ptr<NativeGraphDraft> TakeGraphForReplacement() {
  auto result = std::move(graphDraft);
  Clear();
  return result;
}
} // namespace NativeFormPreparation
} // namespace SkyBuild::Navmesh
