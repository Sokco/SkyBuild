#include "navmesh/cuts/NativeCutCapture.h"
#include "navmesh/cuts/CutMaterialization.h"
#include <algorithm>
#include <cstring>
namespace SkyBuild::Navmesh {
namespace {
template <class T> T Read(const std::byte *data, std::size_t offset) {
  T result;
  std::memcpy(&result, data + offset, sizeof(result));
  return result;
}
// Native BSTArray header with explicitly traced element strides. Some
// CommonLib undo/auxiliary declarations are incomplete or have wrong strides.
struct ArrayView {
  const std::byte *data{};
  std::uint32_t count{};
};
ArrayView Array(const std::byte *base, std::size_t offset,
                std::size_t maximum) {
  const auto data = Read<const std::byte *>(base, offset);
  const auto capacity = Read<std::uint32_t>(base, offset + 8);
  const auto count = Read<std::uint32_t>(base, offset + 16);
  if (count > maximum || count > capacity || (count && !data))
    throw std::runtime_error(
        "Invalid native cut array or capture budget exceeded");
  return {data, count};
}
std::vector<std::uint16_t> Indices(const std::byte *base, std::size_t offset) {
  const auto array = Array(base, offset, 65535);
  std::vector<std::uint16_t> result(array.count);
  if (array.count)
    std::memcpy(result.data(), array.data, array.count * 2);
  return result;
}
} // namespace
NativeCutSnapshot CaptureNativeCuts(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &loaded) {
  if (loaded.size() > 1024)
    throw std::runtime_error("Native cut mesh budget exceeded");
  std::map<const RE::BSNavmesh *, std::uint32_t> identities;
  std::set<std::uint32_t> ids;
  for (const auto &mesh : loaded)
    if (mesh && (!identities.emplace(mesh.get(), mesh->QNavmeshID()).second ||
                 !ids.insert(mesh->QNavmeshID()).second || !mesh->QNavmeshID()))
      throw std::runtime_error("Repeated or missing native cut mesh identity");
  NativeCutSnapshot snapshot;
  const REL::Relocation<std::uintptr_t> undoVtable{REL::ID(246891)};
  std::size_t undoBudget{}, valueBudget{};
  for (const auto &mesh : loaded) {
    if (!mesh)
      continue;
    if (mesh->vertices.size() > 65535 || mesh->triangles.size() > 65535)
      throw std::runtime_error("Native cut geometry budget exceeded");
    const auto id = mesh->QNavmeshID();
    const auto raw = reinterpret_cast<const std::byte *>(mesh.get());
    const auto undos = Array(raw, 0xD0, 4096);
    undoBudget += undos.count;
    if (undoBudget > 4096)
      throw std::runtime_error("Native cut undo budget exceeded");
    auto &records = snapshot.records[id];
    std::vector<std::uint16_t> flags;
    flags.reserve(mesh->triangles.size());
    for (const auto &face : mesh->triangles)
      flags.push_back(face.triangleFlags.underlying());
    std::vector<CutOwnershipWitness> ownership;
    if (mesh->triangleToObstacleMap) {
      const auto map =
          reinterpret_cast<const std::byte *>(mesh->triangleToObstacleMap);
      const auto capacity = Read<std::uint32_t>(map, 12);
      const auto free = Read<std::uint32_t>(map, 16);
      if (capacity > 65536 || free > capacity ||
          (capacity && !Read<const void *>(map, 0x28)))
        throw std::runtime_error("Invalid native cut ownership map header");
      if (mesh->triangleToObstacleMap->size() > 65535)
        throw std::runtime_error("Native cut ownership budget exceeded");
      for (const auto &[triangle, data] : *mesh->triangleToObstacleMap)
        ownership.push_back(
            {triangle, reinterpret_cast<std::uintptr_t>(data.get())});
    }
    std::sort(
        ownership.begin(), ownership.end(),
        [](const auto &a, const auto &b) { return a.triangle < b.triangle; });
    snapshot.ownership[id] = ownership;
    for (std::size_t index = 0; index < undos.count; ++index) {
      const auto undo = Read<const std::byte *>(undos.data, index * 8);
      const auto lifecycle =
          undo ? DecodeCutLifecycle(Read<std::uint32_t>(undo, 0x10))
               : std::nullopt;
      if (!undo || Read<std::uintptr_t>(undo, 0) != undoVtable.address() ||
          !lifecycle)
        throw std::runtime_error(
            std::format("Unsupported native cut undo: mesh {:08X}, record {}, "
                        "pointer {}, vtable {:X}, state {}",
                        id, index, static_cast<const void *>(undo),
                        undo ? Read<std::uintptr_t>(undo, 0) : 0,
                        undo ? Read<std::uint32_t>(undo, 0x10) : 0));
      CutUndoRecord record;
      record.rawStatus = Read<std::uint32_t>(undo, 0x10);
      record.observedDataIdentity = Read<std::uintptr_t>(undo, 0x18);
      record.originalVertices = Read<std::uint32_t>(undo, 0x20);
      record.addedVertices = Read<std::uint32_t>(undo, 0x24);
      record.originalTriangles = Read<std::uint32_t>(undo, 0x28);
      record.addedTriangles = Read<std::uint32_t>(undo, 0x2C);
      const auto materialized = ObserveCutMaterialization(
          {record.rawStatus, record.originalVertices, record.addedVertices,
           record.originalTriangles, record.addedTriangles,
           static_cast<std::uint32_t>(mesh->vertices.size()),
           record.observedDataIdentity, flags, ownership});
      record.observedMaterialization = materialized.state;
      record.materializationReason = materialized.reason;
      // Raw native status is evidence, not a command to normalize another
      // representation's geometry. Publication adopts all owning references;
      // authored generation never recovers geometry by reversing this history.
      record.lifecycle = *lifecycle;
      if (!record.Applied() &&
          materialized.state != CutMaterialization::Applied) {
        // Restored records retain append-range provenance, but their borrowed
        // link pointers may name unloaded meshes. Never resolve or replay
        // historical cut effects that the native undo already consumed.
        records.push_back(std::move(record));
        continue;
      }
      record.affectedTriangles = Indices(undo, 0x30);
      record.auxiliaryIndices = Indices(undo, 0xA8);
      const auto edges = Array(undo, 0x48, 65535);
      for (std::size_t edge = 0; edge < edges.count; ++edge) {
        const auto saved = edges.data + edge * 12;
        CutSavedEdges decoded;
        decoded.triangle = Read<std::uint16_t>(saved, 0);
        for (unsigned side = 0; side < 3; ++side) {
          const auto external = Read<std::uint8_t>(saved, 2 + side);
          if (external > 1)
            throw std::runtime_error("Invalid native saved edge boolean");
          decoded.external[side] = external != 0;
          decoded.edges[side] = Read<std::uint16_t>(saved, 6 + side * 2);
        }
        record.restoredEdges.push_back(decoded);
      }
      const auto capacity = Read<std::uint32_t>(undo, 0x60 + 12);
      const auto free = Read<std::uint32_t>(undo, 0x60 + 16);
      if (capacity > 65536 || free > capacity)
        throw std::runtime_error("Invalid native saved obstacle map counters");
      record.hasSavedObstacleData = capacity != free;
      const auto links = Array(undo, 0x90, 8192);
      const auto resolve = [&](const std::byte *entry, std::size_t offset) {
        const auto pointer = Read<const RE::BSNavmesh *>(entry, offset);
        // Native local restoration uses null to mean this mesh.
        if (!pointer)
          return id;
        const auto found = identities.find(pointer);
        if (found == identities.end())
          throw std::runtime_error("Native cut refers to an unloaded mesh");
        return found->second;
      };
      for (std::size_t link = 0; link < links.count; ++link) {
        const auto entry = links.data + link * 0x38;
        record.links.push_back({resolve(entry, 0), resolve(entry, 0x18),
                                Read<std::uint16_t>(entry, 8),
                                Read<std::uint16_t>(entry, 0x20),
                                Read<std::uint32_t>(entry, 0x10),
                                Read<std::uint32_t>(entry, 0x28)});
      }
      valueBudget += record.affectedTriangles.size() +
                     record.auxiliaryIndices.size() + edges.count + links.count;
      if (valueBudget > 1000000)
        throw std::runtime_error("Native cut payload budget exceeded");
      records.push_back(std::move(record));
    }
    snapshot.auxiliaryIndices[id] = Indices(raw, 0xF0);
    snapshot.obstacleEntries[id] =
        mesh->triangleToObstacleMap ? mesh->triangleToObstacleMap->size() : 0;
  }
  return snapshot;
}
std::set<const RE::BSNavmesh *>
CaptureCutBorrowedTargets(const std::vector<const RE::BSNavmesh *> &sources) {
  std::set<const RE::BSNavmesh *> visited, targets;
  const REL::Relocation<std::uintptr_t> table{REL::ID(246891)};
  std::size_t records{}, edges{};
  for (const auto *mesh : sources) {
    if (!mesh || !visited.insert(mesh).second)
      continue;
    const auto undos =
        Array(reinterpret_cast<const std::byte *>(mesh), 0xD0, 4096);
    if ((records += undos.count) > 4096)
      throw std::runtime_error("Retirement undo census budget exceeded");
    for (std::size_t i = 0; i < undos.count; ++i) {
      const auto *undo = Read<const std::byte *>(undos.data, i * 8);
      if (!undo || Read<std::uintptr_t>(undo, 0) != table.address())
        throw std::runtime_error(
            "Retirement census encountered unsupported undo type");
      const auto links = Array(undo, 0x90, 8192);
      if ((edges += links.count) > 1000000)
        throw std::runtime_error("Retirement link census budget exceeded");
      for (std::size_t j = 0; j < links.count; ++j) {
        const auto *entry = links.data + j * 0x38;
        for (const auto offset : {0U, 0x18U}) {
          const auto *target = Read<const RE::BSNavmesh *>(entry, offset);
          targets.insert(target ? target : mesh);
        }
      }
    }
  }
  return targets;
}
} // namespace SkyBuild::Navmesh
