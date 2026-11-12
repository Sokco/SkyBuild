#include "rendering/OverlayBackend.h"
#include "rendering/RenderPass.h"
#include "building/preview/DiffusePreview.h"
#include "terrain/brush-overlay/BrushOverlay.h"

namespace SkyBuild::Rendering {
namespace {
void Check(HRESULT result) {
  if (FAILED(result))
    throw std::runtime_error(std::format("DX11 {:08X}", unsigned(result)));
}
}
void OverlayBackend::Initialize(ID3D11DeviceContext *immediate) {
  Microsoft::WRL::ComPtr<ID3D11Device> current;
  immediate->GetDevice(&current);
  if (current != device) {
    *this = {};
    device = current;
  }
  if (initialized || failed)
    return;
  Check(CreatePassState(device.Get(), context, state));
  auto blend = AlphaBlend();
  Check(device->CreateBlendState(&blend, &states.alpha));
  blend.RenderTarget[0].RenderTargetWriteMask = 0;
  Check(device->CreateBlendState(&blend, &states.colorOff));
  blend = AlphaBlend();
  blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
  Check(device->CreateBlendState(&blend, &states.maximum));
  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = true;
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  Check(device->CreateDepthStencilState(&depth, &states.writeDepth));
  depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  depth.DepthFunc = D3D11_COMPARISON_EQUAL;
  Check(device->CreateDepthStencilState(&depth, &states.equalDepth));
  depth.DepthEnable = false;
  depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
  Check(device->CreateDepthStencilState(&depth, &states.noDepth));
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_BACK;
  raster.FrontCounterClockwise = true;
  raster.DepthClipEnable = true;
  Check(device->CreateRasterizerState(&raster, &states.cull));
  raster.CullMode = D3D11_CULL_NONE;
  Check(device->CreateRasterizerState(&raster, &states.twoSided));
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_ANISOTROPIC;
  sampler.MaxAnisotropy = 8;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  Check(device->CreateSamplerState(&sampler, &states.sampler));
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const unsigned white = 0xFFFFFFFF;
  D3D11_SUBRESOURCE_DATA data{&white, 4, 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Check(device->CreateTexture2D(&desc, &data, &texture));
  Check(device->CreateShaderResourceView(texture.Get(), nullptr, &states.white));
  initialized = true;
}
HRESULT OverlayBackend::CreateProgram(std::string_view source, const char *name,
                                     Program &program, const char *vertexEntry,
                                     const char *pixelEntry) {
  Program next;
  Microsoft::WRL::ComPtr<ID3DBlob> pixel, errors;
  auto result = D3DCompile(source.data(), source.size(), name, nullptr, nullptr,
                          vertexEntry, "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                          &next.vertexCode, &errors);
  if (SUCCEEDED(result))
    result = D3DCompile(source.data(), source.size(), name, nullptr, nullptr,
                        pixelEntry, "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors);
  if (FAILED(result)) {
    if (errors)
      spdlog::error("{} shader: {}", name, static_cast<const char *>(errors->GetBufferPointer()));
    return result;
  }
  result = device->CreateVertexShader(next.vertexCode->GetBufferPointer(),
      next.vertexCode->GetBufferSize(), nullptr, &next.vertex);
  if (SUCCEEDED(result))
    result = device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
                                      nullptr, &next.pixel);
  if (SUCCEEDED(result))
    program = std::move(next);
  return result;
}
HRESULT OverlayBackend::CreateConstants(UINT bytes, ID3D11Buffer **buffer) {
  return Rendering::CreateConstants(device.Get(), bytes, buffer);
}
void OverlayBackend::BindFrame(const WorldFrame::Frame &frame) {
  context->ClearState();
  const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  auto *camera = frame.camera.Get();
  context->VSSetConstantBuffers(1, 1, &camera);
  context->PSSetConstantBuffers(1, 1, &camera);
  auto *depth = frame.depth.Get();
  context->PSSetShaderResources(0, 1, &depth);
  auto *sampler = states.sampler.Get();
  context->PSSetSamplers(0, 1, &sampler);
}
void OverlayBackend::Draw(ID3D11DeviceContext *immediate,
                          const WorldFrame::Frame &frame) {
  if (!immediate || !frame.camera || !frame.depth)
    return;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output;
  immediate->OMGetRenderTargets(1, &output, nullptr);
  if (!output)
    return;
  try {
    Initialize(immediate);
    if (!initialized)
      return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    output->GetResource(&resource);
    Check(resource.As(&texture));
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    width = desc.Width;
    height = desc.Height;
    target = output.Get();
    ScopedPass restore(context.Get(), state.Get());
    struct ClearBindings {
      OverlayBackend &backend;
      ~ClearBindings() { backend.Context()->ClearState(); backend.target = nullptr; }
    } clear{*this};
    BindFrame(frame);
    DiffusePreview::Render(*this);
    BindFrame(frame);
    Terrain::BrushOverlay::Render(*this);
  } catch (const std::exception &error) {
    if (!failed)
      spdlog::error("Overlay backend: {}", error.what());
    failed = true;
  }
}
}
