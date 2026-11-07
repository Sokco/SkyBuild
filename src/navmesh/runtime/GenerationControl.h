#pragma once
namespace SkyBuild::Navmesh {
constexpr bool GenerationMayAdvance(bool command, bool sampling,
                                    bool waitingForCollision, bool worker) {
  return (command || sampling) && !waitingForCollision && !worker;
}
} // namespace SkyBuild::Navmesh
