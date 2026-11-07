#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace SkyBuild::DiffusePreview {
struct Surface {
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> diffuse;
  RE::NiTransform local;
  unsigned stride{}, uvOffset{}, indexCount{};
  bool fullPrecision{}, hasUV{}, twoSided{}, blendAlpha{};
  int alphaTest{-1};
  float threshold{}, alpha{1};
  float uv[4]{1, 1, 0, 0};
};
struct Model {
  // Retain the detached native asset so its renderer allocations cannot be
  // recycled while the private pass is borrowing its buffers and textures.
  RE::NiPointer<RE::NiNode> owner;
  std::vector<Surface> surfaces;
};
struct Instance {
  std::shared_ptr<const Model> model;
  RE::NiTransform placement;
  RE::NiColor tint;
  float opacity{1}, tintStrength{}, depthBias{};
};
std::shared_ptr<const Model> Load(RE::TESBoundObject *base);
void Publish(const void *owner, const Instance &instance);
void Remove(const void *owner);
void Render(); // Called from the existing render hook, before FrameBoundary.
} // namespace SkyBuild::DiffusePreview
