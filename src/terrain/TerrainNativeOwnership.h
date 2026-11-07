#pragma once
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace SkyBuild::Terrain {
// Populate freshly constructed native arrays using Havok's allocator helper.
// Its reserve clears the empty-array DONT_DEALLOCATE flag and records actual
// allocation capacity, allowing the native shape destructor to free storage.
template <class Array, class Reserve>
void AllocateEmptyHavokArray(Array &array, std::int32_t count,
                             Reserve &&reserve) {
  using T = typename Array::value_type;
  static_assert(std::is_trivially_destructible_v<T>);
  constexpr std::uint32_t CapacityMask = 0x3fffffff,
                          DontDeallocate = 0x80000000;
  if (count <= 0 ||
      std::uint64_t(count) >
          std::numeric_limits<std::int32_t>::max() / sizeof(T) ||
      array._data || array._size ||
      (std::uint32_t(array._capacityAndFlags) & CapacityMask))
    throw std::runtime_error("Invalid fresh Havok array allocation");
  reserve(&array, count, std::int32_t(sizeof(T)));
  if (!array._data)
    throw std::bad_alloc();
  const auto flags = std::uint32_t(array._capacityAndFlags);
  if ((flags & DontDeallocate) || (flags & CapacityMask) < std::uint32_t(count))
    throw std::runtime_error("Native Havok array allocation is not owned");
  for (std::int32_t i = 0; i < count; ++i) {
    std::construct_at(array._data + i);
    ++array._size;
  }
}

// A temporary material comes from the creating thread's ScrapHeap. Destroy
// texture smart pointers via its virtual destructor, then return its storage
// to that exact heap. Ordinary delete uses a different allocator.
template <class Material, class Heap> struct ScrapMaterialDeleter {
  Heap *heap;
  void operator()(Material *material) const {
    if (!material)
      return;
    material->~Material();
    heap->Deallocate(material);
  }
};
} // namespace SkyBuild::Terrain
