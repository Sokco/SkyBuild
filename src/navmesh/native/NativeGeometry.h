#pragma once
#include "navmesh/generation/MeshBuilder.h"

namespace SkyBuild::Navmesh {
class NativeUpdateLease;
class NativeFormDraft;
class NativeGraphDraft;
class NativePublication;
// Allocate on the game thread, before pausing navigation. This owns detached
// engine-allocated buffers. It is never registered or installed by
// construction.
class NativeGeometry {
public:
  explicit NativeGeometry(const Mesh &mesh, std::uint32_t gridSize = 16);
  // Exact detached clone: preserve native indices, link flags, traversal words
  // and the existing spatial buckets. Auxiliary records remain on the source;
  // this constructor does not remap them or authorize an edited installation.
  explicit NativeGeometry(const RE::BSNavmesh &source);
  [[nodiscard]] bool Matches(const RE::BSNavmesh &source) const noexcept;
  NativeGeometry(const NativeGeometry &) = delete;
  NativeGeometry &operator=(const NativeGeometry &) = delete;
  [[nodiscard]] bool Valid() const { return error.empty(); }
  [[nodiscard]] const std::string &Error() const { return error; }

private:
  NativeGeometry() =
      default; // Empty storage for reversible retired-core evacuation.
  friend class NativePublication;
  friend class NativeUpdateLease;
  friend class NativeFormDraft;
  friend class NativeGraphDraft;
  // Only native publication owners may exchange buffers. Keeping this
  // private prevents callers from treating a game-thread update as a barrier.
  // A second exchange restores the original allocations without copying.
  void Exchange(RE::BSNavmesh &mesh) noexcept;
  RE::BSTArray<RE::BSNavmeshVertex> vertices;
  RE::BSTArray<RE::BSNavmeshTriangle> triangles;
  RE::BSNavmeshGrid grid{};
  std::string error;
};
} // namespace SkyBuild::Navmesh
