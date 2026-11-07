#pragma once
namespace SkyBuild::Physics {
// Body transforms can already contain scene placement. Cancel it before
// attaching the physical shape offset to the model's collision node.
template <class Transform>
Transform ColliderLocalPose(const Transform &nodeRelative,
                            const Transform &graphicsPose,
                            const Transform &physicalPose) {
  return nodeRelative * graphicsPose.Invert() * physicalPose;
}
} // namespace SkyBuild::Physics
