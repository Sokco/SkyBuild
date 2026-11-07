#include "navmesh/native/NativeTopologyCapture.h"
#include <cstring>
namespace SkyBuild::Navmesh {
AuthoredMesh CaptureNativeTopology(const RE::BSNavmesh &mesh,
                                   std::uint32_t formID) {
  if (!formID || mesh.vertices.size() > 65535 ||
      mesh.triangles.size() > 65535 || mesh.extraEdgeInfo.size() > 65535 ||
      mesh.coverArray.size() > 1000000 || mesh.doorPortals.size() > 65535 ||
      mesh.closedDoors.size() > 65535)
    throw std::runtime_error("Native topology capture budget exceeded");
  AuthoredMesh copy;
  copy.formID = formID;
  copy.nativeDependencies =
      !mesh.obstacles.empty() ||
      (mesh.triangleToObstacleMap && !mesh.triangleToObstacleMap->empty()) ||
      !mesh.unk0F0.empty();
  for (const auto &vertex : mesh.vertices)
    copy.vertices.push_back(
        {vertex.location.x, vertex.location.y, vertex.location.z});
  for (const auto &triangle : mesh.triangles) {
    AuthoredTriangle value;
    for (unsigned edge = 0; edge < 3; ++edge) {
      value.vertices[edge] = triangle.vertices[edge];
      value.edges[edge] = triangle.triangles[edge];
    }
    std::memcpy(&value.flags, &triangle.triangleFlags, sizeof(value.flags));
    std::memcpy(&value.traversal, &triangle.traversalFlags,
                sizeof(value.traversal));
    copy.triangles.push_back(value);
  }
  for (const auto &record : mesh.extraEdgeInfo) {
    AuthoredPortal value;
    std::memcpy(&value.type, &record.type, sizeof(value.type));
    value.targetMesh = record.portal.otherMeshID;
    value.targetTriangle = record.portal.triangle;
    // The native loader and copier omit bytes10/11 for EVERY edge type.
    // Keep runtime/padding bytes out of portable geometry and comparisons.
    copy.portals.push_back(value);
  }
  // The native serializer/builder use uint16 triangle indices. CommonLib's
  // eight-byte BSNavmeshCoverEdge declaration does not match the actual array.
  const auto data = reinterpret_cast<const std::byte *>(mesh.coverArray.data());
  for (std::size_t index = 0; index < mesh.coverArray.size(); ++index) {
    std::uint16_t triangle;
    std::memcpy(&triangle, data + index * sizeof(triangle), sizeof(triangle));
    copy.coverTriangles.push_back(triangle);
  }
  for (const auto &record : mesh.doorPortals)
    copy.doorTriangles.push_back(record.owningTriangleIndex);
  for (const auto &record : mesh.closedDoors)
    copy.closedDoorTriangles.push_back(record.triangleIndex);
  return copy;
}
} // namespace SkyBuild::Navmesh
