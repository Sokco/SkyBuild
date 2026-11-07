#pragma once
#include "navmesh/generation/MeshPartition.h"
#include <set>
namespace SkyBuild::Navmesh {
struct DoorAnchors {
  std::vector<std::optional<Point>> doors, closedDoors;
  // Native placeholder records: no reference handle or initialized locations.
  std::vector<std::uint16_t> unboundDoors, unboundClosedDoors;
  bool operator==(const DoorAnchors &) const = default;
};
struct TopologyProfile {
  std::vector<std::pair<std::string, double>> stages;
};
using NativeDoorAnchors = std::map<std::uint32_t, DoorAnchors>;
[[nodiscard]] std::optional<Point> ProjectDoorAnchor(const AuthoredMesh &mesh,
                                                     std::uint16_t triangle,
                                                     Point point);
// A native door access point must remain on its authored surface.
[[nodiscard]] bool ContainsDoorAnchor(const AuthoredMesh &mesh,
                                      std::uint16_t triangle, Point point);
// Rebuilds ordinary authored portals, including incoming links from unchanged
// meshes, and transfers traversal/cover/door indices through source provenance.
// Input is owned data only. Unsupported cut records, special links, missing
// loaded destination indices and missing door access points reject the whole
// result. Unloaded authored destinations become owned deferred descriptors;
// their runtime edges are closed until reconstruction with a loaded neighbor.
// Native
// door anchors remap subdivided/sampled surfaces after all boundary refinement;
// unchanged door faces do not need an anchor. Explicitly unbound native
// placeholders retain all surviving authored descendants with an owned record
// mapping; they never select an arbitrary sampled face or access coordinate.
// A shared boundary plan collects both sides' endpoints before subdivisions
// are applied. Reverse provenance maps update only fragments of a split face;
// boundary refinement never restarts a graph-wide discovery loop.
// Ordinary return links align subdivisions by edge parameters, allowing
// authored edges with distinct coordinates. Untouched ledges retain payloads;
// edited ledges/enable-disable links remain rejected.
[[nodiscard]] PartitionedMesh PreserveGeneratedTopology(
    const Mesh &generated, const std::vector<std::uint32_t> &owners,
    const std::map<std::uint32_t, AuthoredMesh> &originals,
    const std::set<std::uint32_t> &replaced,
    const NativeDoorAnchors &doorAnchors = {},
    TopologyProfile *profile = nullptr);
} // namespace SkyBuild::Navmesh
