#include "terrain/brush-overlay/TerrainBrushPass.h"
#include "terrain/brush-overlay/BrushOverlayShader.h"
#include <cstring>
namespace SkyBuild::Terrain::BrushOverlay {
namespace {
struct Constants {
  float brush[4];
};
static_assert(sizeof(Constants) == 16);
} // namespace
bool TerrainBrushPass::Initialize(Rendering::OverlayBackend &backend) {
  auto *device = backend.Device();
  if (owner.Get() == device)
    return initialized;
  *this = TerrainBrushPass{};
  owner = device;
  if (FAILED(backend.CreateProgram(shader, "SkyBuildBrushOverlay", program)) ||
      FAILED(backend.CreateProgram(shader, "SkyBuildBrushComposite", composite, "CompositeVS", "CompositePS")))
    return false;
  const D3D11_INPUT_ELEMENT_DESC input{
      "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
      0,          0, D3D11_INPUT_PER_VERTEX_DATA,
      0};
  static_assert(sizeof(OverlayVertex) == 12);
  if (FAILED(device->CreateInputLayout(&input, 1,
                                       program.vertexCode->GetBufferPointer(),
                                       program.vertexCode->GetBufferSize(), &layout)))
    return false;
  if (FAILED(backend.CreateConstants(sizeof(Constants), &constants)))
    return false;
  initialized = true;
  return true;
}
bool TerrainBrushPass::Draw(Rendering::OverlayBackend &backend,
                            std::span<const OverlayVertex> triangles, const Brush &brush) {
  if (triangles.empty() || triangles.size() > 180000 || !Initialize(backend))
    return false;
  auto *device = backend.Device();
  auto *context = backend.Context();
  auto *target = backend.Target();
  const auto &states = backend.states;
  if (!mask || maskWidth != backend.Width() ||
      maskHeight != backend.Height()) {
    mask.Reset();
    maskRTV.Reset();
    maskSRV.Reset();
    D3D11_TEXTURE2D_DESC md{};
    md.Width = backend.Width();
    md.Height = backend.Height();
    md.MipLevels = md.ArraySize = md.SampleDesc.Count = 1;
    md.Format = DXGI_FORMAT_R8_UNORM;
    md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&md, nullptr, &mask)) ||
        FAILED(device->CreateRenderTargetView(mask.Get(), nullptr, &maskRTV)) ||
        FAILED(
            device->CreateShaderResourceView(mask.Get(), nullptr, &maskSRV))) {
      mask.Reset();
      return false;
    }
    maskWidth = md.Width;
    maskHeight = md.Height;
  }
  if (triangles.size() > capacity) {
    capacity =
        std::min(180000U, std::max(4096U, unsigned(triangles.size()) * 2));
    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = capacity * sizeof(OverlayVertex);
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertices.Reset();
    if (FAILED(device->CreateBuffer(&vb, nullptr, &vertices))) {
      capacity = 0;
      return false;
    }
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(
          context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return false;
  std::memcpy(mapped.pData, triangles.data(), triangles.size_bytes());
  context->Unmap(vertices.Get(), 0);
  Constants data{};
  data.brush[0] = brush.center.x;
  data.brush[1] = brush.center.y;
  data.brush[2] = brush.center.z;
  data.brush[3] = brush.radius;
  if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                          &mapped)))
    return false;
  std::memcpy(mapped.pData, &data, sizeof(data));
  context->Unmap(constants.Get(), 0);
  context->RSSetState(states.twoSided.Get());
  auto *maskTarget = maskRTV.Get();
  const float clear[4]{};
  context->ClearRenderTargetView(maskTarget, clear);
  context->OMSetRenderTargets(1, &maskTarget, nullptr);
  context->OMSetBlendState(states.maximum.Get(), nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(states.noDepth.Get(), 0);
  context->IASetInputLayout(layout.Get());
  const UINT stride = sizeof(OverlayVertex), offset = 0;
  auto *vb = vertices.Get();
  context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  context->VSSetShader(program.vertex.Get(), nullptr, 0);
  context->PSSetShader(program.pixel.Get(), nullptr, 0);
  auto *cb = constants.Get();
  context->VSSetConstantBuffers(0, 1, &cb);
  context->PSSetConstantBuffers(0, 1, &cb);
  context->Draw(unsigned(triangles.size()), 0);
  // Resolve MAX coverage once per pixel, so duplicated/overlapping triangles
  // cannot accumulate alpha. The terrain mask is private and resize-aware.
  context->OMSetRenderTargets(1, &target, nullptr);
  context->OMSetBlendState(states.alpha.Get(), nullptr, 0xFFFFFFFF);
  auto *maskView = maskSRV.Get();
  context->PSSetShaderResources(0, 1, &maskView);
  context->IASetInputLayout(nullptr);
  context->VSSetShader(composite.vertex.Get(), nullptr, 0);
  context->PSSetShader(composite.pixel.Get(), nullptr, 0);
  context->Draw(3, 0);
  return true;
}
} // namespace SkyBuild::Terrain::BrushOverlay
