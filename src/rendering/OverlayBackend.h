#pragma once
#include "rendering/WorldFrame.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <string_view>
#include <wrl/client.h>

namespace SkyBuild::Rendering {
struct Program {
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel;
  Microsoft::WRL::ComPtr<ID3DBlob> vertexCode;
};
class OverlayBackend {
public:
  ID3D11Device *Device() const { return device.Get(); }
  ID3D11DeviceContext *Context() const { return context.Get(); }
  ID3D11RenderTargetView *Target() const { return target; }
  unsigned Width() const { return width; }
  unsigned Height() const { return height; }
  HRESULT CreateProgram(std::string_view source, const char *name, Program &program,
                        const char *vertexEntry = "VS", const char *pixelEntry = "PS");
  HRESULT CreateConstants(UINT bytes, ID3D11Buffer **buffer);
  void Draw(ID3D11DeviceContext *immediate,
            const WorldFrame::Frame &frame);

  struct States {
    Microsoft::WRL::ComPtr<ID3D11BlendState> alpha, colorOff, maximum;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> writeDepth, equalDepth, noDepth;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> cull, twoSided;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> white;
  } states;

private:
  void Initialize(ID3D11DeviceContext *immediate);
  void BindFrame(const WorldFrame::Frame &frame);
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
  Microsoft::WRL::ComPtr<ID3DDeviceContextState> state;
  ID3D11RenderTargetView *target{};
  unsigned width{}, height{};
  bool initialized{}, failed{};
};
}
