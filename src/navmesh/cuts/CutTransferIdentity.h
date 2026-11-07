#pragma once
#include <array>
#include <cstddef>
#include <cstring>
namespace SkyBuild::Navmesh {
// Cut data +0x20: XYZ, quaternion, inverse transform and local/world bounds.
// XYZ's fourth word and the trailing alignment bytes are uninitialized in
// constructor 91378. +0x94 is native processing bookkeeping; +0x98/99 are
// flags. A transfer also requires the exact retained collision body and
// auxiliary.
inline bool SameCutTransferShape(const std::array<std::byte, 0x80> &a,
                                 const std::array<std::byte, 0x80> &b) {
  return !std::memcmp(a.data(), b.data(), 0x0C) &&
         !std::memcmp(a.data() + 0x10, b.data() + 0x10, 0x64) &&
         !std::memcmp(a.data() + 0x78, b.data() + 0x78, 2);
}
} // namespace SkyBuild::Navmesh
