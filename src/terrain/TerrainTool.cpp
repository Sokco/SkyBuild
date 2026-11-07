#include "terrain/TerrainTool.h"
#include "RE/C/CellMopp.h"
#include "RE/E/ExtraHavok.h"
#include "RE/S/SendHUDMessage.h"
#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "physics/Raycast.h"
#include "terrain/NativeTerrainChunk.h"
#include "terrain/TerrainBuffers.h"
#include "terrain/TerrainCollisionQueue.h"
#include "terrain/TerrainDensitySerialization.h"
#include "terrain/TerrainGrass.h"
#include "terrain/TerrainHeightQueries.h"
#include "terrain/TerrainLoadRecovery.h"
#include "terrain/TerrainNavigation.h"
#include "terrain/TerrainSmoothing.h"
#include "terrain/TerrainStroke.h"
#include "terrain/brush-overlay/BrushOverlay.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/UI.h"
#include <atomic>
#include <chrono>
#include <map>

namespace SkyBuild::Terrain {
namespace {
constexpr std::size_t MaxCells = 64;
constexpr float Reach = 768;
std::atomic_bool modeEnabled{false};
std::atomic<Tool> selectedTool{Tool::Sculpt};
bool sceneLoaded{};
std::atomic<std::shared_ptr<const RecoverySnapshot>> loadRecovery;
void RemoveLoadRecovery(RE::FormID id) {
  loadRecovery.store(WithoutRecoveryCell(loadRecovery.load(), id));
}
void PublishLoadRecovery(RE::FormID id, const Volume &volume) {
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
  const auto world = cell ? cell->GetRuntimeData().worldSpace : nullptr;
  loadRecovery.store(WithRecoveryCell(loadRecovery.load(), id,
                                      world ? world->GetFormID() : 0, volume));
}
template <std::uint64_t Target>
__declspec(noinline) bool
RecoveryHeightQuery(void *owner, const RE::NiPoint3 &position, float &height) {
  // Lower-level callers may supply XY only; do not read a nonexistent Z.
  const RE::NiPoint3 input{position.x, position.y,
                           Target == 13344 ? position.z : 0.F};
  using Query = bool (*)(void *, const RE::NiPoint3 &, float &);
  static const REL::Relocation<Query> original{REL::ID(Target)};
  bool result = original(owner, position, height);
  {
    const auto snapshot = loadRecovery.load();
    const auto tes = static_cast<RE::TES *>(owner);
    const auto world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
    if (snapshot && world)
      for (const auto &saved : *snapshot) {
        const auto &entry = *saved;
        if (entry.world != world->GetFormID())
          continue;
        // The native recovery caller ignores the query's success flag. Supply
        // our known floor even if freshly streamed LAND is not queryable yet.
        const float nativeHeight =
            result ? height : std::numeric_limits<float>::max();
        if (const auto recovery = LoadRecoveryHeight(
                entry.volume, {input.x, input.y, input.z}, nativeHeight)) {
          height = *recovery;
          result = true;
          break;
        }
      }
  }
  return result;
}
void InstallLoadRecoveryHook() {
  const auto version = REL::Module::get().version();
  // The caller changed size in 1.7.104. Both sites pass XYZ and a float output
  // to TES::GetLandHeight, then compare that height with the recovery position.
  std::uintptr_t offset{};
  if (version == REL::Version(1, 6, 1170, 0))
    offset = 0xCD9;
  else if (version == REL::Version(1, 7, 104, 0))
    offset = 0xCEA;
  else
    return;
  const auto site = REL::ID(37507).address() + offset;
  std::int32_t displacement{};
  std::memcpy(&displacement, reinterpret_cast<void *>(site + 1),
              sizeof(displacement));
  if (*reinterpret_cast<const std::uint8_t *>(site) != 0xE8 ||
      site + 5 + displacement != REL::ID(13344).address()) {
    spdlog::warn(
        "Terrain load recovery skipped modified call site (37507+0x{:X}).",
        offset);
    return;
  }
  SKSE::GetTrampoline().write_call<5>(site, RecoveryHeightQuery<13344>);
  spdlog::info("Installed edited-terrain below-ground recovery query hook.");
}
std::atomic_bool ready{}, converted{}, busy{}, collisionBusy{};
std::atomic_int progress{};
std::atomic<float> brushRadius{96.F};

enum class Command { None, Convert, Undo, Redo, Restore };
std::atomic<Command> queuedCommand{Command::None};
enum class BrushMode : std::uint64_t {
  Smooth = 0,
  Subtract = 1,
  Add = 2,
  Level = 3
};
constexpr std::uint64_t EncodeBrushState(std::uint64_t stroke, BrushMode mode) {
  return (stroke << 2) | static_cast<std::uint64_t>(mode);
}
constexpr BrushMode DecodeBrushMode(std::uint64_t state) {
  return static_cast<BrushMode>(state & 3);
}
constexpr std::uint64_t DecodeBrushStroke(std::uint64_t state) {
  return state >> 2;
}
// Hand off mode and stroke together so input cannot mix two different presses.
std::atomic_uint64_t heldBrush{}, tappedBrush{}, nextStroke{1};
std::atomic_uint32_t heldButton{std::numeric_limits<std::uint32_t>::max()};
std::uint64_t editingStroke{};
LevelStrokeTarget levelTarget;
float editingStrength{};
std::atomic<BrushStrengthClock::Clock::duration::rep> strokePressed{},
    strokeReleased{};
BrushStrengthClock strengthClock;
BrushRepeat brushRepeat;
std::atomic<float> strengthPerSecond{BrushStrengthPerSecond};

using Chunks = std::array<std::shared_ptr<NativeChunk>, 16>;
struct RetiredChunk {
  std::shared_ptr<NativeChunk> chunk;
  unsigned frames = 3;
};
std::vector<RetiredChunk> retired;

struct Original {
  RE::NiPointer<RE::NiAVObject> node;
  bool culled{};
};
struct Live {
  RE::NiPointer<RE::NiNode> parent;
  RE::TESObjectLAND::LoadedLandData
      *land{}; // dereference only after current cell/owner identity checks
  RE::NiPointer<RE::CellMopp> collisionOwner;
  Chunks chunks;
  Chunks colliders, nextColliders;
  CollisionQueue collisionQueue;
  bool collisionReady{};
  Grass::Surfaces surfaces;
  std::vector<Original> originals;
  std::vector<RE::NiPointer<RE::bhkRigidBody>> bodies;
  LandHeights originalHeights{};
  float heightBase{};
};
struct Cell {
  Volume volume;
  std::optional<Live> live;
  bool failed{};
  std::chrono::steady_clock::time_point nextPreparation{};
};
std::map<RE::FormID, Cell> cells;
bool CollisionPending() { return collisionBusy.load(); }
void RefreshCollisionState() {
  collisionBusy.store(
      std::any_of(cells.begin(), cells.end(), [](const auto &item) {
        return item.second.live && item.second.live->collisionQueue.Pending();
      }));
}
struct Pending {
  RE::FormID cell{};
  Volume volume;
  RE::NiPointer<RE::NiNode> parent;
  RE::TESObjectLAND::LoadedLandData *land{}; // identity only
  RE::NiPointer<RE::CellMopp> collisionOwner;
  Chunks chunks;
  Grass::Surfaces surfaces;
  ChunkMask dirty;
  std::vector<TerrainTextures> textures;
  int next{};
};
std::optional<Pending> pending;
std::vector<std::pair<RE::FormID, Volume>> batch;
std::size_t batchNext{};
std::vector<Pending> staged;
std::optional<RE::FormID> restoreAfterBatch;
using StrokeRecord = std::map<RE::FormID, std::vector<RemovedBrush>>;
std::vector<StrokeRecord> redoHistory;
enum class HistoryAction { None, Edit, Undo, Redo, Restore };
HistoryAction pendingHistory{};
StrokeRecord pendingUndo;

void Notify(const char *message) {
  RE::SendHUDMessage::ShowHUDMessage(message);
}
RE::TESObjectCELL *CurrentCell() {
  const auto p = RE::PlayerCharacter::GetSingleton();
  return p ? p->GetParentCell() : nullptr;
}
RE::NiNode *Parent(RE::TESObjectCELL *cell) {
  const auto data = cell ? cell->GetRuntimeData().loadedData : nullptr;
  return cell && cell->IsAttached() && data ? data->cell3D.get() : nullptr;
}
RE::TESObjectLAND::LoadedLandData *Land(RE::TESObjectCELL *cell) {
  const auto land = cell ? cell->GetRuntimeData().cellLand : nullptr;
  return land ? land->loadedData : nullptr;
}
RE::CellMopp *CollisionOwner(RE::TESObjectCELL *cell) {
  const auto extra =
      cell ? cell->extraList.GetByType<RE::ExtraHavok>() : nullptr;
  return extra ? extra->cellMopp.get() : nullptr;
}
RE::bhkRigidBody *Body(RE::NiAVObject *node) {
  const auto collision = node && node->collisionObject
                             ? node->collisionObject->AsBhkNiCollisionObject()
                             : nullptr;
  return collision && collision->body ? collision->body->AsBhkRigidBody()
                                      : nullptr;
}
void Retire(std::shared_ptr<NativeChunk> &chunk) {
  if (!chunk)
    return;
  chunk->Detach();
  retired.push_back({std::move(chunk), 3});
}
void RemoveChunks(Chunks &chunks) {
  for (auto &chunk : chunks)
    Retire(chunk);
}
void Release(RE::FormID id, Cell &entry) {
  // Streaming detaches geometry, not the edited surface. Fast travel can query
  // LAND before that geometry/collision has been reconstructed on arrival.
  if (sceneLoaded)
    PublishLoadRecovery(id, entry.volume);
  if (!entry.live)
    return;
  auto &live = *entry.live;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
  RemoveChunks(live.chunks);
  RemoveChunks(live.colliders);
  RemoveChunks(live.nextColliders);
  for (auto &original : live.originals)
    original.node->SetAppCulled(original.culled);
  // Never insert old collision into a newly streamed instance of this cell.
  if (Parent(cell) == live.parent.get() && Land(cell) == live.land &&
      CollisionOwner(cell) == live.collisionOwner.get()) {
    std::memcpy(live.land->heights, live.originalHeights.data(),
                sizeof(live.land->heights));
    if (const auto world = cell->GetbhkWorld())
      for (auto &body : live.bodies)
        if (body && !body->GetWorld1())
          body->MoveToWorld(world);
  }
  entry.live.reset();
  RefreshCollisionState();
  Grass::Remove(id);
}
void CancelPending() {
  if (pending)
    RemoveChunks(pending->chunks);
  pending.reset();
  for (auto &work : staged)
    RemoveChunks(work.chunks);
  staged.clear();
  batch.clear();
  batchNext = 0;
  restoreAfterBatch.reset();
  pendingHistory = HistoryAction::None;
  pendingUndo.clear();
  busy.store(false);
  progress.store(0);
}
void SuppressOriginalCollision(RE::FormID id, Live &live) {
  for (const auto &body : live.bodies) {
    if (!body || !body->GetWorld1())
      continue;
    spdlog::warn("Terrain cell {:08X}: original ground body was reinserted; "
                 "removing it again.",
                 id);
    body->RemoveFromCurrentWorld();
    if (body->GetWorld1())
      spdlog::error(
          "Terrain cell {:08X}: original ground body removal did not complete.",
          id);
  }
}

Live CaptureOriginals(RE::TESObjectCELL *cell);
void BeginUnchecked(RE::FormID id, const Volume &volume) {
  if (pending)
    return;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
  if (!cell || !cell->IsAttached() || !Parent(cell) || !Land(cell) ||
      !cell->GetbhkWorld())
    return;
  const auto coordinates = cell->GetCoordinates();
  if (!coordinates || coordinates->worldX != volume.origin.x ||
      coordinates->worldY != volume.origin.y)
    return;
  Pending work;
  work.cell = id;
  work.volume = volume;
  work.parent = RE::NiPointer<RE::NiNode>(Parent(cell));
  work.land = Land(cell);
  work.collisionOwner = RE::NiPointer<RE::CellMopp>(CollisionOwner(cell));
  const auto found = cells.find(id);
  const bool live = found != cells.end() && found->second.live.has_value();
  {
    work.dirty = DirtyChunks(live ? &found->second.volume : nullptr, volume);
  }
  std::optional<Live> captured;
  if (!live)
    captured.emplace(CaptureOriginals(cell));
  const auto &source = live ? *found->second.live : *captured;
  // LAND's retained geom[] can be an unmaterialized construction template.
  // Its attached render geometry owns the live layered material. Capture the
  // actual scene leaves, including replacements installed by other mods.
  for (const auto &original : source.originals) {
    const auto geometry =
        netimmerse_cast<RE::BSTriShape *>(original.node.get());
    if (!geometry)
      continue;
    const auto property = netimmerse_cast<RE::BSLightingShaderProperty *>(
        geometry->GetGeometryRuntimeData().shaderProperty.get());
    if (!property || !property->material)
      continue;
    const auto feature = property->material->GetFeature();
    if (feature != RE::BSShaderMaterial::Feature::kMultiTexLand &&
        feature != RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend)
      continue; // Borders and auxiliary terrain geometry are not texture
                // sources.
    work.textures.push_back(CaptureTerrainTextures(geometry));
  }
  // Validate coverage now, before accepting any brush work. Do not require
  // four templates or select materials by their order in the scene graph.
  for (unsigned index = 0; index < 16; ++index) {
    const auto center = volume.origin + Vec{float(index % 4) * 1024 + 512,
                                            float(index / 4) * 1024 + 512, 0};
    if (std::none_of(work.textures.begin(), work.textures.end(),
                     [&](const auto &candidate) {
                       return candidate.surface.Contains(center);
                     }))
      throw std::runtime_error(
          std::format("Landscape material coverage missing for chunk {} ({} "
                      "rendered sources)",
                      index, work.textures.size()));
  }

  pending = std::move(work);
  busy.store(true);
  progress.store(0);
}
void Begin(RE::FormID id, const Volume &volume, bool quiet = false) {
  try {
    auto prepared = volume;
    InitializeDensity(prepared);
    BeginUnchecked(id, prepared);
  } catch (const std::exception &error) {
    if (!quiet) {
      spdlog::error("Terrain preparation for cell {:08X}: {}", id,
                    error.what());
      Notify("Terrain preparation failed. See SkyBuild log.");
    }
  }
}
Live CaptureOriginals(RE::TESObjectCELL *cell) {
  Live live;
  live.parent = RE::NiPointer<RE::NiNode>(Parent(cell));
  const auto land = Land(cell);
  live.land = land;
  live.collisionOwner = RE::NiPointer<RE::CellMopp>(CollisionOwner(cell));
  if (!land)
    throw std::runtime_error("Landscape unloaded during conversion");
  std::memcpy(live.originalHeights.data(), land->heights,
              sizeof(land->heights));
  // Native LAND height interpolation (AE 18763) adds the float at +49C0
  // to the quadrant samples. Preserve this base and all original samples.
  std::memcpy(&live.heightBase, &land->unk49C0, sizeof(live.heightBase));
  if (!std::isfinite(live.heightBase))
    throw std::runtime_error("Invalid landscape height base");
  std::vector<RE::NiAVObject *> stack;
  std::set<RE::NiAVObject *> seen;
  std::set<RE::bhkRigidBody *> bodies;
  const auto addBody = [&](RE::bhkRigidBody *body, bool cellOwned) {
    if (!body || !body->GetWorld1())
      return;
    const auto native = body->GetRigidBody();
    if (!native)
      return;
    const auto layer = native->collidable.GetCollisionLayer();
    // The cell's heightfield bodies use Ground on the tested 1.6.1170
    // runtime. Only accept that layer from this cell's authoritative owner;
    // a Ground body on an arbitrary visual node is not ownership evidence.
    if ((layer == RE::COL_LAYER::kTerrain ||
         (cellOwned && layer == RE::COL_LAYER::kGround)) &&
        bodies.insert(body).second)
      live.bodies.emplace_back(body);
  };
  // LAND's renderer nodes do not own its landscape collision. ExtraHavok's
  // CellMopp holds the cell's heightfield bodies independently of that graph.
  std::size_t cellBodies{};
  if (live.collisionOwner) {
    const auto &array = live.collisionOwner->rigidBodies;
    if (array.free_idx() > array.capacity() || array.capacity() > 4096)
      throw std::runtime_error("Invalid CellMopp rigid-body array");
    for (std::uint16_t i = 0; i < array.free_idx(); ++i) {
      if (const auto &body = array[i]; body) {
        ++cellBodies;
        addBody(body.get(), true);
      }
    }
  }
  for (auto mesh : land->mesh)
    if (mesh)
      stack.push_back(mesh);
  for (const auto &geom : land->geom)
    if (geom)
      stack.push_back(geom.get());
  if (land->border)
    stack.push_back(land->border.get());
  // Cull only landscape geometry. Keep ancestor nodes updating normally.
  while (!stack.empty()) {
    const auto node = stack.back();
    stack.pop_back();
    if (!seen.insert(node).second)
      continue;
    if (node->AsGeometry())
      live.originals.push_back(
          {RE::NiPointer<RE::NiAVObject>(node), node->GetAppCulled()});
    addBody(Body(node), false);
    if (const auto group = node->AsNode())
      for (const auto &child : group->GetChildren())
        if (child)
          stack.push_back(child.get());
  }

  if (live.originals.empty() || live.bodies.empty())
    throw std::runtime_error(
        std::format("Landscape capture found {} geometries and {} terrain "
                    "bodies (CellMopp {}, {} entries)",
                    live.originals.size(), live.bodies.size(),
                    live.collisionOwner ? "present" : "missing", cellBodies));
  return live;
}
// Process the next dirty chunk only while its original cell owners remain
// valid.
void BuildNext() {
  if (!pending)
    return;
  const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(pending->cell);
  if (Parent(cell) != pending->parent.get() || Land(cell) != pending->land ||
      CollisionOwner(cell) != pending->collisionOwner.get() ||
      !cell->GetbhkWorld()) {
    CancelPending();
    return;
  }
  try {
    auto &work = *pending;
    while (work.next < 16 && !work.dirty[work.next])
      ++work.next;
    if (work.next < 16) {
      const int index = work.next;

      auto mesh = Polygonize(work.volume, (index % 4) * 16, (index / 4) * 16);

      if (!mesh.triangles.empty()) {
        const auto center =
            work.volume.origin + Vec{float(index % 4) * 1024 + 512,
                                     float(index / 4) * 1024 + 512, 0};
        const auto texture =
            std::find_if(work.textures.begin(), work.textures.end(),
                         [&](const auto &candidate) {
                           return candidate.surface.Contains(center);
                         });
        if (texture == work.textures.end())
          throw std::runtime_error(
              "No original landscape material covers terrain chunk");
        work.chunks[index] =
            NativeChunk::Create(mesh, work.volume.origin, *texture);
      }

      if (!mesh.triangles.empty()) {
        work.surfaces[index] = std::make_shared<TerrainSurface>(
            std::move(mesh),
            work.volume.origin +
                Vec{float(index % 4) * 1024, float(index / 4) * 1024, 0});
        work.chunks[index]->StampOverlay(work.cell, unsigned(index),
                                         work.surfaces[index]);
      }
      progress.store(++work.next);
      while (work.next < 16 && !work.dirty[work.next])
        ++work.next;
      if (work.next < 16)
        return;
    }
    staged.push_back(std::move(*pending));
    pending.reset();
    if (batchNext < batch.size()) {
      const auto &[id, volume] = batch[batchNext++];
      BeginUnchecked(id, volume);
      if (!pending)
        throw std::runtime_error("Terrain batch preparation did not start");
      return;
    }
    // Capture and validate the entire batch before changing any visible state.
    std::vector<std::optional<Live>> captures;
    std::vector<LandHeights> recoveries;
    {
      for (auto &built : staged) {
        const auto target =
            RE::TESForm::LookupByID<RE::TESObjectCELL>(built.cell);
        if (Parent(target) != built.parent.get() ||
            Land(target) != built.land ||
            CollisionOwner(target) != built.collisionOwner.get() ||
            !target->GetbhkWorld())
          throw std::runtime_error(
              "An affected terrain cell unloaded during preparation");
        const auto existing = cells.find(built.cell);
        auto &original = captures.emplace_back(
            existing != cells.end() && existing->second.live
                ? std::optional<Live>{}
                : std::optional<Live>{CaptureOriginals(target)});
        const auto &source = original ? *original : *existing->second.live;
        recoveries.push_back(RecoveryHeights(
            built.volume, source.originalHeights, source.heightBase));
      }
    }
    // Any failed attachment is detached by CancelPending; originals are intact.
    for (auto &built : staged) {
      const auto target =
          RE::TESForm::LookupByID<RE::TESObjectCELL>(built.cell);
      for (auto &chunk : built.chunks)
        if (chunk)
          chunk->Attach(built.parent.get(), target->GetbhkWorld(),
                        built.volume.origin);
    }
    {
      for (std::size_t builtIndex = 0; builtIndex < staged.size();
           ++builtIndex) {
        auto &work = staged[builtIndex];
        const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(work.cell);
        auto originals = std::move(captures[builtIndex]);
        const auto &recovery = recoveries[builtIndex];
        auto &entry = cells[work.cell];
        if (!entry.live) {
          entry.live = std::move(originals);
          for (auto &original : entry.live->originals)
            original.node->SetAppCulled(true);
        }
        for (int i = 0; i < 16; ++i)
          if (work.dirty[i]) {
            Retire(entry.live->chunks[i]);
            // Discard an unpublished collider for this chunk if newer geometry
            // supersedes it. The currently active collider stays in the world.
            Retire(entry.live->nextColliders[i]);
            entry.live->collisionQueue.Queue(unsigned(i));
            collisionBusy.store(true);
            entry.live->chunks[i] = std::move(work.chunks[i]);
            entry.live->surfaces[i] = std::move(work.surfaces[i]);
          }
        entry.volume = std::move(work.volume);
        // During collider lag, keep a conservative recovery envelope: raising
        // it early could fight the previous (lower) collider every physics
        // tick.
        for (std::size_t i = 0; i < recovery.size(); ++i)
          entry.live->land->heights[i / 289][i % 289] = std::min(
              entry.live->land->heights[i / 289][i % 289], recovery[i]);

        entry.failed = false;
        Grass::Publish(cell, entry.live->surfaces, entry.volume);
      }
    }
    if (pendingHistory == HistoryAction::Undo)
      redoHistory.push_back(std::move(pendingUndo));
    else if (pendingHistory == HistoryAction::Redo) {
      if (!redoHistory.empty())
        redoHistory.pop_back();
    } else if (pendingHistory == HistoryAction::Edit ||
               pendingHistory == HistoryAction::Restore) {
      redoHistory.clear();
      Grass::ClearRedo();
    }
    pendingHistory = HistoryAction::None;
    pendingUndo.clear();
    staged.clear();
    batch.clear();
    batchNext = 0;
    if (restoreAfterBatch) {
      const auto id = *restoreAfterBatch;
      restoreAfterBatch.reset();
      if (auto found = cells.find(id); found != cells.end()) {
        Release(id, found->second);
        cells.erase(found);
        RemoveLoadRecovery(id);
      }
    }
    busy.store(false);
  } catch (const std::exception &error) {
    heldBrush.store(0);
    tappedBrush.store(0);
    spdlog::error("Terrain conversion: {}", error.what());
    Notify("Terrain conversion failed; previous terrain retained. See SkyBuild "
           "log.");
    if (pending)
      if (const auto found = cells.find(pending->cell); found != cells.end())
        found->second.failed = true;
    CancelPending();
  }
}
// Keep scene/Havok mutation on the existing game-thread update. Each update
// performs one native collider build; visual publication is a separate stage.
void BuildNextCollision() {
  if (!CollisionPending())
    return;
  for (auto &[id, entry] : cells) {
    if (!entry.live || !entry.live->collisionQueue.Pending())
      continue;
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    auto &live = *entry.live;
    if (Parent(cell) != live.parent.get() || Land(cell) != live.land ||
        CollisionOwner(cell) != live.collisionOwner.get() ||
        !cell->GetbhkWorld()) {
      Release(id, entry);
      return;
    }
    try {
      const auto index = unsigned(live.collisionQueue.Next());
      if (live.surfaces[index])
        live.nextColliders[index] = NativeChunk::CreateCollision(
            live.surfaces[index]->Geometry(), entry.volume.origin,
            live.bodies.front().get());
      live.collisionQueue.Complete(index);
      if (live.collisionQueue.Pending())
        return;
      // Attach every replacement before removing any current collider. If
      // attachment fails, Release restores original terrain and world bodies.
      for (unsigned i = 0; i < 16; ++i)
        if (live.collisionQueue.changed[i] && live.nextColliders[i])
          live.nextColliders[i]->Attach(live.parent.get(), cell->GetbhkWorld(),
                                        entry.volume.origin);
      const auto recovery =
          RecoveryHeights(entry.volume, live.originalHeights, live.heightBase);
      for (unsigned i = 0; i < 16; ++i)
        if (live.collisionQueue.changed[i]) {
          Retire(live.colliders[i]);
          live.colliders[i] = std::move(live.nextColliders[i]);
        }
      for (auto &body : live.bodies)
        if (body && body->GetWorld1())
          body->RemoveFromCurrentWorld();
      std::memcpy(live.land->heights, recovery.data(),
                  sizeof(live.land->heights));
      live.collisionReady = true;
      PublishLoadRecovery(id, entry.volume);
      live.collisionQueue.Published();
      RefreshCollisionState();
    } catch (const std::exception &error) {
      spdlog::error("Terrain collision pipeline failed for {:08X}: {}", id,
                    error.what());
      heldBrush.store(0);
      tappedBrush.store(0);
      CancelPending();
      Release(id, entry);
      entry.failed = true;
      Notify("Terrain collision failed; original terrain restored.");
    }
    return;
  }
}
void StartBatch(std::vector<std::pair<RE::FormID, Volume>> jobs) {
  if (jobs.empty())
    return;

  for (auto &[id, volume] : jobs)
    InitializeDensity(volume);
  std::size_t memory{};
  for (const auto &[id, entry] : cells) {
    const auto next =
        std::find_if(jobs.begin(), jobs.end(),
                     [&](const auto &job) { return job.first == id; });
    memory += DensityMemory(next == jobs.end() ? entry.volume : next->second);
  }
  for (const auto &[id, volume] : jobs)
    if (!cells.contains(id))
      memory += DensityMemory(volume);
  if (memory > MaxDensitySaveBytes)
    throw std::runtime_error(
        "Terrain density memory limit reached; undo or restore terrain");
  // Preflight every participant before allocating replacements or publishing.
  for (const auto &[id, volume] : jobs) {
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (!Parent(cell) || !Land(cell) || !cell->GetbhkWorld() || !volume.Valid())
      throw std::runtime_error("All affected terrain cells must be loaded");
    if (const auto found = cells.find(id);
        found == cells.end() || !found->second.live)
      (void)CaptureOriginals(cell);
  }
  batch = std::move(jobs);
  batchNext = 1;
  try {
    BeginUnchecked(batch.front().first, batch.front().second);
    if (!pending)
      throw std::runtime_error("Terrain batch preparation did not start");
  } catch (...) {
    CancelPending();
    throw;
  }
}
Volume SampleLandscape(RE::TESObjectCELL *cell) {
  Volume volume;
  const auto coordinates = cell->GetCoordinates();
  const auto tes = RE::TES::GetSingleton();
  if (!coordinates || !tes)
    throw std::runtime_error("Landscape coordinates unavailable");
  volume.origin = {coordinates->worldX, coordinates->worldY, 0};
  auto halo = std::make_shared<DensityHalo>();
  for (int y = -1; y <= Side; ++y)
    for (int x = -1; x <= Side; ++x) {
      const float wx = volume.origin.x + x * Step,
                  wy = volume.origin.y + y * Step;
      std::optional<float> original;
      // Converted neighboring LAND contains the recovery envelope. Sample its
      // retained original landscape, never that lowered envelope, at shared
      // edges.
      for (const auto &[id, known] : cells) {
        const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
        const auto p = known.volume.origin;
        if (owner &&
            owner->GetRuntimeData().worldSpace ==
                cell->GetRuntimeData().worldSpace &&
            wx >= p.x && wy >= p.y && wx <= p.x + CellSize &&
            wy <= p.y + CellSize) {
          original = known.volume.Height(wx, wy);
          break;
        }
      }
      float height{};
      if (original)
        height = *original;
      else if (!tes->GetLandHeight({wx, wy, 0}, height))
        throw std::runtime_error(
            "Landscape sampling failed; load neighboring cells and retry");
      if (!std::isfinite(height))
        throw std::runtime_error("Invalid landscape height");
      (*halo)[(y + 1) * DensitySide + x + 1] = height;
      if (x >= 0 && x < Side && y >= 0 && y < Side)
        volume.heights[y * Side + x] = height;
    }
  volume.densityHeights = halo;
  volume.SetBounds();
  InitializeDensity(volume);
  return volume;
}
// The overlay only snapshots terrain. It never converts a cell or touches
// materials/collision merely because the shovel is aimed at it.
void UpdateBrushOverlay(RE::PlayerCharacter *player) {
  if (!Ready() || selectedTool.load() == Tool::Idle || UI::IsToolWheelOpen() ||
      UI::Native::IsAnyBlockingWindowOpened()) {
    BrushOverlay::Clear();
    return;
  }
  const auto current = player ? player->GetParentCell() : nullptr;
  const auto tes = RE::TES::GetSingleton();
  if (!current || current->IsInteriorCell() || !tes) {
    BrushOverlay::Clear();
    return;
  }
  const auto hits =
      Physics::CastCameraRay(Reach, RE::COL_LAYER::kProjectile, false);
  if (hits.empty() || !hits.front().didHit) {
    BrushOverlay::Clear();
    return;
  }
  const auto &p = hits.front().position;
  const Brush brush{{p.x, p.y, p.z}, brushRadius.load()};
  if (!Finite(brush.center)) {
    BrushOverlay::Clear();
    return;
  }
  BrushOverlay::Begin(brush);
  for (const auto &[x, y] : BrushCells(brush, 0)) {
    const auto cell =
        tes->GetCell({(x + .5F) * CellSize, (y + .5F) * CellSize, p.z});
    if (!cell || !cell->IsAttached() ||
        cell->GetRuntimeData().worldSpace !=
            current->GetRuntimeData().worldSpace)
      continue;
    const auto found = cells.find(cell->GetFormID());
    if (found != cells.end() && found->second.live) {
      const auto &entry = found->second;
      for (unsigned i = 0; i < entry.live->surfaces.size(); ++i) {
        const Vec origin = entry.volume.origin +
                           Vec{float(i % 4) * 1024, float(i / 4) * 1024, 0};
        const float dx = brush.center.x -
                         std::clamp(brush.center.x, origin.x, origin.x + 1024);
        const float dy = brush.center.y -
                         std::clamp(brush.center.y, origin.y, origin.y + 1024);
        if (dx * dx + dy * dy <= brush.radius * brush.radius)
          BrushOverlay::ExpectChunk((std::uint64_t(cell->GetFormID()) << 32) |
                                    i);
      }
    } else if (const auto land = Land(cell)) {
      // mesh contains the actual terrain render shapes; no border/grass meshes.
      for (auto *mesh : land->mesh)
        if (mesh)
          BrushOverlay::AddOriginal(mesh);
    }
  }
  BrushOverlay::Publish();
}
bool BrushAction(std::optional<bool> add, bool level = false,
                 bool smooth = false) {
  try {
    const auto current = CurrentCell();
    const auto tes = RE::TES::GetSingleton();
    if (!current || current->IsInteriorCell() || !tes)
      throw std::runtime_error(
          "Terrain tool needs a loaded exterior landscape");
    const auto hits =
        Physics::CastCameraRay(Reach, RE::COL_LAYER::kProjectile, false);
    if (hits.empty() || !hits.front().didHit)
      return false;
    const auto &hit = hits.front();
    if (!std::isfinite(hit.position.x) || !std::isfinite(hit.position.y) ||
        !std::isfinite(hit.position.z) || std::abs(hit.position.x) > 16777216 ||
        std::abs(hit.position.y) > 16777216)
      throw std::runtime_error("Invalid terrain brush position");
    Brush brush{{hit.position.x, hit.position.y, hit.position.z},
                brushRadius.load(),
                add.value_or(false),
                add ? editingStrength : 0.F,
                add ? editingStroke : 0};
    if (add && selectedTool.load() == Tool::Grass) {
      const auto world = current->GetRuntimeData().worldSpace;
      if (!world)
        return false;
      const bool changed = Grass::Paint(world->GetFormID(), brush, *add);
      if (changed)
        redoHistory.clear();
      return changed;
    }
    std::vector<std::pair<RE::FormID, Volume>> jobs;
    std::size_t newCells{};
    std::vector<RE::TESObjectCELL *> participants;
    bool owned = false;
    float support = Step;
    for (const auto &[id, entry] : cells) {
      const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
      if (owner &&
          owner->GetRuntimeData().worldSpace ==
              current->GetRuntimeData().worldSpace &&
          entry.volume.density)
        support = std::max(support, entry.volume.density->bound);
    }
    for (const auto &[x, y] : BrushCells(brush, support + Step + 1)) {
      const auto cell = tes->GetCell(
          {(x + .5F) * CellSize, (y + .5F) * CellSize, hit.position.z});
      if (!cell || cell->IsInteriorCell() || !Parent(cell) || !Land(cell) ||
          cell->GetRuntimeData().worldSpace !=
              current->GetRuntimeData().worldSpace)
        throw std::runtime_error(
            "All cells touched by the brush must be loaded");
      const auto found = cells.find(cell->GetFormID());
      const bool live = found != cells.end() && found->second.live;
      if (live) {
        for (const auto &chunk : found->second.live->colliders)
          if (const auto body = chunk ? chunk->Body() : nullptr;
              body && body->GetRigidBody() &&
              &body->GetRigidBody()->collidable == hit.collidable)
            owned = true;
      }
      if (!live || !found->second.live->collisionReady) {
        const auto native = CaptureOriginals(cell);
        for (const auto &body : native.bodies)
          if (body->GetRigidBody() &&
              &body->GetRigidBody()->collidable == hit.collidable)
            owned = true;
      }
      participants.push_back(cell);
    }
    if (!owned)
      return false;
    if (level)
      brush.center.z = levelTarget.Sample(editingStroke, hit.position.z);

    DensitySnapshot smoothing;
    std::map<RE::FormID, Volume> smoothVolumes;
    if (smooth || (add && !level))
      for (const auto cell : participants) {
        const auto found = cells.find(cell->GetFormID());
        auto volume =
            found != cells.end() ? found->second.volume : SampleLandscape(cell);
        InitializeDensity(volume);
        smoothing.volumes.push_back(volume);
        smoothVolumes.emplace(cell->GetFormID(), std::move(volume));
      }
    for (const auto cell : participants) {
      const auto found = cells.find(cell->GetFormID());
      const bool live = found != cells.end() && found->second.live;
      if (!add && live)
        continue;
      auto volume = (smooth || (add && !level))
                        ? smoothVolumes.at(cell->GetFormID())
                    : found != cells.end() ? found->second.volume
                                           : SampleLandscape(cell);
      if (add) {
        if (!(smooth ? ApplyDensitySmooth(
                           volume, brush,
                           [&](Vec world) { return smoothing.Sample(world); })
              : level ? ApplyDensityLevel(volume, brush)
                      : ApplyDensityBrush(volume, brush, [&](Vec world) {
                          return smoothing.CanonicalSample(world);
                        })))
          continue;
      }
      if (found == cells.end())
        ++newCells;
      jobs.emplace_back(cell->GetFormID(), std::move(volume));
    }
    if (cells.size() + newCells > MaxCells)
      throw std::runtime_error("Terrain cell limit reached (64)");
    if (jobs.empty()) {
      if (!add)
        Notify("Terrain in the brush radius is already editable.");
      return false;
    }

    StartBatch(std::move(jobs));
    if (add) {
      pendingHistory = HistoryAction::Edit;
      Grass::BrushArea(brush);
    }
    return true;
  } catch (const std::exception &error) {
    heldBrush.store(0);
    spdlog::warn("Terrain brush preparation: {}", error.what());
    Notify(error.what());
    return false;
  }
}
void Convert() { BrushAction({}); }
bool Edit(BrushMode mode) {
  if (selectedTool.load() == Tool::Grass)
    return BrushAction(mode == BrushMode::Add);
  return editingStrength > 0 &&
         BrushAction(mode == BrushMode::Add, mode == BrushMode::Level,
                     mode == BrushMode::Smooth);
}
void UndoOrRestore(RE::TESObjectCELL *cell, bool restore) {
  const auto found = cells.find(cell->GetFormID());
  if (restore && found == cells.end())
    return;
  std::optional<Brush> selected;
  if (!restore) {
    // Undo the latest stroke in this worldspace, even when the player stood
    // outside the cell being edited or crossed a cell boundary while holding.
    for (const auto &[id, entry] : cells) {
      const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
      if (!owner ||
          owner->GetRuntimeData().worldSpace !=
              cell->GetRuntimeData().worldSpace ||
          entry.volume.edits.empty())
        continue;
      const auto &last = entry.volume.edits.back();
      if (!selected || last.stroke > selected->stroke ||
          (!last.stroke && !selected->stroke && id == cell->GetFormID()))
        selected = last;
    }
    if (!selected)
      return;
  }
  const auto restoreBrushes =
      restore ? found->second.volume.edits : std::vector<Brush>{};
  std::vector<std::pair<RE::FormID, Volume>> jobs;
  StrokeRecord record;
  for (const auto &[id, entry] : cells) {
    const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (!owner ||
        owner->GetRuntimeData().worldSpace != cell->GetRuntimeData().worldSpace)
      continue;
    auto volume = entry.volume;
    bool changed = restore && id == cell->GetFormID();
    if (restore) {
      for (const auto &brush : restoreBrushes) {
        const auto removed = RemoveStroke(volume, brush);
        changed |= !removed.empty();
      }
    } else {
      auto removed = RemoveStroke(volume, *selected);
      changed = !removed.empty();
      if (changed)
        record.emplace(id, std::move(removed));
    }
    if (changed) {
      volume.SetBounds();
      jobs.emplace_back(id, std::move(volume));
    }
  }
  try {
    StartBatch(std::move(jobs));
    if (restore) {
      restoreAfterBatch = cell->GetFormID();
      pendingHistory = HistoryAction::Restore;
    } else {
      pendingUndo = std::move(record);
      pendingHistory = HistoryAction::Undo;
    }
  } catch (const std::exception &error) {
    spdlog::warn("Terrain undo/restore: {}", error.what());
    Notify(error.what());
  }
}
void Redo(RE::TESObjectCELL *cell) {
  if (redoHistory.empty())
    return;
  try {
    std::vector<std::pair<RE::FormID, Volume>> jobs;
    for (const auto &[id, removed] : redoHistory.back()) {
      const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
      const auto found = cells.find(id);
      if (!owner || found == cells.end() ||
          owner->GetRuntimeData().worldSpace !=
              cell->GetRuntimeData().worldSpace)
        throw std::runtime_error(
            "Return to the edited worldspace to redo terrain");
      auto volume = found->second.volume;
      RestoreStroke(volume, removed);
      volume.SetBounds();
      jobs.emplace_back(id, std::move(volume));
    }
    StartBatch(std::move(jobs));
    pendingHistory = HistoryAction::Redo;
  } catch (const std::exception &error) {
    spdlog::warn("Terrain redo: {}", error.what());
    Notify(error.what());
  }
}
void ExecuteCommand(Command command) {
  if (command == Command::None)
    return;

  if (busy.load() || CollisionPending())
    return;
  if (command == Command::Convert)
    Convert();
  else if (const auto cell = CurrentCell()) {
    const auto world = cell->GetRuntimeData().worldSpace;
    const auto worldID = world ? world->GetFormID() : 0;
    if (command == Command::Undo) {
      std::uint64_t terrainStroke{};
      for (const auto &[id, entry] : cells) {
        const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
        if (owner && owner->GetRuntimeData().worldSpace == world &&
            !entry.volume.edits.empty())
          terrainStroke =
              std::max(terrainStroke, entry.volume.edits.back().stroke);
      }
      if (Grass::UndoStroke(worldID) > terrainStroke) {
        Grass::History(worldID, false);
        return;
      }
    } else if (command == Command::Redo) {
      std::uint64_t terrainStroke{};
      if (!redoHistory.empty())
        for (const auto &[id, removed] : redoHistory.back())
          for (const auto &part : removed)
            terrainStroke = std::max(terrainStroke, part.brush.stroke);
      const auto grassStroke = Grass::RedoStroke(worldID);
      if (grassStroke && (!terrainStroke || grassStroke < terrainStroke)) {
        Grass::History(worldID, true);
        return;
      }
    }
    if (command == Command::Redo)
      Redo(cell);
    else if (command == Command::Restore || command == Command::Undo)
      UndoOrRestore(cell, command == Command::Restore);
  }
}
} // namespace
std::vector<NavigationCell> CaptureNavigation(std::uint32_t worldspaceID) {
  std::vector<NavigationCell> result;
  for (const auto &[id, entry] : cells) {
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    const auto world = cell ? cell->GetRuntimeData().worldSpace : nullptr;
    if (entry.live && cell && cell->IsAttached() && world &&
        world->GetFormID() == worldspaceID && HasTerrainChanges(entry.volume))
      result.push_back({id, entry.volume, entry.live->surfaces});
  }
  return result;
}
bool NavigationBusy() {
  if (pending)
    return true;
  for (const auto &[id, entry] : cells) {
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (!entry.live && !entry.failed && Parent(cell) && Land(cell))
      return true;
  }
  return false;
}
void SetStrengthPerSecond(float strength) {
  if (std::isfinite(strength))
    strengthPerSecond.store(std::clamp(strength, MinBrushStrengthPerSecond,
                                       MaxBrushStrengthPerSecond));
}
void Initialize() {
  InstallLoadRecoveryHook();
  InitializeTerrainWeldingPolicy();
  Grass::Initialize();
}
void SetSceneLoaded(bool loaded) {
  if (!loaded)
    loadRecovery.store(nullptr);
  sceneLoaded = loaded;
  // PostLoadGame can precede Havok world admission. Restore from Update after
  // the cell is attached; publication here can fail and disable saved terrain.
  if (!loaded) {
    BrushOverlay::Clear();
    queuedCommand.store(Command::None);
    heldBrush.store(0);
    tappedBrush.store(0);
    ready.store(false);
    CancelPending();
    for (auto &[id, cell] : cells) {
      CheckpointDensity(cell.volume);
      Release(id, cell);
    }
    Grass::Clear();
  }
}
bool Ready() { return ready.load(); }
bool Enabled() { return modeEnabled.load(); }
void SetEnabled(bool enabled) {
  modeEnabled.store(enabled);
  SuspendInput();
  const auto player = RE::PlayerCharacter::GetSingleton();
  ready.store(enabled && BuildingMode::IsHammerReady() && sceneLoaded &&
              player && player->Is3DLoaded());
  UI::RefreshPresentation();
}
void Update(RE::PlayerCharacter *player) {
  {
    std::erase_if(retired,
                  [](RetiredChunk &entry) { return --entry.frames == 0; });
  }
  ready.store(sceneLoaded && modeEnabled.load() &&
              BuildingMode::IsHammerReady() && player && player->Is3DLoaded() &&
              !UI::IsGameMenuBlocking(true));
  const auto grassCell =
      sceneLoaded && player ? player->GetParentCell() : nullptr;
  const auto grassWorld = grassCell && !grassCell->IsInteriorCell()
                              ? grassCell->GetRuntimeData().worldSpace
                              : nullptr;
  Grass::SetWorld(grassWorld ? grassWorld->GetFormID() : 0);
  UI::RefreshPresentation();
  const auto now = BrushRepeat::Clock::now();
  constexpr auto interval = std::chrono::milliseconds(0);
  if (!Ready()) {
    heldBrush.store(0);
    tappedBrush.store(0);
    brushRepeat.Due(now, 0, true);
    queuedCommand.store(Command::None);
  } else {
    if (!busy.load() && !CollisionPending())
      ExecuteCommand(queuedCommand.exchange(Command::None));
    if (!busy.load() && !CollisionPending() &&
        queuedCommand.load() == Command::None) {
      const auto tapped = tappedBrush.exchange(0);
      const auto held = heldBrush.load();
      if (tapped) {
        editingStroke = DecodeBrushStroke(tapped);
        const auto pressed = BrushStrengthClock::Clock::time_point(
            BrushStrengthClock::Clock::duration(strokePressed.load()));
        const auto released = BrushStrengthClock::Clock::time_point(
            BrushStrengthClock::Clock::duration(strokeReleased.load()));
        const auto pulseTime =
            released >= pressed && released < now ? released : now;
        editingStrength = strengthClock.Amount(
            pulseTime, editingStroke, pressed, strengthPerSecond.load());
        if (Edit(DecodeBrushMode(tapped)))
          strengthClock.Applied(pulseTime, editingStroke);
        brushRepeat.Started(now, editingStroke, interval);
      } else if (brushRepeat.Due(now, DecodeBrushStroke(held), false,
                                 interval)) {
        editingStroke = DecodeBrushStroke(held);
        const auto pressed = BrushStrengthClock::Clock::time_point(
            BrushStrengthClock::Clock::duration(strokePressed.load()));
        editingStrength = strengthClock.Amount(now, editingStroke, pressed,
                                               strengthPerSecond.load());
        if (Edit(DecodeBrushMode(held)))
          strengthClock.Applied(now, editingStroke);
      }
    }
  }
  if (!sceneLoaded)
    return;
  // A queued edit may finish after unequipping; it belongs to the cell, not the
  // item.
  if (!UI::IsGameMenuBlocking(true)) {
    const bool visualStage = busy.load();
    if (visualStage) {
      // Several cheap visual chunks can fit into one update. This is a soft
      // budget: an individual chunk or atomic publication is never interrupted.
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
      do {
        BuildNext();
      } while (pending && busy.load() &&
               std::chrono::steady_clock::now() < deadline);
    }
    // Never combine the final visual build and a native MOPP build in one
    // update. The next brush pulse waits for all affected cells to catch up.
    if (!visualStage && !busy.load())
      BuildNextCollision();
  }
  {
    for (auto &[id, entry] : cells) {
      const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
      if (entry.live &&
          (Parent(cell) != entry.live->parent.get() ||
           Land(cell) != entry.live->land ||
           CollisionOwner(cell) != entry.live->collisionOwner.get()))
        Release(id, entry);
      if (entry.live && entry.live->collisionReady)
        SuppressOriginalCollision(id, *entry.live);
      if (!entry.live && !entry.failed && !pending && cell &&
          cell->IsAttached() && Parent(cell) && Land(cell) &&
          std::chrono::steady_clock::now() >= entry.nextPreparation) {
        entry.nextPreparation =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        Begin(id, entry.volume, true);
      }
    }
  }
  if (!busy.load()) {
    const auto held = DecodeBrushStroke(heldBrush.load());
    for (auto &[id, entry] : cells)
      if (entry.volume.density && (!held || entry.volume.edits.empty() ||
                                   entry.volume.edits.back().stroke != held))
        CheckpointDensity(entry.volume);
  }
  UpdateBrushOverlay(player);
  Grass::FinishBrush(DecodeBrushStroke(heldBrush.load()), busy.load());
  Grass::Update();
  const auto current = player ? player->GetParentCell() : nullptr;
  converted.store(current && cells.contains(current->GetFormID()) &&
                  cells.at(current->GetFormID()).live.has_value());
}
float BrushRadius() { return brushRadius.load(); }
bool HandleButton(RE::ButtonEvent *button) {
  if (!button)
    return false;
  const auto device = button->GetDevice();
  const auto key = button->GetIDCode();
  const auto matches = [&](const ControlBindings::KeyboardBinding &binding) {
    return device == binding.device && key == binding.code;
  };
  if (!Ready())
    return false;
  if (selectedTool.load() == Tool::Idle &&
      !matches(ControlBindings::switchMode) && !UI::IsToolWheelOpen())
    return false;
  if (matches(ControlBindings::switchMode)) {
    heldBrush.store(0);
    tappedBrush.store(0);
    if (button->IsDown())
      UI::OpenToolWheel();
    else if (button->IsUp())
      UI::FinishToolWheel();
    return true;
  }
  if (UI::IsToolWheelOpen()) {
    if (device == RE::INPUT_DEVICE::kKeyboard &&
        key == RE::BSKeyboardDevice::Key::kEscape && button->IsDown())
      UI::CancelToolWheel();
    return true;
  }

  if (matches(ControlBindings::historyUndo) ||
      matches(ControlBindings::historyRedo)) {
    if (button->IsDown()) {
      UI::CancelToolWheel();
      heldBrush.store(0);
      tappedBrush.store(0);
      queuedCommand.store(matches(ControlBindings::historyUndo)
                              ? Command::Undo
                              : Command::Redo);
    }
    return true;
  }
  if ((matches(ControlBindings::brushIncrease) ||
       matches(ControlBindings::brushDecrease)) &&
      selectedTool.load() != Tool::Idle) {
    if (button->IsDown())
      brushRadius.store(std::clamp(
          brushRadius.load() +
              (matches(ControlBindings::brushIncrease) ? 16.F : -16.F),
          16.F, 512.F));
    return true;
  }
  const bool primary = matches(ControlBindings::placeAction);
  const bool secondary = matches(ControlBindings::secondaryAction);
  if (primary || secondary) {
    const auto token = key | (static_cast<std::uint32_t>(device) << 16);
    if (!button->IsPressed()) {
      if (heldButton.load() == token) {
        strokeReleased.store(
            BrushStrengthClock::Clock::now().time_since_epoch().count());
        heldBrush.store(0);
      }
    } else if (button->IsDown() && selectedTool.load() != Tool::Idle &&
               (primary || selectedTool.load() == Tool::Sculpt ||
                selectedTool.load() == Tool::Grass)) {
      const auto stroke = nextStroke.fetch_add(1);
      strokePressed.store(
          BrushStrengthClock::Clock::now().time_since_epoch().count());
      strokeReleased.store(0);
      heldButton.store(token);
      const auto mode = selectedTool.load() == Tool::Smooth  ? BrushMode::Smooth
                        : selectedTool.load() == Tool::Level ? BrushMode::Level
                        : selectedTool.load() == Tool::Sculpt
                            ? (primary ? BrushMode::Add : BrushMode::Subtract)
                        : secondary && !primary ? BrushMode::Add
                                                : BrushMode::Subtract;
      const auto state = EncodeBrushState(stroke, mode);
      heldBrush.store(state);
      if (!busy.load() && !CollisionPending())
        tappedBrush.store(state);
    }
    return true;
  }
  return false;
}
bool CanUndo() {
  if (!Ready() || busy.load() || CollisionPending() || heldBrush.load())
    return false;
  const auto cell = CurrentCell();
  if (!cell)
    return false;
  const auto world = cell->GetRuntimeData().worldSpace;
  if (Grass::UndoStroke(world ? world->GetFormID() : 0))
    return true;
  for (const auto &[id, entry] : cells) {
    const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    if (owner && owner->GetRuntimeData().worldSpace == world &&
        !entry.volume.edits.empty())
      return true;
  }
  return false;
}
bool CanRedo() {
  if (!Ready() || busy.load() || CollisionPending() || heldBrush.load())
    return false;
  const auto cell = CurrentCell();
  if (!cell)
    return false;
  const auto world = cell->GetRuntimeData().worldSpace;
  if (Grass::RedoStroke(world ? world->GetFormID() : 0))
    return true;
  if (!redoHistory.empty())
    for (const auto &[id, removed] : redoHistory.back()) {
      const auto owner = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
      if (owner && owner->GetRuntimeData().worldSpace == world &&
          !removed.empty())
        return true;
    }
  return false;
}
Tool GetTool() { return selectedTool.load(); }
void SuspendInput() {
  heldBrush.store(0);
  tappedBrush.store(0);
  heldButton.store(0);
  brushRepeat = {};
  strengthClock = {};
  queuedCommand.store(Command::None);
  Grass::FinishBrush(0, busy.load());
}
int BuildProgress() {
  return busy.load() ? progress.load() : CollisionPending() ? 16 : -1;
}
void SelectWheelAction(int sector) {
  if (!Ready())
    return;
  heldBrush.store(0);
  tappedBrush.store(0);
  if (sector == 0)
    selectedTool.store(Tool::Sculpt);
  else if (sector == 1)
    selectedTool.store(Tool::Level);
  else if (sector == 2)
    selectedTool.store(Tool::Smooth);
  else if (sector == 3)
    selectedTool.store(Tool::Idle);
  else if (sector == 4)
    selectedTool.store(Tool::Grass);
  UI::RefreshPresentation();
}
void Reset(bool preserveGrass) {
  loadRecovery.store(nullptr);
  BrushOverlay::Clear();
  selectedTool.store(Tool::Sculpt);
  levelTarget = {};
  tappedBrush.store(0);
  heldBrush.store(0);
  brushRepeat = {};
  strengthClock = {};
  redoHistory.clear();
  queuedCommand.store(Command::None);
  CancelPending();
  for (auto &[id, cell] : cells)
    Release(id, cell);
  loadRecovery.store(nullptr);
  cells.clear();
  Grass::Clear();
  if (!preserveGrass)
    Grass::ResetPersistent();
  converted.store(false);
}
void Save(SKSE::SerializationInterface *s) {
  if (!s)
    return;
  try {
    // Save the last published state, including a completed prefix of a held
    // stroke. Unpublished batch work never enters a checkpoint or co-save.
    std::vector<std::pair<RE::FormID, std::vector<std::byte>>> payloads;
    std::size_t bytes = sizeof(std::uint32_t);
    for (auto &[id, cell] : cells) {
      InitializeDensity(cell.volume);
      CheckpointDensity(cell.volume);
      auto payload = EncodeDensity(cell.volume);
      bytes += sizeof(id) + sizeof(std::uint32_t) + payload.size();
      if (bytes > MaxDensitySaveBytes)
        throw std::runtime_error("Terrain density save limit reached");
      payloads.emplace_back(id, std::move(payload));
    }
    if (!s->OpenRecord(SaveRecord, 3))
      return;
    const auto count = std::uint32_t(payloads.size());
    if (!s->WriteRecordData(&count, sizeof(count)))
      return;
    for (const auto &[id, payload] : payloads) {
      const auto size = std::uint32_t(payload.size());
      if (!s->WriteRecordData(&id, sizeof(id)) ||
          !s->WriteRecordData(&size, sizeof(size)) ||
          !s->WriteRecordData(payload.data(), size))
        return;
    }
  } catch (const std::exception &error) {
    spdlog::error("Terrain density save failed: {}", error.what());
    Notify("Terrain density could not be saved. Check SkyBuild log.");
  }
}
void Load(SKSE::SerializationInterface *s, std::uint32_t version,
          std::uint32_t length) {
  if (!s || (version != 1 && version != 2 && version != 3) ||
      length > MaxDensitySaveBytes ||
      (version < 3 && length > 16 * 1024 * 1024))
    return;
  const auto get = [&](auto &v) {
    if (length < sizeof(v))
      return false;
    if (s->ReadRecordData(&v, sizeof(v)) != sizeof(v))
      return false;
    length -= static_cast<std::uint32_t>(sizeof(v));
    return true;
  };
  std::uint32_t count{};
  if (!get(count) || count > MaxCells)
    return;
  std::map<RE::FormID, Cell> loaded;
  std::size_t memory{};
  for (std::uint32_t i = 0; i < count; ++i) {
    RE::FormID id{}, resolved{};
    Volume v;
    if (version == 3) {
      std::uint32_t size{};
      if (!get(id) || !get(size) || size > length || size > MaxDensitySaveBytes)
        return;
      std::vector<std::byte> payload(size);
      if (s->ReadRecordData(payload.data(), size) != size ||
          !DecodeDensity(payload, v))
        return;
      length -= size;
      memory += DensityMemory(v);
      if (memory > MaxDensitySaveBytes)
        return;
      if (s->ResolveFormID(id, resolved)) {
        if (loaded.contains(resolved))
          return;
        loaded.emplace(resolved, Cell{std::move(v), {}, false});
      }
      continue;
    }

    std::uint32_t edits{};
    if (!get(id) || !get(v.origin) || !get(v.bottom) || !get(v.top) ||
        !get(v.heights) || !get(edits) || edits > MaxEdits)
      return;
    for (std::uint32_t e = 0; e < edits; ++e) {
      Brush brush;
      std::uint8_t add{};
      if (!get(brush.center) || !get(brush.radius) || !get(add) || add > 1)
        return;
      if (version >= 2 && (!get(brush.strength) || !get(brush.stroke)))
        return;
      if (brush.stroke >= (std::uint64_t(1) << 62) - 1)
        return;
      brush.add = add != 0;
      v.edits.push_back(brush);
    }
    if (!v.Valid())
      return;
    try {
      InitializeDensity(v);
      auto cache = std::make_shared<DensityCache>(*v.density);
      cache->baseline = cache->current;
      cache->strokes.clear();
      v.edits.clear();
      v.density = std::move(cache);
    } catch (const std::exception &error) {
      spdlog::warn("Terrain legacy density upgrade failed: {}", error.what());
      return;
    }
    memory += DensityMemory(v);
    if (memory > MaxDensitySaveBytes)
      return;
    if (s->ResolveFormID(id, resolved))
      loaded.emplace(resolved, Cell{std::move(v), {}, false});
  }
  if (length != 0)
    return;
  Reset(true);
  cells = std::move(loaded);
  std::shared_ptr<const RecoverySnapshot> recovery;
  for (const auto &[id, entry] : cells) {
    const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
    const auto world = cell ? cell->GetRuntimeData().worldSpace : nullptr;
    if (world && HasTerrainChanges(entry.volume))
      recovery =
          WithRecoveryCell(recovery, id, world->GetFormID(), entry.volume);
  }
  loadRecovery.store(recovery);
  for (const auto &[id, entry] : cells)
    for (const auto &brush : entry.volume.edits)
      nextStroke.store(std::max(nextStroke.load(), brush.stroke + 1));
}
} // namespace SkyBuild::Terrain
