#include "navmesh/native/NativeGeometry.h"
#include "navmesh/generation/SpatialIndex.h"
#include <cstring>
#include <utility>

namespace SkyBuild::Navmesh {
namespace {
// SimpleArray has a destructor but no ownership-transferring move operation.
// Use its protected, typed data member through a pointer-to-member. This swaps
// ownership without layout offsets, derived-object casts, or shallow copies.
struct BucketOwnership : RE::SimpleArray<RE::BSTArray<std::uint16_t>> {
  static void
  Exchange(RE::SimpleArray<RE::BSTArray<std::uint16_t>> &a,
           RE::SimpleArray<RE::BSTArray<std::uint16_t>> &b) noexcept {
    const auto member = &BucketOwnership::_data;
    std::swap(a.*member, b.*member);
  }
};
struct HeapOwnership : RE::BSTArrayHeapAllocator {
  static void Exchange(RE::BSTArrayHeapAllocator &a,
                       RE::BSTArrayHeapAllocator &b) noexcept {
    const auto set = &HeapOwnership::set_allocator_traits;
    auto *data = a.data();
    const auto capacity = a.capacity();
    (a.*set)(b.data(), b.capacity(), 0);
    (b.*set)(data, capacity, 0);
  }
};
template <class T>
void ExchangeArrayOwnership(RE::BSTArray<T> &a, RE::BSTArray<T> &b) noexcept {
  // Exchange the public allocator/size base subobjects. This transfers the
  // entire allocation without touching elements (some native NI types are
  // intentionally incomplete in CommonLib). No layout casts or memcpy.
  HeapOwnership::Exchange(static_cast<RE::BSTArrayHeapAllocator &>(a),
                          static_cast<RE::BSTArrayHeapAllocator &>(b));
  std::swap(static_cast<RE::BSTArrayBase &>(a),
            static_cast<RE::BSTArrayBase &>(b));
}
void ExchangeGrid(RE::BSNavmeshGrid &a, RE::BSNavmeshGrid &b) noexcept {
  std::swap(a.gridSize, b.gridSize);
  std::swap(a.columnSectionLen, b.columnSectionLen);
  std::swap(a.rowSectionLen, b.rowSectionLen);
  std::swap(a.gridBoundsMin, b.gridBoundsMin);
  std::swap(a.gridBoundsMax, b.gridBoundsMax);
  BucketOwnership::Exchange(a.gridData, b.gridData);
}
} // namespace
NativeGeometry::NativeGeometry(const RE::BSNavmesh &source) {
  const auto &lookup = source.meshGrid;
  if (source.vertices.size() > 65535 || source.triangles.size() > 65535 ||
      lookup.gridSize > 64 ||
      lookup.gridData.size() !=
          static_cast<std::size_t>(lookup.gridSize) * lookup.gridSize) {
    error = "Invalid native geometry or spatial grid dimensions";
    return;
  }
  std::size_t entries{};
  for (const auto &bucket : lookup.gridData) {
    entries += bucket.size();
    if (entries > 4'000'000) {
      error = "Native spatial grid exceeds staging budget";
      return;
    }
    for (const auto triangle : bucket)
      if (triangle >= source.triangles.size()) {
        error = "Native spatial grid contains an invalid triangle index";
        return;
      }
  }
  for (const auto &vertex : source.vertices)
    if (!std::isfinite(vertex.location.x) ||
        !std::isfinite(vertex.location.y) ||
        !std::isfinite(vertex.location.z)) {
      error = "Native geometry contains a non-finite vertex";
      return;
    }
  // BSTArray copies allocate their own engine buffers. SimpleArray must be
  // copied bucket-by-bucket because its implicit copy would share ownership.
  vertices = source.vertices;
  triangles = source.triangles;
  grid.gridSize = lookup.gridSize;
  grid.columnSectionLen = lookup.columnSectionLen;
  grid.rowSectionLen = lookup.rowSectionLen;
  grid.gridBoundsMin = lookup.gridBoundsMin;
  grid.gridBoundsMax = lookup.gridBoundsMax;
  grid.gridData.resize(lookup.gridData.size());
  for (std::size_t i = 0; i < lookup.gridData.size(); ++i)
    grid.gridData[i] = lookup.gridData[i];
}
namespace {
template <class Array> bool SameArray(const Array &a, const Array &b) noexcept {
  using Value = typename Array::value_type;
  static_assert(std::is_trivially_copyable_v<Value>);
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(Value)) == 0);
}
} // namespace
bool NativeGeometry::Matches(const RE::BSNavmesh &source) const noexcept {
  const auto &lookup = source.meshGrid;
  if (!Valid() || !SameArray(vertices, source.vertices) ||
      !SameArray(triangles, source.triangles) ||
      grid.gridSize != lookup.gridSize ||
      grid.columnSectionLen != lookup.columnSectionLen ||
      grid.rowSectionLen != lookup.rowSectionLen ||
      std::memcmp(&grid.gridBoundsMin, &lookup.gridBoundsMin,
                  sizeof(grid.gridBoundsMin)) != 0 ||
      std::memcmp(&grid.gridBoundsMax, &lookup.gridBoundsMax,
                  sizeof(grid.gridBoundsMax)) != 0 ||
      grid.gridData.size() != lookup.gridData.size())
    return false;
  for (std::size_t i = 0; i < grid.gridData.size(); ++i)
    if (!SameArray(grid.gridData[i], lookup.gridData[i]))
      return false;
  return true;
}
NativeGeometry::NativeGeometry(const Mesh &mesh, std::uint32_t gridSize) {
  auto lookup = BuildSpatialIndex(mesh, gridSize);
  if (!lookup.Valid()) {
    error = std::move(lookup.error);
    return;
  }
  for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
    const auto &triangle = mesh.triangles[index];
    if (triangle.flags & 7U) {
      error = "Native geometry needs authored external portal mapping";
      return;
    }
    for (unsigned side = 0; side < 3; ++side) {
      const auto neighbor = triangle.neighbors[side];
      if (neighbor < -1 ||
          neighbor >= static_cast<std::int32_t>(mesh.triangles.size())) {
        error = "Invalid native geometry adjacency";
        return;
      }
      if (neighbor >= 0) {
        const auto &other = mesh.triangles[neighbor];
        bool reciprocal = false;
        for (unsigned edge = 0; edge < 3; ++edge)
          reciprocal |=
              other.neighbors[edge] == static_cast<std::int32_t>(index) &&
              other.vertices[edge] == triangle.vertices[(side + 1) % 3] &&
              other.vertices[(edge + 1) % 3] == triangle.vertices[side];
        if (!reciprocal) {
          error = "Non-reciprocal native geometry adjacency";
          return;
        }
      }
    }
  }
  for (const auto &vertex : mesh.vertices)
    vertices.push_back({{vertex.x, vertex.y, vertex.z}});
  for (const auto &triangle : mesh.triangles) {
    RE::BSNavmeshTriangle native{};
    for (unsigned i = 0; i < 3; ++i) {
      native.vertices[i] = static_cast<std::uint16_t>(triangle.vertices[i]);
      native.triangles[i] =
          triangle.neighbors[i] < 0
              ? 0xFFFF
              : static_cast<std::uint16_t>(triangle.neighbors[i]);
    }
    native.triangleFlags =
        static_cast<RE::BSNavmeshTriangle::TriangleFlag>(triangle.flags);
    triangles.push_back(native);
  }
  const auto &index = lookup.index;
  grid.gridSize = index.size;
  grid.columnSectionLen = index.columnLength;
  grid.rowSectionLen = index.rowLength;
  grid.gridBoundsMin = {index.minimum.x, index.minimum.y, index.minimum.z};
  grid.gridBoundsMax = {index.maximum.x, index.maximum.y, index.maximum.z};
  grid.gridData.resize(index.buckets.size());
  for (std::size_t i = 0; i < index.buckets.size(); ++i)
    for (const auto triangle : index.buckets[i])
      grid.gridData[i].push_back(triangle);
}
void NativeGeometry::Exchange(RE::BSNavmesh &mesh) noexcept {
  // Exchange allocator ownership directly: rollback must not invoke an array
  // move-assignment/release path, even for a library type lacking noexcept.
  ExchangeArrayOwnership(vertices, mesh.vertices);
  ExchangeArrayOwnership(triangles, mesh.triangles);
  ExchangeGrid(grid, mesh.meshGrid);
}
} // namespace SkyBuild::Navmesh
