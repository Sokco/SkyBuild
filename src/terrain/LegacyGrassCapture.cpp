#include "terrain/LegacyGrassCapture.h"
#include "terrain/GrassBufferShadow.h"
#include "terrain/GrassRootCache.h"
#include "terrain/PackedGrassGroup.h"
#include "terrain/TerrainGrass.h"
#include <MinHook.h>
#include <d3d11.h>
#include <mutex>
#include <wrl/client.h>

namespace SkyBuild::Terrain::Grass::RendererCapture {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID shadowID{0x6270639d,
                        0x6aba,
                        0x4db6,
                        {0xa5, 0x31, 0x52, 0x84, 0x40, 0x31, 0xb2, 0xe8}};
constexpr GUID debugNameID{0x429b8c22,
                           0x9188,
                           0x4b0c,
                           {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};
enum class Kind { Instances, Origins, PackedInstances, Groups };
bool packedCapture{};
std::size_t Stride(Kind kind) {
  return kind == Kind::Instances ? 32 : kind == Kind::Groups ? 24 : 16;
}
std::recursive_mutex captureGate;
std::atomic_uint64_t nextIdentity{1};
// Attached to the D3D resource itself. Its CPU shadow dies with the resource;
// there is no owning resource cache, pointer-reuse hazard or GPU lifetime leak.
struct Shadow final : IUnknown {
  std::atomic_ulong references{1};
  Kind kind;
  BufferShadow bytes;
  std::vector<RootHeightCache> roots;
  std::vector<std::optional<GrassBounds>> bounds;
  const std::uint64_t identity = nextIdentity.fetch_add(1);
  std::uint64_t revision{}, sourceEpoch{}, originsEpoch{}, originsIdentity{};
  Shadow(Kind kind, std::size_t size) : kind(kind), bytes(size, Stride(kind)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references;
    if (!remaining)
      delete this;
    return remaining;
  }
};
using UpdateFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *,
                                           ID3D11Resource *, UINT,
                                           const D3D11_BOX *, const void *,
                                           UINT, UINT);
using CopyFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *,
                                         ID3D11Resource *, UINT, UINT, UINT,
                                         UINT, ID3D11Resource *, UINT,
                                         const D3D11_BOX *);
using BindFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT,
                                         ID3D11ShaderResourceView *const *);
UpdateFn updateOriginal{};
CopyFn copyOriginal{};
BindFn bindOriginal{};
bool installed{};
ComPtr<Shadow> GetShadow(ID3D11Resource *resource, bool create = true) {
  ComPtr<Shadow> result;
  if (!resource)
    return result;
  UINT size = sizeof(Shadow *);
  Shadow *stored{};
  if (SUCCEEDED(resource->GetPrivateData(shadowID, &size, &stored)) && stored) {
    result.Attach(
        stored); // GetPrivateData adds a reference for interface data.
    return result;
  }
  if (!create)
    return result;
  char name[96]{};
  size = sizeof(name) - 1;
  if (FAILED(resource->GetPrivateData(debugNameID, &size, name)))
    return result;
  const auto end = std::find(
      name, name + std::min<std::size_t>(size, sizeof(name) - 1), '\0');
  const std::string_view label(name, end - name);
  Kind kind;
  if (label == "GrassOptimizations::InstanceBuf")
    kind = packedCapture ? Kind::PackedInstances : Kind::Instances;
  else if (label == "GrassOptimizations::OriginBuf")
    kind = Kind::Origins;
  else if (label == "GrassOptimizations::GroupBuf")
    kind = Kind::Groups;
  else if (packedCapture && label == "GrassOptimizations::CompactionScratch")
    kind = Kind::PackedInstances;
  else
    return result;
  ComPtr<ID3D11Buffer> buffer;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(buffer.GetAddressOf()))))
    return result;
  D3D11_BUFFER_DESC desc{};
  buffer->GetDesc(&desc);
  const auto stride = static_cast<UINT>(Stride(kind));
  if (desc.Usage != D3D11_USAGE_DEFAULT ||
      (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
       label != "GrassOptimizations::CompactionScratch") ||
      !desc.ByteWidth || desc.ByteWidth > 64 * 1024 * 1024 ||
      desc.ByteWidth % stride ||
      (kind == Kind::Groups && desc.StructureByteStride != 24))
    return result;
  result.Attach(new Shadow(kind, desc.ByteWidth));
  if (FAILED(resource->SetPrivateDataInterface(shadowID, result.Get())))
    result.Reset();
  return result;
}
void STDMETHODCALLTYPE Update(ID3D11DeviceContext *context,
                              ID3D11Resource *resource, UINT subresource,
                              const D3D11_BOX *box, const void *data,
                              UINT rowPitch, UINT depthPitch) {
  try {
    std::lock_guard guard(captureGate);
    if (auto shadow = GetShadow(resource); shadow && !subresource) {
      const auto first = box ? box->left : 0u;
      const auto last = box ? box->right : shadow->bytes.source.size();
      if (last >= first)
        shadow->bytes.Write(first, last - first, data);
    }
  } catch (const std::exception &error) {
    spdlog::error("Terrain grass GPU upload tracking failed: {}.",
                  error.what());
  }
  updateOriginal(context, resource, subresource, box, data, rowPitch,
                 depthPitch);
}
void STDMETHODCALLTYPE Copy(ID3D11DeviceContext *context, ID3D11Resource *dest,
                            UINT destSub, UINT x, UINT y, UINT z,
                            ID3D11Resource *source, UINT sourceSub,
                            const D3D11_BOX *box) {
  try {
    std::lock_guard guard(captureGate);
    auto from = GetShadow(source), to = GetShadow(dest);
    if (from && to && from->kind == to->kind && !destSub && !sourceSub && !y &&
        !z) {
      const auto first = box ? box->left : 0u;
      const auto last = box ? box->right : from->bytes.source.size();
      if (last >= first)
        to->bytes.Copy(x, from->bytes, first, last - first);
    }
  } catch (const std::exception &error) {
    spdlog::error("Terrain grass GPU compaction tracking failed: {}.",
                  error.what());
  }
  copyOriginal(context, dest, destSub, x, y, z, source, sourceSub, box);
}
void Adjust(ID3D11DeviceContext *context, ID3D11Resource *resource,
            Shadow &instances, const Shadow &origins,
            const HeightSnapshot &terrain) {
  if (instances.revision == terrain.revision &&
      instances.sourceEpoch == instances.bytes.epoch &&
      instances.originsEpoch == origins.bytes.epoch &&
      instances.originsIdentity == origins.identity)
    return;
  const auto count =
      std::min(instances.bytes.valid.size(), origins.bytes.valid.size());
  const bool refresh = instances.revision != terrain.revision ||
                       instances.originsIdentity != origins.identity;
  if (instances.roots.size() != count) {
    instances.roots.resize(count);
    instances.bounds.resize((count + BufferShadow::BlockRecords - 1) /
                            BufferShadow::BlockRecords);
  }
  static const REL::Relocation<float (*)(std::uint16_t)> decode{REL::ID(76218)};
  static const REL::Relocation<std::uint16_t (*)(float)> encode{REL::ID(76217)};
  std::size_t first = count, last{};
  const auto flush = [&] {
    if (first == count)
      return;
    const auto begin = first * 32, end = (last + 1) * 32;
    const D3D11_BOX box{static_cast<UINT>(begin), 0, 0,
                        static_cast<UINT>(end),   1, 1};
    // The shadow contains the latest renderer upload, including spawn times
    // and flags. Change only height; submit before culling binds the buffer.
    {
      updateOriginal(context, resource, 0, &box,
                     instances.bytes.gpu.data() + begin, 0, 0);
    }
    first = count;
  };

  for (std::size_t block = 0; block * BufferShadow::BlockRecords < count;
       ++block) {
    const bool uploaded =
        instances.bytes.changed[block] > instances.sourceEpoch ||
        origins.bytes.changed[block] > instances.originsEpoch;
    const bool newOrigins = instances.originsIdentity != origins.identity;
    const bool spatial = instances.revision != terrain.revision &&
                         instances.bounds[block] &&
                         terrain.changes.AffectsSince(instances.revision,
                                                      *instances.bounds[block]);
    if (!uploaded && !newOrigins && !spatial) {
      flush();
      continue;
    }
    const auto end = std::min(count, (block + 1) * BufferShadow::BlockRecords);
    if (uploaded || newOrigins)
      instances.bounds[block].reset();
    for (auto i = block * BufferShadow::BlockRecords; i < end; ++i) {
      auto &root = instances.roots[i];
      if (!instances.bytes.valid[i] || !origins.bytes.valid[i]) {
        root.valid = false;
        flush();
        continue;
      }
      const auto *source = instances.bytes.source.data() + i * 32;
      const auto *originBytes = origins.bytes.source.data() + i * 16;
      if (!root.Matches(source, originBytes)) {
        std::array<float, 3> origin;
        std::array<std::uint16_t, 3> position;
        std::memcpy(origin.data(), originBytes, 12);
        std::memcpy(position.data(), source, 6);
        root.x = decode(position[0]) + origin[0];
        root.y = decode(position[1]) + origin[1];
      }
      if ((uploaded || newOrigins) && std::isfinite(root.x) &&
          std::isfinite(root.y)) {
        const GrassBounds point{root.x, root.y, root.x, root.y};
        if (instances.bounds[block])
          instances.bounds[block]->Include(point);
        else
          instances.bounds[block] = point;
      }
      const bool rootRefresh =
          refresh && terrain.changes.AffectsSince(
                         instances.revision, {root.x, root.y, root.x, root.y});
      const auto height = root.Resolve(source, originBytes, rootRefresh, [&] {
        std::array<float, 3> origin;
        std::array<std::uint16_t, 3> position;
        std::memcpy(origin.data(), originBytes, 12);
        std::memcpy(position.data(), source, 6);
        if (!std::isfinite(origin[0]) || !std::isfinite(origin[1]) ||
            !std::isfinite(origin[2]))
          return position[2];
        const auto x = root.x, y = root.y;
        if (!terrain.MayAffect(x, y))
          return position[2];
        if (terrain.Concealed(x, y))
          return encode(BrushMask::HiddenLocalHeight(origin[2]));
        if (const auto value = terrain.Highest(x, y);
            value && std::abs(*value - origin[2]) <= 65504)
          return encode(*value - origin[2]);
        return position[2];
      });
      const auto offset = i * 32 + 4;
      if (std::memcmp(&height, instances.bytes.gpu.data() + offset, 2)) {
        // Do not upload large untouched gaps between changed roots. Small
        // gaps stay batched to avoid an update call for each blade.
        if (first != count && i > last + 8)
          flush();
        std::memcpy(instances.bytes.gpu.data() + offset, &height, 2);
        first = std::min(first, i);
        last = i;
      }
    }
  }
  flush();

  instances.revision = terrain.revision;
  instances.sourceEpoch = instances.bytes.epoch;
  instances.originsEpoch = origins.bytes.epoch;
  instances.originsIdentity = origins.identity;
}
// New Bottled/CS layout: 16-byte packed source records and 24-byte cull
// groups. SourceStart/Count map each group's origin to its instance range.
void AdjustPacked(ID3D11DeviceContext *context, ID3D11Resource *resource,
                  Shadow &instances, const Shadow &groups,
                  const HeightSnapshot &terrain) {
  if (instances.revision == terrain.revision &&
      instances.sourceEpoch == instances.bytes.epoch &&
      instances.originsEpoch == groups.bytes.epoch &&
      instances.originsIdentity == groups.identity)
    return;
  const auto count = instances.bytes.valid.size();
  instances.roots.resize(count);
  instances.bounds.resize(groups.bytes.valid.size());
  static const REL::Relocation<float (*)(std::uint16_t)> decode{REL::ID(76218)};
  static const REL::Relocation<std::uint16_t (*)(float)> encode{REL::ID(76217)};
  const bool newGroups = instances.originsIdentity != groups.identity;
  const bool revisionChanged =
      instances.revision != terrain.revision || newGroups;
  std::size_t first = count, last{};
  const auto flush = [&] {
    if (first == count)
      return;
    const D3D11_BOX box{static_cast<UINT>(first * 16),      0, 0,
                        static_cast<UINT>((last + 1) * 16), 1, 1};
    updateOriginal(context, resource, 0, &box,
                   instances.bytes.gpu.data() + first * 16, 0, 0);
    first = count;
  };
  for (std::size_t g = 0; g < groups.bytes.valid.size(); ++g) {
    if (!groups.bytes.valid[g])
      continue;
    const auto *data = groups.bytes.source.data() + g * 24;
    PackedGrassGroup group;
    std::memcpy(&group, data, sizeof(group));
    const auto &origin = group.origin;
    const std::size_t start = group.sourceStart, size = group.count;
    if (!group.Valid(count) || !std::isfinite(origin[0]) ||
        !std::isfinite(origin[1]) || !std::isfinite(origin[2]))
      continue;
    bool uploaded =
        newGroups || groups.bytes.changed[g / BufferShadow::BlockRecords] >
                         instances.originsEpoch;
    for (auto block = start / BufferShadow::BlockRecords;
         !uploaded && block <= (start + size - 1) / BufferShadow::BlockRecords;
         ++block)
      uploaded = instances.bytes.changed[block] > instances.sourceEpoch;
    const bool spatial =
        revisionChanged && instances.bounds[g] &&
        terrain.changes.AffectsSince(instances.revision, *instances.bounds[g]);
    if (!uploaded && !spatial)
      continue;
    if (uploaded)
      instances.bounds[g].reset();
    for (auto i = start; i < start + size; ++i) {
      if (!instances.bytes.valid[i]) {
        flush();
        continue;
      }
      const auto *source = instances.bytes.source.data() + i * 16;
      auto &root = instances.roots[i];
      if (!root.Matches(source, data)) {
        std::array<std::uint16_t, 3> position;
        std::memcpy(position.data(), source, 6);
        root.x = origin[0] + decode(position[0]);
        root.y = origin[1] + decode(position[1]);
      }
      if (!std::isfinite(root.x) || !std::isfinite(root.y))
        continue;
      if (uploaded) {
        const GrassBounds point{root.x, root.y, root.x, root.y};
        if (instances.bounds[g])
          instances.bounds[g]->Include(point);
        else
          instances.bounds[g] = point;
      }
      const bool refresh =
          revisionChanged &&
          terrain.changes.AffectsSince(instances.revision,
                                       {root.x, root.y, root.x, root.y});
      const auto height = root.Resolve(source, data, refresh, [&] {
        std::uint16_t original;
        std::memcpy(&original, source + 4, 2);
        if (!terrain.MayAffect(root.x, root.y))
          return original;
        if (terrain.Concealed(root.x, root.y))
          return encode(BrushMask::HiddenLocalHeight(origin[2]));
        if (const auto top = terrain.Highest(root.x, root.y);
            top && std::abs(*top - origin[2]) <= 65504)
          return encode(*top - origin[2]);
        return original;
      });
      const auto offset = i * 16 + 4;
      if (std::memcmp(&height, instances.bytes.gpu.data() + offset, 2)) {
        if (first != count && (i < last || i > last + 8))
          flush();
        std::memcpy(instances.bytes.gpu.data() + offset, &height, 2);
        first = std::min(first, i);
        last = i;
      }
    }
  }
  flush();
  instances.revision = terrain.revision;
  instances.sourceEpoch = instances.bytes.epoch;
  instances.originsEpoch = groups.bytes.epoch;
  instances.originsIdentity = groups.identity;
}
void STDMETHODCALLTYPE Bind(ID3D11DeviceContext *context, UINT start,
                            UINT count,
                            ID3D11ShaderResourceView *const *views) {
  {
    if (!start && count >= 2 && views && views[0] && views[1]) {
      try {
        const auto terrain = CaptureHeights();
        std::lock_guard guard(captureGate);
        ComPtr<ID3D11Resource> records, origins;
        views[0]->GetResource(records.GetAddressOf());
        views[1]->GetResource(origins.GetAddressOf());
        auto instancesShadow = GetShadow(records.Get()),
             originsShadow = GetShadow(origins.Get());
        if (instancesShadow && originsShadow &&
            instancesShadow->kind == Kind::Instances &&
            originsShadow->kind == Kind::Origins)
          Adjust(context, records.Get(), *instancesShadow.Get(),
                 *originsShadow.Get(), *terrain);
        else if (instancesShadow && originsShadow &&
                 instancesShadow->kind == Kind::PackedInstances &&
                 originsShadow->kind == Kind::Groups)
          AdjustPacked(context, records.Get(), *instancesShadow.Get(),
                       *originsShadow.Get(), *terrain);
      } catch (const std::exception &error) {
        spdlog::error("Terrain grass GPU height update failed: {}.",
                      error.what());
      }
    }
  }
  bindOriginal(context, start, count, views);
}
} // namespace
void Initialize(RE::BSNonReentrantSpinLock *) {
  if (installed)
    return;
  const auto provider = GetModuleHandleW(L"CommunityShaders.dll");
  if (!provider)
    return;
  const auto table = reinterpret_cast<void **>(
      RE::VTABLE_BSMultiStreamInstanceTriShape[0].address());
  const auto owned = [&](void *address) {
    HMODULE owner{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &owner) &&
           owner == provider;
  };
  packedCapture = owned(table[0x3C]);
  if (!packedCapture && !owned(table[0x3A]))
    return;
  const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
  const auto context = renderer ? reinterpret_cast<ID3D11DeviceContext *>(
                                      renderer->GetRuntimeData().context)
                                : nullptr;
  if (!context)
    return;
  // Standard ID3D11DeviceContext ABI, verified against the Windows SDK header.
  const auto contextTable = *reinterpret_cast<void ***>(context);
  const std::array<void *, 3> targets{contextTable[48], contextTable[46],
                                      contextTable[67]};
  const std::array<void *, 3> detours{reinterpret_cast<void *>(&Update),
                                      reinterpret_cast<void *>(&Copy),
                                      reinterpret_cast<void *>(&Bind)};
  const std::array<void **, 3> originals{
      reinterpret_cast<void **>(&updateOriginal),
      reinterpret_cast<void **>(&copyOriginal),
      reinterpret_cast<void **>(&bindOriginal)};
  std::size_t created{};
  for (; created < targets.size(); ++created)
    if (MH_CreateHook(targets[created], detours[created], originals[created]) !=
        MH_OK)
      break;
  bool enabled = created == targets.size();
  for (std::size_t i = 0; enabled && i < created; ++i)
    enabled = MH_QueueEnableHook(targets[i]) == MH_OK;
  enabled = enabled && MH_ApplyQueued() == MH_OK;
  if (!enabled) {
    for (std::size_t i = 0; i < created; ++i) {
      MH_DisableHook(targets[i]);
      MH_RemoveHook(targets[i]);
    }
    spdlog::error(
        "Terrain grass: legacy in-place GPU hooks could not be installed.");
    return;
  }
  installed = true;
}
bool UsesInPlace() { return installed; }
void Clear() {} // Immutable height snapshot withdrawal restores original GPU
                // roots at their next bind.
} // namespace SkyBuild::Terrain::Grass::RendererCapture
