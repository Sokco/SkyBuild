#pragma once
#include <DirectXMath.h>
#include <cstddef>

namespace SkyBuild::Rendering::WorldFrame {
using Matrix = DirectX::XMFLOAT4X4;
struct alignas(16) CameraConstants {
  Matrix view, projection, viewProjection, unjittered;
  float unused[24 * 4]{};
  float position[4]{};
  float reserved[8]{};
  float scale[4]{};
};
static_assert(sizeof(CameraConstants) == 704);
static_assert(offsetof(CameraConstants, position) == 640);
static_assert(offsetof(CameraConstants, scale) == 688);

inline void BuildCameraMatrices(Matrix view, const Matrix &projection,
                                const Matrix &unjittered, float x, float y, float z,
                                CameraConstants &out) {
  // Project camera-relative positions, retaining the cached camera basis.
  view._41 = view._42 = view._43 = 0;
  view._14 = view._24 = view._34 = 0;
  view._44 = 1;
  using namespace DirectX;
  const auto v = XMLoadFloat4x4(&view);
  XMStoreFloat4x4(&out.view, XMMatrixTranspose(v));
  XMStoreFloat4x4(&out.projection, XMMatrixTranspose(XMLoadFloat4x4(&projection)));
  XMStoreFloat4x4(&out.viewProjection, XMMatrixTranspose(v * XMLoadFloat4x4(&projection)));
  XMStoreFloat4x4(&out.unjittered, XMMatrixTranspose(v * XMLoadFloat4x4(&unjittered)));
  out.position[0] = x;
  out.position[1] = y;
  out.position[2] = z;
}
}
