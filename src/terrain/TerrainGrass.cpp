#include "terrain/TerrainGrass.h"
#include "terrain/GrassRegistrationRefresh.h"
#include "terrain/LegacyGrassCapture.h"
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <intrin.h>
#include <map>
#include <mutex>
#include <unordered_map>

namespace SkyBuild::Terrain::Grass {
namespace {
using Buffer = RE::BSGraphics::VertexBuffer;
using CreateFn = Buffer *(*)(void *, std::uint32_t, const void *,
                             std::uint64_t);
using FreeFn = void (*)(void *, Buffer *);
CreateFn createOriginal{};
FreeFn freeOriginal{};
using AddFn = std::uint32_t (*)(RE::BSMultiStreamInstanceTriShape *,
                                std::uint32_t, std::uint16_t *, std::uint32_t,
                                float);
AddFn addOriginal{};
bool registrationRefreshInstalled{};
std::uint32_t Add(RE::BSMultiStreamInstanceTriShape *shape, std::uint32_t count,
                  std::uint16_t *data, std::uint32_t stride, float padding) {
  if (const auto existing = RegistrationRefresh::Reuse(shape))
    return *existing;
  return addOriginal(shape, count, data, stride, padding);
}
struct Notifications {
  std::vector<std::pair<RE::BSMultiStreamInstanceTriShape *, std::uint32_t>>
      groups;
  ~Notifications() {
    if (groups.empty())
      return;
    const auto communityShaders = GetModuleHandleW(L"CommunityShaders.dll");
    if (!communityShaders)
      return;
    for (const auto &[shape, index] : groups) {
      const auto vtable = *reinterpret_cast<void ***>(shape);
      HMODULE provider{};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(vtable[0x3C]),
                              &provider) ||
          provider != communityShaders)
        continue;
      if (!registrationRefreshInstalled)
        continue;
      RegistrationRefresh refresh(shape, index);
      std::uint16_t unused{};
      const auto returned = shape->AddGroup(0, unused, 16, 0);
      if (!refresh.Observed() || returned != index)
        spdlog::error("Terrain grass: renderer registration refresh was not "
                      "intercepted.");
    }
  }
};
void **allocator{};
RE::BSNonReentrantSpinLock *groupLock{};
bool installed{};
struct Entry {
  Volume baseline;
  Surfaces surfaces;
  RE::FormID world{};
};
// Main-thread terrain snapshots. Upload hooks only retain source instance data.
std::map<RE::FormID, Entry> entries;
std::atomic_bool terrainActive{};
std::uint64_t revision{1}, activeStroke{};
BrushMask hidden;
PersistentGrass persistent;
RE::FormID currentWorld{};
bool brushTransition{};
std::atomic<std::shared_ptr<const HeightSnapshot>> heightSnapshot{
    std::make_shared<HeightSnapshot>()};
void PublishHeights() {
  auto snapshot = std::make_shared<HeightSnapshot>();
  snapshot->revision = revision;
  snapshot->world = currentWorld;
  snapshot->hidden = hidden;
  if (const auto it = persistent.worlds.find(currentWorld);
      it != persistent.worlds.end())
    snapshot->cleared = it->second;
  if (activeStroke)
    snapshot->cells = heightSnapshot.load()->cells;
  else
    for (const auto &[id, entry] : entries)
      if (entry.world == currentWorld)
        snapshot->cells.push_back({entry.baseline, entry.surfaces});
  snapshot->PrepareBounds();
  snapshot->PrepareChanges(*heightSnapshot.load());
  heightSnapshot.store(std::move(snapshot));
}
struct Instances {
  std::vector<std::uint16_t> original;
  std::uint64_t revision{};
  bool adjusted{}, repairBaseline{};
  bool recovered{};
  std::optional<float> padding;
};
std::mutex buffersMutex;
std::unordered_map<Buffer *, Instances> buffers;
std::array<std::uintptr_t, 4> grassUploads;
std::chrono::steady_clock::time_point nextUpdate;
struct Lock {
  RE::BSNonReentrantSpinLock &value;
  explicit Lock(RE::BSNonReentrantSpinLock &lock) : value(lock) {
    value.Lock();
  }
  ~Lock() { value.Unlock(); }
};
float Decode(std::uint16_t value) {
  static const REL::Relocation<float (*)(std::uint16_t)> fn{REL::ID(76218)};
  return fn(value);
}
std::uint16_t Encode(float value) {
  static const REL::Relocation<std::uint16_t (*)(float)> fn{REL::ID(76217)};
  return fn(value);
}
Buffer *Create(void *self, std::uint32_t size, const void *data,
               std::uint64_t flags) {
  const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
  auto result = createOriginal(self, size, data, flags);
  // Immediate/deferred live uploads and both cached group constructors.
  // Other vertex buffers are never copied. No terrain is written to cache
  // files.
  if (result && data && size && size <= 0x40000 && size % 32 == 0 &&
      std::find(grassUploads.begin(), grassUploads.end(), caller) !=
          grassUploads.end()) {
    try {
      Instances record;
      record.repairBaseline =
          terrainActive.load() &&
          (caller == grassUploads[0] || caller == grassUploads[1]);
      const auto words = static_cast<const std::uint16_t *>(data);
      record.original.assign(words, words + size / 2);
      std::lock_guard guard(buffersMutex);
      buffers.insert_or_assign(result, std::move(record));
    } catch (const std::bad_alloc &) {
      spdlog::error("Terrain grass: could not retain {} instance bytes.", size);
    }
  }
  return result;
}
void Free(void *self, Buffer *buffer) {
  {
    std::lock_guard guard(buffersMutex);
    buffers.erase(buffer);
  }
  freeOriginal(self, buffer);
}
std::uintptr_t Rip(std::uintptr_t instruction) {
  return instruction + 7 +
         *reinterpret_cast<const std::int32_t *>(instruction + 3);
}
bool Overlaps(const RE::BSMultiStreamInstanceTriShape::InstanceGroup &group) {
  if (hidden.Overlaps(
          group.center.x - group.size.x, group.center.y - group.size.y,
          group.center.x + group.size.x, group.center.y + group.size.y))
    return true;
  const auto snapshot = CaptureHeights();
  if (snapshot->cleared.Overlaps(
          group.center.x - group.size.x, group.center.y - group.size.y,
          group.center.x + group.size.x, group.center.y + group.size.y))
    return true;
  return std::any_of(entries.begin(), entries.end(), [&](const auto &item) {
    if (item.second.world != currentWorld)
      return false;
    const auto &volume = item.second.baseline;
    // Most loaded grass belongs to other cells. Reject its bounds before
    // consulting the modified-column mask.
    if (group.center.x + group.size.x < volume.origin.x - Step ||
        group.center.x - group.size.x > volume.origin.x + CellSize + Step ||
        group.center.y + group.size.y < volume.origin.y - Step ||
        group.center.y - group.size.y > volume.origin.y + CellSize + Step)
      return false;
    if (volume.density) {
      for (int y = 0; y < Side; ++y)
        for (int x = 0; x < Side; ++x) {
          const auto wx = volume.origin.x + x * Step,
                     wy = volume.origin.y + y * Step;
          if (wx + Step >= group.center.x - group.size.x &&
              wx - Step <= group.center.x + group.size.x &&
              wy + Step >= group.center.y - group.size.y &&
              wy - Step <= group.center.y + group.size.y &&
              DensityAffected(volume, wx, wy))
            return true;
        }
      return false;
    }
    const auto &edits = item.second.baseline.edits;
    return std::any_of(edits.begin(), edits.end(), [&](const Brush &brush) {
      return group.center.x + group.size.x >= brush.center.x - brush.radius &&
             group.center.y + group.size.y >= brush.center.y - brush.radius &&
             group.center.x - group.size.x <= brush.center.x + brush.radius &&
             group.center.y - group.size.y <= brush.center.y + brush.radius;
    });
  });
}
bool Rewrite(RE::BSMultiStreamInstanceTriShape *shape,
             RE::BSMultiStreamInstanceTriShape::InstanceGroup *group,
             Instances &record) {
  if (record.original.size() != std::size_t(group->instanceCount) * 16)
    return false;
  const auto snapshot = CaptureHeights();
  auto updated = record.original;
  bool adjusted{};
  float minZ = std::numeric_limits<float>::max(), maxZ = -minZ;
  float originalMin = minZ, originalMax = maxZ;
  for (std::size_t i = 0; i < updated.size(); i += 16) {
    const float x = Decode(updated[i]) + shape->local.translate.x,
                y = Decode(updated[i + 1]) + shape->local.translate.y;
    const float previous = Decode(updated[i + 2]);
    originalMin = std::min(originalMin, previous);
    originalMax = std::max(originalMax, previous);
    // With in-place capture hide only its GPU shadow, preserving native source
    // heights so regenerated captures can always restore the original roots.
    const bool concealed =
        snapshot->Concealed(x, y) && !RendererCapture::UsesInPlace();
    if (!concealed)
      for (const auto &entry : snapshot->cells) {
        const auto origin = entry.baseline.origin;
        if (x < origin.x || y < origin.y || x >= origin.x + CellSize ||
            y >= origin.y + CellSize)
          continue;
        if (!DensityAffected(entry.baseline, x, y))
          break;
        std::optional<SurfacePoint> top;
        for (const auto &surface : entry.surfaces)
          if (surface)
            if (const auto point = surface->Highest(x, y);
                point && (!top || point->height > top->height))
              top = point;
        if (top && std::abs(top->height) <= 65504) {
          // Newly generated live grass sampled the lowered LAND recovery
          // envelope. Retain its original terrain height for restoration
          // instead of that floor.
          if (record.repairBaseline ||
              (record.recovered && previous < entry.baseline.Height(x, y) - 4))
            record.original[i + 2] = Encode(entry.baseline.Height(x, y));
          updated[i + 2] = Encode(top->height);
          adjusted = true;
        }
        break;
      }
    if (concealed) {
      updated[i + 2] = Encode(BrushMask::HiddenHeight);
      adjusted = true;
    }
    const float z = Decode(updated[i + 2]);
    if (!std::isfinite(z))
      return false;
    // Concealed roots are intentionally outside the visible bounds. Do not
    // turn a small grass group into a 30,000-unit culling volume.
    const float boundsZ = concealed ? previous : z;
    minZ = std::min(minZ, boundsZ);
    maxZ = std::max(maxZ, boundsZ);
  }
  if (!adjusted && !record.adjusted) {
    record.revision = revision;
    return false;
  }
  const auto current =
      static_cast<const std::uint16_t *>(group->vertexBuffer->m_data);
  if (!current)
    return false;
  std::size_t changedRoots{};
  for (std::size_t i = 2; i < updated.size(); i += 16)
    if (updated[i] != current[i]) {
      ++changedRoots;
    }
  if (!changedRoots) {
    record.revision = revision;
    record.adjusted = adjusted;
    return false;
  }
  auto replacement =
      createOriginal(*allocator, static_cast<std::uint32_t>(updated.size() * 2),
                     updated.data(), 0);
  if (!replacement)
    return false;
  if (!record.padding)
    record.padding =
        std::max(0.F, group->size.z - (originalMax - originalMin) * .5F);
  // Keep the group and index owned by CellGrassData/NGIO. Use the same
  // allocator and instance lock as native group creation/removal; no renderer
  // context lock.
  group->center.z = (minZ + maxZ) * .5F;
  group->size.z = (maxZ - minZ) * .5F + *record.padding;
  auto old = group->vertexBuffer;
  group->vertexBuffer = replacement;
  record.revision = revision;
  record.adjusted = adjusted;
  auto moved = buffers.extract(old);
  moved.key() = replacement;
  buffers.insert(std::move(moved));
  freeOriginal(*allocator, old);

  return true;
}
} // namespace
void Initialize() {
  if (installed)
    return;
  const auto version = REL::Module::get().version();
  // Upload bodies, call offsets, globals and allocator slots audited on both.
  if (version != REL::Version(1, 6, 1170, 0) &&
      version != REL::Version(1, 7, 104, 0)) {
    spdlog::warn("Terrain grass buffers are not verified for this runtime.");
    return;
  }
  const auto add = REL::ID(76323).address();
  const std::array<std::uint8_t, 3> load{0x48, 0x8B, 0x0D},
      address{0x48, 0x8D, 0x05};
  if (std::memcmp(reinterpret_cast<const void *>(add + 0x218), load.data(),
                  3) ||
      std::memcmp(reinterpret_cast<const void *>(add + 0x23E), address.data(),
                  3)) {
    spdlog::error(
        "Terrain grass: native instance instructions differ; hooks disabled.");
    return;
  }
  // RIP-relative globals read by the audited native AddGroup implementation.
  allocator = reinterpret_cast<void **>(Rip(add + 0x218));
  groupLock = reinterpret_cast<RE::BSNonReentrantSpinLock *>(Rip(add + 0x23E));
  grassUploads = {add + 0x236, REL::ID(76326).address() + 0x22D,
                  REL::ID(76338).address() + 0x98,
                  REL::ID(76339).address() + 0xED};
  const auto vtable = reinterpret_cast<void **>(
      RE::VTABLE_BSShaderResourceManager[0].address());
  auto create = vtable[0x1E], release = vtable[0x1F];
  const auto status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    return;
  if (MH_CreateHook(create, reinterpret_cast<void *>(&Create),
                    reinterpret_cast<void **>(&createOriginal)) != MH_OK)
    return;
  if (MH_CreateHook(release, reinterpret_cast<void *>(&Free),
                    reinterpret_cast<void **>(&freeOriginal)) != MH_OK) {
    MH_RemoveHook(create);
    return;
  }
  if (MH_QueueEnableHook(create) != MH_OK ||
      MH_QueueEnableHook(release) != MH_OK || MH_ApplyQueued() != MH_OK) {
    MH_DisableHook(create);
    MH_DisableHook(release);
    MH_RemoveHook(create);
    MH_RemoveHook(release);
    spdlog::error("Terrain grass buffer hooks could not be enabled.");
    return;
  }
  installed = true;
  // CS captures immutable instance copies in its AddGroup virtual hook. Keep
  // that hook in the chain, but suppress native insertion during a refresh.
  // Ordinary engine calls still execute the original function unchanged.
  void *addTarget = reinterpret_cast<void *>(add);
  if (MH_CreateHook(addTarget, reinterpret_cast<void *>(&Add),
                    reinterpret_cast<void **>(&addOriginal)) == MH_OK) {
    if (MH_EnableHook(addTarget) == MH_OK)
      registrationRefreshInstalled = true;
    else
      MH_RemoveHook(addTarget);
  }
  if (!registrationRefreshInstalled)
    spdlog::warn(
        "Terrain grass: renderer registration bridge could not be installed.");
  RendererCapture::Initialize(groupLock);
}
void SetWorld(RE::FormID world) {
  if (currentWorld == world)
    return;
  currentWorld = world;
  activeStroke = 0;
  hidden.tiles.clear();
  ++revision;
  PublishHeights();
  nextUpdate = {};
  brushTransition = true;
}
bool Paint(RE::FormID world, const Brush &brush, bool restore) {
  if (!installed || !persistent.Paint(world, brush, restore))
    return false;
  SetWorld(world);
  ++revision;
  PublishHeights();
  nextUpdate = {};
  brushTransition = true;
  terrainActive.store(true);
  return true;
}
std::uint64_t UndoStroke(RE::FormID world) {
  return persistent.LastUndo(world);
}
std::uint64_t RedoStroke(RE::FormID world) {
  return persistent.LastRedo(world);
}
void History(RE::FormID world, bool redo) {
  if (persistent.Replay(world, redo)) {
    ++revision;
    PublishHeights();
    nextUpdate = {};
    brushTransition = true;
  }
}
void ClearRedo() { persistent.redo.clear(); }
void ResetPersistent() {
  persistent = {};
  currentWorld = 0;
  ++revision;
  PublishHeights();
  nextUpdate = {};
}
void Save(SKSE::SerializationInterface *serialization) {
  if (!serialization)
    return;
  try {
    const auto bytes = EncodeGrass(persistent);
    if (!serialization->OpenRecord(ClearSaveRecord, 1) ||
        !serialization->WriteRecordData(bytes.data(),
                                        std::uint32_t(bytes.size())))
      spdlog::error("Could not save grass clearing masks.");
  } catch (const std::exception &error) {
    spdlog::error("Grass clearing save failed: {}", error.what());
  }
}
void Load(SKSE::SerializationInterface *serialization, std::uint32_t version,
          std::uint32_t length) {
  if (!serialization || version != 1 || length > 16 * 1024 * 1024)
    return;
  std::vector<std::byte> bytes(length);
  PersistentGrass loaded, resolved;
  if (serialization->ReadRecordData(bytes.data(), length) != length ||
      !DecodeGrass(bytes, loaded)) {
    spdlog::warn("Invalid grass clearing save record.");
    return;
  }
  for (auto &[world, mask] : loaded.worlds) {
    RE::FormID id{};
    if (serialization->ResolveFormID(world, id)) {
      if (resolved.worlds.contains(id)) {
        spdlog::warn("Duplicate resolved grass world.");
        return;
      }
      resolved.worlds.emplace(id, std::move(mask));
    }
  }
  persistent = std::move(resolved);
  ++revision;
  PublishHeights();
  nextUpdate = {};
  brushTransition = true;
}
void Publish(RE::TESObjectCELL *cell, const Surfaces &surfaces,
             const Volume &volume) {
  if (!installed || !cell)
    return;
  const auto world = cell->GetRuntimeData().worldSpace;
  Entry entry{volume, surfaces, world ? world->GetFormID() : 0};
  if (volume.density) {
    // Grass consumes the published field, not the editing transaction. Avoid
    // pinning the previous working/completed grids through grass snapshots.
    auto readOnly = std::make_shared<DensityCache>(*volume.density);
    readOnly->completed = readOnly->current;
    readOnly->from.reset();
    readOnly->changed.clear();
    entry.baseline.density = std::move(readOnly);
  }
  entries.insert_or_assign(cell->GetFormID(), std::move(entry));
  terrainActive.store(true);
  if (activeStroke)
    return;
  ++revision;
  PublishHeights();
  nextUpdate = {};
}
void BrushArea(const Brush &brush) {
  if (!installed || !brush.stroke)
    return;
  if (activeStroke != brush.stroke) {
    // A newly started press can follow a completed press before the next idle
    // update. Commit its final heights before freezing the new stroke.
    if (activeStroke) {
      activeStroke = 0;
      hidden.tiles.clear();
      ++revision;
      PublishHeights();
    }
    activeStroke = brush.stroke;
  }
  if (hidden.Add(brush)) {
    brushTransition = true;
    terrainActive.store(true);
    ++revision;
    PublishHeights();
    nextUpdate = {};
  }
}
void FinishBrush(std::uint64_t heldStroke, bool building) {
  if (!activeStroke || building || heldStroke == activeStroke)
    return;
  activeStroke = 0;
  hidden.tiles.clear();
  brushTransition = true;
  terrainActive.store(activeStroke || !entries.empty());
  ++revision;
  PublishHeights();
  nextUpdate = {};
}
void Remove(RE::FormID id) {
  if (entries.erase(id)) {
    terrainActive.store(activeStroke || !entries.empty());
    ++revision;
    PublishHeights();
    nextUpdate = {};
  }
}
void Clear() {
  RendererCapture::Clear();
  entries.clear();
  activeStroke = 0;
  hidden.tiles.clear();
  terrainActive.store(false);
  ++revision;
  PublishHeights();
  nextUpdate = {};
  // Retain originals until native buffers are freed, including during
  // transition.
}
std::shared_ptr<const HeightSnapshot> CaptureHeights() {
  return heightSnapshot.load();
}

void Update() {
  if (!installed || std::chrono::steady_clock::now() < nextUpdate)
    return;
  nextUpdate =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  auto manager = RE::BGSGrassManager::GetSingleton();
  if (!manager || !manager->enableGrass || manager->generateGrassDataFiles ||
      !*allocator)
    return;
  // grassShapes is an attachment queue, not the registry of rendered grass.
  // It can be empty once the shapes are attached (including NGIO's path).
  // Snapshot the owning type registry under its lock, retaining shapes while
  // inspecting their groups. Release the registry lock before the group lock.
  std::vector<RE::NiPointer<RE::BSMultiStreamInstanceTriShape>> shapes;
  {
    const RE::BSReadLockGuard typesGuard(manager->grassTypeLock);
    for (const auto &[key, type] : manager->grassTypeMap)
      if (type && type->typeShape)
        shapes.emplace_back(type->typeShape);
  }
  std::sort(shapes.begin(), shapes.end(), [](const auto &a, const auto &b) {
    return std::less<RE::BSMultiStreamInstanceTriShape *>{}(a.get(), b.get());
  });
  shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
  // Declare before locks so its destructor notifies renderer hooks only after
  // the buffer mutex and engine group lock have been released. CS acquires its
  // lifecycle mutex before the engine group lock; notifying inside it
  // deadlocks.
  Notifications notifications;
  Lock groupsGuard(*groupLock);
  std::lock_guard buffersGuard(buffersMutex);
  std::size_t changed{};
  const auto heights = CaptureHeights();
  // In-place capture hides grass on the GPU; mask-only changes do not need
  // native group rewrites or recapturing their renderer buffers.
  const auto &spatialChanges = RendererCapture::UsesInPlace()
                                   ? heights->surfaceChanges
                                   : heights->changes;
  for (const auto &shapeOwner : shapes) {
    auto shape = shapeOwner.get();
    if (!shape)
      continue;
    const auto &runtime = shape->GetMultiStreamTrishapeRuntimeData();
    // Cache-only shapes need not call BeginAddingInstances, which initializes
    // instanceSize for live creation. Validate each buffer's actual stride in
    // Rewrite instead of rejecting an otherwise valid cached group.
    for (std::uint32_t index = 0; index < runtime.instanceGroups.size();
         ++index) {
      auto group = runtime.instanceGroups[index];
      if (!group || !group->instanceCount)
        continue;
      auto found = buffers.find(group->vertexBuffer);
      if (found != buffers.end() && found->second.revision != revision &&
          !spatialChanges.AffectsSince(found->second.revision,
                                       {group->center.x - group->size.x,
                                        group->center.y - group->size.y,
                                        group->center.x + group->size.x,
                                        group->center.y + group->size.y})) {
        found->second.revision = revision;
        continue;
      }
      // Revision is authoritative until a terrain publication or buffer upload.
      // Check it before the spatial scan, including for untouched grass groups.
      if (found != buffers.end() && found->second.revision == revision)
        continue;
      const bool overlaps = Overlaps(*group);
      if (found == buffers.end() && overlaps) {
        // Native grass buffers retain their CPU instance bytes (also used by
        // the native/NGIO cache writer). This is the authoritative source even
        // when grass existed before our hooks or another uploader was used.
        const auto buffer = group->vertexBuffer;
        const auto bytes = std::size_t(group->instanceCount) * 32;
        // AE 77293 writes a 32-bit byte count at +0x10. CommonLib declares
        // size_t, but the upper four bytes are not initialized by the engine.
        std::uint32_t nativeBytes{};
        if (buffer)
          std::memcpy(&nativeBytes,
                      reinterpret_cast<const std::byte *>(buffer) + 0x10,
                      sizeof(nativeBytes));
        if (buffer && buffer->m_data && nativeBytes == bytes &&
            bytes <= 0x40000) {
          Instances record;
          const auto source =
              static_cast<const std::uint16_t *>(buffer->m_data);
          record.original.assign(source, source + bytes / 2);
          record.recovered = true;
          found = buffers.emplace(buffer, std::move(record)).first;
        }
      }
      if (found == buffers.end() || found->second.revision == revision)
        continue;
      if (!found->second.adjusted && !overlaps) {
        found->second.revision = revision;
        continue;
      }
      if (Rewrite(shape, group, found->second)) {
        // The capture hook applies this same height snapshot to the existing
        // renderer buffer before culling. Re-registering it through AddGroup
        // replaces the provider's chunk and restarts its whole-group fade.
        // Keep native roots/bounds current for Skyrim and future captures,
        // but leave the current renderer registration intact.
        if (!RendererCapture::UsesInPlace())
          notifications.groups.emplace_back(shape, index);
        if (++changed == 4 && !brushTransition) {
          return;
        }
      }
    }
  }
  brushTransition = false;
}
} // namespace SkyBuild::Terrain::Grass
