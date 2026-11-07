#pragma once
#include <cassert>
#include <cstring>
namespace SkyBuild::Navmesh {
// Native single-pointer strong-reference ABI. Caller exclusively owns the slot
// and retains BOTH inputs separately, so releasing its old reference cannot
// destroy an object or run a native callback. Not an engine exclusion
// mechanism.
template <class T>
void ReplaceRetainedReferenceSlot(void *storage, T *replacement) noexcept {
  T *prior{};
  std::memcpy(&prior, storage, sizeof(prior));
  assert(prior && replacement);
  replacement->IncRefCount();
  std::memcpy(storage, &replacement, sizeof(replacement));
  prior->DecRefCount();
}
} // namespace SkyBuild::Navmesh
