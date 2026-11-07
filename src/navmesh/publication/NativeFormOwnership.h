#pragma once
// NavMesh inherits two allocator families. A final full-form intrusive release
// must dispatch its primary TESForm destructor, as native cell arrays do.
namespace SkyBuild::Navmesh {
inline void ReleaseNativeFormReference(RE::NavMesh *value) {
  if (value->DecRef() == 0)
    delete static_cast<RE::TESForm *>(value);
}
} // namespace SkyBuild::Navmesh

template <>
inline void
RE::BSTSmartPointerIntrusiveRefCount<RE::NavMesh>::Release(RE::NavMesh *value) {
  SkyBuild::Navmesh::ReleaseNativeFormReference(value);
}
