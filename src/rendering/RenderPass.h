#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace SkyBuild::Rendering {
inline HRESULT CreatePassState(ID3D11Device *device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> &context,
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> &state) {
  Microsoft::WRL::ComPtr<ID3D11Device1> extended;
  auto result = device->QueryInterface(IID_PPV_ARGS(&extended));
  if (FAILED(result))
    return result;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
  device->GetImmediateContext(&immediate);
  result = immediate.As(&context);
  if (FAILED(result))
    return result;
  const auto level = device->GetFeatureLevel();
  return extended->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                            __uuidof(ID3D11Device), nullptr, &state);
}

// Restore all engine bindings, including on an early return or exception.
class ScopedPass {
public:
  ScopedPass(ID3D11DeviceContext1 *context, ID3DDeviceContextState *state)
      : context(context) { context->SwapDeviceContextState(state, &previous); }
  ~ScopedPass() { context->SwapDeviceContextState(previous.Get(), nullptr); }
  ScopedPass(const ScopedPass &) = delete;
  ScopedPass &operator=(const ScopedPass &) = delete;
private:
  ID3D11DeviceContext1 *context;
  Microsoft::WRL::ComPtr<ID3DDeviceContextState> previous;
};

inline D3D11_BLEND_DESC AlphaBlend() {
  D3D11_BLEND_DESC desc{};
  auto &b = desc.RenderTarget[0];
  b.BlendEnable = true;
  b.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  b.BlendOp = D3D11_BLEND_OP_ADD;
  b.SrcBlendAlpha = D3D11_BLEND_ZERO;
  b.DestBlendAlpha = D3D11_BLEND_ONE;
  b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  return desc;
}

inline HRESULT CreateConstants(ID3D11Device *device, UINT bytes, ID3D11Buffer **buffer) {
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = bytes;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return device->CreateBuffer(&desc, nullptr, buffer);
}
}
