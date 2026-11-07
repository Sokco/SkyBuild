#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SkyBuild::Terrain::Grass {
// Original renderer uploads, kept separate from the terrain-adjusted GPU data.
// Tracks whole valid records across partial uploads and GPU compaction copies.
struct BufferShadow {
  std::size_t stride;
  std::vector<std::byte> source, gpu;
  std::vector<bool> valid;
  static constexpr std::size_t BlockRecords = 256;
  std::vector<std::uint64_t> changed;
  std::uint64_t epoch{1};
  BufferShadow(std::size_t size, std::size_t stride)
      : stride(stride), source(size), gpu(size), valid(size / stride),
        changed((size / stride + BlockRecords - 1) / BlockRecords) {}
  void Mark(std::size_t offset, std::size_t size) {
    ++epoch;
    if (size)
      std::fill(changed.begin() + offset / stride / BlockRecords,
                changed.begin() +
                    ((offset + size - 1) / stride / BlockRecords) + 1,
                epoch);
  }
  bool Write(std::size_t offset, std::size_t size, const void *bytes) {
    if (!bytes || offset > source.size() || size > source.size() - offset ||
        offset % stride || size % stride)
      return false;
    std::memcpy(source.data() + offset, bytes, size);
    std::memcpy(gpu.data() + offset, bytes, size);
    std::fill(valid.begin() + offset / stride,
              valid.begin() + (offset + size) / stride, true);
    Mark(offset, size);
    return true;
  }
  bool Copy(std::size_t dest, const BufferShadow &from, std::size_t start,
            std::size_t size) {
    if (stride != from.stride || dest % stride || start % stride ||
        size % stride || dest > source.size() || size > source.size() - dest ||
        start > from.source.size() || size > from.source.size() - start)
      return false;
    const std::vector<bool> known(from.valid.begin() + start / stride,
                                  from.valid.begin() + (start + size) / stride);
    std::memmove(source.data() + dest, from.source.data() + start, size);
    std::memmove(gpu.data() + dest, from.gpu.data() + start, size);
    std::copy(known.begin(), known.end(), valid.begin() + dest / stride);
    Mark(dest, size);
    return true;
  }
};
} // namespace SkyBuild::Terrain::Grass
