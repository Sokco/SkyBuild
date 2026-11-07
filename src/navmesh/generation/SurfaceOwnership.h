#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include <map>
namespace SkyBuild::Navmesh {
struct SurfaceOwnership {
  std::vector<std::uint32_t> owners;
  // Disconnected sampled islands need genuinely new native forms. Never assign
  // them to an existing mesh merely because it is nearby in world coordinates.
  std::vector<std::vector<std::uint32_t>> newFormComponents;
  std::string error;
  [[nodiscard]] bool Valid() const { return error.empty(); }
};
[[nodiscard]] SurfaceOwnership
AssignConnectedOwners(const Mesh &mesh,
                      const std::map<std::uint32_t, std::size_t> &sourceCounts);
} // namespace SkyBuild::Navmesh
