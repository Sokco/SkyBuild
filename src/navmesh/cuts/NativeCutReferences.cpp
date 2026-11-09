#include "navmesh/cuts/NativeCutReferences.h"
#include <cstring>
#include <type_traits>
namespace SkyBuild::Navmesh {
namespace {
template <class T> T Read(const void *value, std::size_t offset) {
  T result;
  std::memcpy(&result, static_cast<const std::byte *>(value) + offset,
              sizeof(result));
  return result;
}
using Owners =
    std::remove_pointer_t<decltype(RE::BSNavmesh::triangleToObstacleMap)>;
static_assert(sizeof(Owners) == 0x30);
void ValidateOwners(const Owners &owners) {
  const auto capacity = Read<std::uint32_t>(&owners, 12);
  const auto free = Read<std::uint32_t>(&owners, 16);
  if (capacity > 65536 || free > capacity ||
      (capacity && !Read<const void *>(&owners, 0x28)))
    throw std::runtime_error("Invalid cut reference ownership map");
}
} // namespace
CutReferenceGraph CaptureNativeCutReferences(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &cohort,
    const std::vector<RE::NiPointer<RE::NiRefObject>> &retainedInputs,
    CutMembershipCapture membershipScope,
    const std::map<const RE::BSNavmesh *, const RE::BSNavmeshInfo *>
        *retainedBindings) {
  if (cohort.size() > 1024)
    throw std::runtime_error("Cut reference cohort budget exceeded");
  CutReferenceGraph result;
  std::set<std::uint64_t> infos, meshes;
  // All pointers inspected below are retained by the caller's meshes, their
  // undo arrays or owner maps. This retention does not prevent concurrent
  // writes.
  std::map<std::uint64_t, RE::NiPointer<RE::NiRefObject>> data;
  for (const auto &mesh : cohort) {
    if (!mesh || !mesh->QNavmeshID() ||
        !meshes.insert(mesh->QNavmeshID()).second)
      throw std::runtime_error("Missing or duplicate cut reference mesh");
    const auto *info = static_cast<const RE::BSNavmeshInfo *>(mesh->unk110);
    if (retainedBindings) {
      const auto retained = retainedBindings->find(mesh.get());
      if (retained != retainedBindings->end()) {
        if (!retained->second || (info && info != retained->second) ||
            retained->second->navMesh != mesh.get() ||
            retained->second->navMeshID != mesh->QNavmeshID())
          throw std::runtime_error(
              "Retained cut reference binding disagrees with native identity");
        info = retained->second;
      }
    }
    if (!info || !infos.insert(reinterpret_cast<std::uintptr_t>(info)).second)
      throw std::runtime_error("Missing or duplicate cut reference info");
  }
  const REL::Relocation<std::uintptr_t> undoTable{REL::RelocationID(295732, 246891)};
  const REL::Relocation<std::uintptr_t> dataTable{REL::RelocationID(294665, 245920)};
  if (retainedInputs.size() > 4096)
    throw std::runtime_error("Retained cut input budget exceeded");
  // Builder-retained inputs remain part of the graph even if no current
  // triangle/undo refers to them. Their memberships must not disappear.
  for (const auto &input : retainedInputs) {
    if (!input || Read<std::uintptr_t>(input.get(), 0) != dataTable.address())
      throw std::runtime_error("Unsupported retained cut input type");
    const auto identity = reinterpret_cast<std::uintptr_t>(input.get());
    data.try_emplace(identity, input);
    result.data.insert(identity);
  }
  const auto add = [&](std::uint64_t mesh, CutReferenceKind kind,
                       std::uint64_t record, std::uint64_t element,
                       RE::NiRefObject *value) {
    if (!value || Read<std::uintptr_t>(value, 0) != dataTable.address())
      throw std::runtime_error("Unsupported retained cut reference type");
    const auto identity = reinterpret_cast<std::uintptr_t>(value);
    if (result.references.size() >= 262144 ||
        (data.size() >= 4096 && !data.contains(identity)))
      throw std::runtime_error("Cut reference payload budget exceeded");
    data.try_emplace(identity, RE::NiPointer<RE::NiRefObject>{value});
    result.data.insert(identity);
    result.references.push_back({mesh, record, element, identity, kind});
  };
  for (const auto &mesh : cohort) {
    const auto id = mesh->QNavmeshID();
    if (mesh->obstacles.size() > 4096)
      throw std::runtime_error("Cut reference undo budget exceeded");
    if (mesh->triangleToObstacleMap) {
      ValidateOwners(*mesh->triangleToObstacleMap);
      for (const auto &[face, owner] : *mesh->triangleToObstacleMap) {
        if (face >= mesh->triangles.size())
          throw std::runtime_error("Cut owner outside current geometry");
        add(id, CutReferenceKind::TriangleOwner, 0, face,
            reinterpret_cast<RE::NiRefObject *>(owner.get()));
      }
    }
    for (std::uint32_t index = 0; index < mesh->obstacles.size(); ++index) {
      const auto *undo = mesh->obstacles[index].get();
      if (!undo || Read<std::uintptr_t>(undo, 0) != undoTable.address())
        throw std::runtime_error("Unsupported cut reference undo type");
      add(id, CutReferenceKind::UndoInput, index, 0,
          Read<RE::NiRefObject *>(undo, 0x18));
      // Native saved map is inline at+60; CommonLib's undo class is incomplete.
      const auto &saved = *reinterpret_cast<const Owners *>(
          reinterpret_cast<const std::byte *>(undo) + 0x60);
      ValidateOwners(saved);
      for (const auto &[face, owner] : saved)
        add(id, CutReferenceKind::SavedTriangleOwner, index, face,
            reinterpret_cast<RE::NiRefObject *>(owner.get()));
    }
  }
  for (const auto &[identity, value] : data) {
    const auto count = Read<std::uint32_t>(value.get(), 0xB0);
    const auto capacity = Read<std::uint32_t>(value.get(), 0xA8);
    const auto array = Read<RE::BSNavmeshInfo *const *>(value.get(), 0xA0);
    if (count > 1024 || count > capacity || (count && !array))
      throw std::runtime_error("Invalid cut reference affected-info array");
    auto &membership = result.affectedInfos[identity];
    for (std::uint32_t index = 0; index < count; ++index) {
      const auto info = reinterpret_cast<std::uintptr_t>(array[index]);
      if (!info ||
          (membershipScope == CutMembershipCapture::CohortOnly &&
           !infos.contains(info)) ||
          !membership.insert(info).second)
        throw std::runtime_error(
            "Cut reference membership outside cohort or repeated");
    }
  }
  result.complete = true;
  const auto error = ValidateCutReferenceGraph(result);
  if (!error.empty())
    throw std::runtime_error(error);
  return result;
}
} // namespace SkyBuild::Navmesh
