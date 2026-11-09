#include "navmesh/native/NativeDraftInfo.h"
#include "navmesh/native/NativeBounds.h"
#include "navmesh/native/NativeInfoResolver.h"
#include <cmath>
#include <cstring>
#include <set>
namespace SkyBuild::Navmesh {
namespace {
RE::BSNavmeshInfoMap *Registry() {
  const REL::Relocation<RE::BSNavmeshInfoMap **> registry{REL::RelocationID(515943, 402040)};
  return *registry.get();
}
} // namespace
NativeDraftInfo::~NativeDraftInfo() {
  if (!info)
    return;
  // 30360 releases bounds/pathing-cell ownership and clears the mesh backlink.
  // It does not release the derived info's arrays. Mirror 30362's verified
  // array cleanup even though island drafts are expected to have empty arrays.
  if (info->adjacentMeshesArray)
    RE::free(info->adjacentMeshesArray);
  info->adjacentMeshesArray = nullptr;
  if (info->connectedDoorsArray) {
    const auto allocation =
        reinterpret_cast<std::byte *>(info->connectedDoorsArray) - 8;
    std::size_t count;
    std::memcpy(&count, allocation, sizeof(count));
    using Destroy = void (*)(void *, std::size_t, std::size_t, std::uintptr_t);
    const REL::Relocation<Destroy> destroy{REL::RelocationID(102182, 109621)};
    const REL::Relocation<std::uintptr_t> elementDestructor{REL::RelocationID(529147, 30371)};
    destroy(info->connectedDoorsArray, 8, count, elementDestructor.address());
    using FreeArray = void (*)(void *, std::size_t);
    const REL::Relocation<FreeArray> freeArray{REL::RelocationID(102185, 109625)};
    freeArray(allocation, count * 8 + 8);
    info->connectedDoorsArray = nullptr;
  }
  using Destruct = void (*)(RE::BSNavmeshInfo *);
  const REL::Relocation<Destruct> destruct{REL::RelocationID(29511, 30360)};
  destruct(info);
  RE::free(info);
}
bool NativeDraftInfo::Registered(RE::FormID id) {
  const auto registry = Registry();
  return NativeInfoResolver::LookupRegistered(registry, id) != nullptr;
}
std::unique_ptr<NativeDraftInfo> NativeDraftInfo::Create(RE::NavMesh &form,
                                                         std::string &error) {
  auto registry = Registry();
  if (!form.parentCell || !form.GetSaveParentCell() || !registry ||
      NativeInfoResolver::LookupRegistered(registry, form.GetFormID()) ||
      form.unk110 || !form.extraEdgeInfo.empty() || !form.doorPortals.empty() ||
      !form.closedDoors.empty()) {
    error = "Native island info preconditions failed";
    return nullptr;
  }
  auto draft = std::make_unique<NativeDraftInfo>();
  auto storage =
      static_cast<RE::BSNavmeshInfo *>(RE::malloc(sizeof(RE::BSNavmeshInfo)));
  if (!storage) {
    error = "Could not allocate detached native info";
    return nullptr;
  }
  using Construct = RE::BSNavmeshInfo *(*)(RE::BSNavmeshInfo *);
  const REL::Relocation<Construct> construct{REL::RelocationID(29510, 30359)};
  draft->info = construct(storage);
  using Populate = void (*)(RE::BSNavmeshInfo *, RE::NavMesh *);
  const REL::Relocation<Populate> populate{REL::RelocationID(29513, 30362)};
  const REL::Relocation<Populate> bind{REL::RelocationID(29514, 30363)};
  populate(draft->info, &form);
  bind(draft->info, &form);
  form.unk110 = draft->info;
  if (const auto reason = draft->ValidationError(form); !reason.empty()) {
    error = "Detached native info validation failed: " + reason;
    spdlog::error("Native info draft {:08X}: {}", form.GetFormID(), error);
    return nullptr;
  }
  return draft;
}
bool NativeDraftInfo::Matches(const RE::NavMesh &form) const {
  return ValidationError(form).empty();
}
bool NativeDraftInfo::Refresh(RE::NavMesh &form, std::string &error) {
  if (!info || Registered(form.GetFormID()) || form.unk110 != info ||
      !form.extraEdgeInfo.empty() || !form.doorPortals.empty() ||
      !form.closedDoors.empty()) {
    error = "Detached info refresh preconditions failed";
    return false;
  }
  using Populate = void (*)(RE::BSNavmeshInfo *, RE::NavMesh *);
  const REL::Relocation<Populate> populate{REL::RelocationID(29513, 30362)};
  populate(info, &form);
  error = ValidationError(form);
  return error.empty();
}
std::string NativeDraftInfo::ValidationError(const RE::NavMesh &form,
                                             bool published) const {
  const auto registry = Registry();
  if (!info)
    return "Missing info record";
  if (!registry)
    return "Missing navigation registry";
  const auto registered =
      NativeInfoResolver::LookupRegistered(registry, form.GetFormID());
  if (published && registered != info)
    return "Published info registry identity mismatch";
  if (!published && registered)
    return "Draft unexpectedly registered";
  if (info->navMeshID != form.GetFormID())
    return std::format("Form ID {:08X}, expected {:08X}", info->navMeshID,
                       form.GetFormID());
  if (form.unk110 != info)
    return "Mesh backlink mismatch";
  if (info->navMesh != static_cast<const RE::BSNavmesh *>(&form))
    return "Info mesh pointer mismatch";
  // Populate (30362) resolves the mesh's cell and asks the manager (30684)
  // for a retained wrapper. Its cache can return a different PathingCell
  // allocation with the same world/cell key. Compare identity and resolution,
  // not wrapper addresses; neither wrapper is modified here.
  if (!info->pathingCell || !form.parentCell)
    return "Missing retained pathing cell";
  const REL::Relocation<std::uintptr_t> cellVtable{RE::VTABLE_PathingCell[0]};
  const auto isPathingCell = [&](const RE::BSPathingCell *cell) {
    return *reinterpret_cast<const std::uintptr_t *>(cell) ==
           cellVtable.address();
  };
  if (!isPathingCell(info->pathingCell.get()) ||
      !isPathingCell(form.parentCell.get()))
    return "Unexpected pathing cell wrapper type";
  const auto &actualCell =
      static_cast<const RE::PathingCell *>(info->pathingCell.get())
          ->pathingCellInfo;
  const auto &expectedCell =
      static_cast<const RE::PathingCell *>(form.parentCell.get())
          ->pathingCellInfo;
  if (!(actualCell == expectedCell))
    return std::format("Pathing cell key {:08X}/{:08X}, expected {:08X}/{:08X}",
                       actualCell.worldSpaceID, actualCell.cellID.formID,
                       expectedCell.worldSpaceID, expectedCell.cellID.formID);
  using ResolveCell =
      RE::TESObjectCELL *(*)(RE::Pathing *,
                             const RE::BSTSmartPointer<RE::BSPathingCell> &);
  const REL::Relocation<ResolveCell> resolve{REL::RelocationID(29870, 30686)};
  const auto manager = RE::Pathing::GetSingleton();
  if (!manager)
    return "Missing native pathing manager";
  const auto parent = resolve(manager, form.parentCell);
  if (!parent || resolve(manager, info->pathingCell) != parent)
    return "Info pathing cell does not resolve to the mesh's parent cell";
  if (!connected && (info->connectedDoorCount || info->adjacentMeshCount))
    return std::format("Island has {} doors / {} adjacent meshes",
                       info->connectedDoorCount, info->adjacentMeshCount);
  if (!connected && (info->connectedDoorsArray || info->adjacentMeshesArray))
    return "Island has unexpected connection arrays";
  if (bounds) {
    if (const auto reason = ValidateNativeBounds(*info, form, *bounds);
        !reason.empty())
      return reason;
  } else if (info->bounds)
    return "Unexpected compressed bounds";
  // Native bounds setup91226 sets bit0x20, and clears it when bounds are
  // removed. Validate that lifecycle flag independently of topology.
  const std::uint8_t expectedFlags = bounds ? 0x20 : 0;
  if (info->uiFlags != expectedFlags)
    return std::format("Native info flags {:02X}, expected {:02X} (bounds {})",
                       info->uiFlags, expectedFlags, bounds.has_value());
  if (!connected && info->preferredStartIndex)
    return std::format("Unexpected preferred index {} / flags {}",
                       info->preferredStartIndex, info->uiFlags);
  if (connected) {
    std::set<std::uint32_t> adjacent, preferredAdjacent;
    for (const auto &face : form.triangles)
      for (unsigned side = 0; side < 3; ++side)
        if (face.triangleFlags.underlying() & (1U << side)) {
          if (face.triangles[side] >= form.extraEdgeInfo.size())
            return "Invalid connected-info edge index";
          const auto &portal = form.extraEdgeInfo[face.triangles[side]].portal;
          if (!portal.otherMeshID || portal.triangle == 0xFFFF ||
              portal.otherMeshID == form.GetFormID())
            continue;
          adjacent.insert(portal.otherMeshID);
          if (face.triangleFlags.underlying() & 64U)
            preferredAdjacent.insert(portal.otherMeshID);
        }
    std::set<RE::BSPathingDoor *> doors;
    for (const auto &portal : form.doorPortals)
      if (portal.door)
        doors.insert(portal.door.get());
    if (info->adjacentMeshCount != adjacent.size() ||
        info->connectedDoorCount != doors.size() ||
        (info->adjacentMeshCount && !info->adjacentMeshesArray) ||
        (info->connectedDoorCount && !info->connectedDoorsArray))
      return "Native connected-info ownership counts differ from graph";
    std::set<std::uint32_t> actualAdjacent;
    for (unsigned index = 0; index < info->adjacentMeshCount; ++index) {
      const auto id = info->adjacentMeshesArray[index];
      if (!actualAdjacent.insert(id).second)
        return "Duplicate native info neighbor";
      if ((index >= info->preferredStartIndex) !=
          preferredAdjacent.contains(id))
        return "Native preferred-neighbor partition mismatch";
    }
    std::set<RE::BSPathingDoor *> actualDoors;
    for (unsigned index = 0; index < info->connectedDoorCount; ++index)
      if (!actualDoors.insert(info->connectedDoorsArray[index].get()).second)
        return "Duplicate native info door";
    if (actualAdjacent != adjacent)
      return "Native adjacent mesh IDs differ from graph topology";
    if (actualDoors != doors)
      return "Native connected door identities differ from graph topology";
    if (info->preferredStartIndex != adjacent.size() - preferredAdjacent.size())
      return std::format("Native preferred start {}, expected {}",
                         info->preferredStartIndex,
                         adjacent.size() - preferredAdjacent.size());
  }
  if (form.vertices.empty() || form.triangles.empty())
    return "Empty draft geometry";
  RE::NiPoint3 mean{};
  for (const auto &vertex : form.vertices) {
    mean.x += vertex.location.x;
    mean.y += vertex.location.y;
    mean.z += vertex.location.z;
  }
  const auto inverse = 1.0f / static_cast<float>(form.vertices.size());
  mean.x *= inverse;
  mean.y *= inverse;
  mean.z *= inverse;
  const auto &actual = info->approxLocation;
  std::size_t preferred{};
  for (const auto &face : form.triangles)
    preferred += (face.triangleFlags.underlying() & 64U) != 0;
  const auto ratio =
      static_cast<float>(preferred) / static_cast<float>(form.triangles.size());
  if (!std::isfinite(actual.x) || !std::isfinite(actual.y) ||
      !std::isfinite(actual.z) || std::abs(mean.x - actual.x) >= 0.1f ||
      std::abs(mean.y - actual.y) >= 0.1f ||
      std::abs(mean.z - actual.z) >= 0.1f)
    return std::format("Centroid ({}, {}, {}), expected ({}, {}, {})", actual.x,
                       actual.y, actual.z, mean.x, mean.y, mean.z);
  if (!std::isfinite(info->preferredPercent) ||
      std::abs(info->preferredPercent - ratio) >= 0.00001f)
    return std::format("Preferred fraction {}, expected {}",
                       info->preferredPercent, ratio);
  return {};
}
void NativeDraftInfo::PopulateConnected(RE::NavMesh &form) {
  if (!info || form.unk110 != info ||
      info->navMesh != static_cast<RE::BSNavmesh *>(&form))
    throw std::runtime_error("Detached info binding changed before population");
  using Populate = void (*)(RE::BSNavmeshInfo *, RE::NavMesh *);
  const REL::Relocation<Populate> populate{REL::RelocationID(29513, 30362)};
  populate(info, &form);
  bounds.reset();
  connected = true;
}
void NativeDraftInfo::PrepareBounds(RE::NavMesh &form) {
  if (!info || info->navMesh != static_cast<RE::BSNavmesh *>(&form))
    throw std::runtime_error("Native bounds info binding changed");
  bounds = BuildNativeBounds(*info, form);
  if (const auto reason = ValidateNativeBounds(*info, form, *bounds);
      !reason.empty())
    throw std::runtime_error(reason);
}
} // namespace SkyBuild::Navmesh
