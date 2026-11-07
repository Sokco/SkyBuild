#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace SkyBuild::Terrain::BrushOverlay {
struct SceneFrameBinding {
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  UINT first{}, count{};
};
inline SceneFrameBinding ReadSceneFrame(ID3D11DeviceContext *context) {
  SceneFrameBinding result;
  if (!context)
    return result;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> extended;
  if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&extended))))
    extended->PSGetConstantBuffers1(12, 1, &result.buffer, &result.first,
                                    &result.count);
  else
    context->PSGetConstantBuffers(12, 1, &result.buffer);
  if (result.buffer) {
    D3D11_BUFFER_DESC desc{};
    result.buffer->GetDesc(&desc);
    const auto offset = result.first * 16;
    if (offset > desc.ByteWidth || 704 > desc.ByteWidth - offset ||
        (result.count && result.count < 44))
      result = {};
  }
  return result;
}
// One GPU-only snapshot for the current scene frame. Later postprocessing can
// rewrite the engine's live buffer; the overlay must retain the draw's values.
// Reuse storage, not historical frame contents, and preserve CB range bindings.
class SceneFrameSnapshot {
  SceneFrameBinding binding;
  unsigned bytes{};
  bool captured{};

public:
  bool Captured() const { return captured; }
  void Capture(ID3D11DeviceContext *context) {
    if (captured)
      return;
    const auto source = ReadSceneFrame(context);
    if (!source.buffer)
      return;
    D3D11_BUFFER_DESC desc{};
    source.buffer->GetDesc(&desc);
    if (desc.ByteWidth > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16)
      return;
    Microsoft::WRL::ComPtr<ID3D11Device> device, owner;
    context->GetDevice(&device);
    if (binding.buffer)
      binding.buffer->GetDevice(&owner);
    if (bytes != desc.ByteWidth || owner != device) {
      binding = {};
      bytes = 0;
      D3D11_BUFFER_DESC snapshot{};
      snapshot.ByteWidth = desc.ByteWidth;
      snapshot.Usage = D3D11_USAGE_DEFAULT;
      snapshot.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (FAILED(device->CreateBuffer(&snapshot, nullptr, &binding.buffer)))
        return;
      bytes = desc.ByteWidth;
    }
    context->CopyResource(binding.buffer.Get(), source.buffer.Get());
    binding.first = source.first;
    binding.count = source.count;
    captured = true;
  }
  SceneFrameBinding Current() const {
    return captured ? binding : SceneFrameBinding{};
  }
  void FrameBoundary() { captured = false; }
};
} // namespace SkyBuild::Terrain::BrushOverlay
