#include "building/preview/DiffusePreview.h"
#include "building/preview/DiffusePreviewShader.h"
#include "rendering/WorldFrame.h"
#include "rendering/OverlayBackend.h"
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace SkyBuild::DiffusePreview {
namespace {
using Microsoft::WRL::ComPtr;
namespace WorldFrame = Rendering::WorldFrame;
std::mutex gate;
std::unordered_map<const void *, Instance> instances;
struct Constants {
  float model[4][4];
  float tint[4], alpha[4], uv[4], params[4];
};
static_assert(sizeof(Constants) == 128);
struct ObjectPreviewPass {
  ComPtr<ID3D11Device> device;
  Rendering::Program program;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> depthView;
  std::unordered_map<unsigned, ComPtr<ID3D11InputLayout>> layouts;
  unsigned width{}, height{};
  bool initialized{}, failed{};
  void Check(HRESULT hr) {
    if (FAILED(hr))
      throw std::runtime_error(std::format("DX11 {:08X}", unsigned(hr)));
  }
  void Initialize(Rendering::OverlayBackend &backend) {
    if (backend.Device() != device.Get()) {
      *this = ObjectPreviewPass{};
      device = backend.Device();
    }
    if (initialized || failed)
      return;
    Check(backend.CreateProgram(shader, "SkyBuildDiffusePreview", program));
    Check(backend.CreateConstants(sizeof(Constants), &constants));
    initialized = true;
  }
  ID3D11InputLayout *Layout(const Surface &surface) {
    unsigned key = (surface.uvOffset << 2) | unsigned(surface.fullPrecision) |
                   (unsigned(surface.hasUV) << 1);
    auto &layout = layouts[key];
    if (layout)
      return layout.Get();
    D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0,
         surface.fullPrecision ? DXGI_FORMAT_R32G32B32_FLOAT
                               : DXGI_FORMAT_R16G16B16A16_FLOAT,
         0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0,
         surface.hasUV ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R32G32_FLOAT, 0,
         surface.hasUV ? surface.uvOffset : 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    Check(device->CreateInputLayout(elements, 2, program.vertexCode->GetBufferPointer(),
                                    program.vertexCode->GetBufferSize(), &layout));
    return layout.Get();
  }
  void Draw(Rendering::OverlayBackend &backend, const std::vector<Instance> &draws) {
    Initialize(backend);
    if (!initialized)
      return;
    auto *context = backend.Context();
    auto *target = backend.Target();
    const auto &states = backend.states;
    if (!depth || width != backend.Width() || height != backend.Height()) {
      depthView.Reset();
      depth.Reset();
      D3D11_TEXTURE2D_DESC d{};
      d.Width = backend.Width();
      d.Height = backend.Height();
      d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
      d.Format = DXGI_FORMAT_D32_FLOAT;
      d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      Check(device->CreateTexture2D(&d, nullptr, &depth));
      Check(device->CreateDepthStencilView(depth.Get(), nullptr, &depthView));
      width = d.Width;
      height = d.Height;
    }
    context->OMSetRenderTargets(1, &target, depthView.Get());
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    context->VSSetShader(program.vertex.Get(), nullptr, 0);
    context->PSSetShader(program.pixel.Get(), nullptr, 0);
    auto *cb = constants.Get();
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);
    for (unsigned pass = 0; pass < 2; ++pass) {
      context->OMSetBlendState(pass ? states.alpha.Get() : states.colorOff.Get(), nullptr,
                               0xFFFFFFFF);
      context->OMSetDepthStencilState(pass ? states.equalDepth.Get() : states.writeDepth.Get(),
                                      0);
      for (const auto &instance : draws)
        for (const auto &surface : instance.model->surfaces) {
          Constants data{};
          auto transform = instance.placement * surface.local;
          for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c)
              data.model[r][c] = transform.rotate.entry[r][c] * transform.scale;
          data.model[0][3] = transform.translate.x;
          data.model[1][3] = transform.translate.y;
          data.model[2][3] = transform.translate.z;
          data.model[3][3] = 1;
          data.tint[0] = instance.tint.red;
          data.tint[1] = instance.tint.green;
          data.tint[2] = instance.tint.blue;
          data.tint[3] = instance.opacity * surface.alpha;
          data.params[0] = instance.depthBias;
          data.params[1] = std::clamp(instance.tintStrength * .60F, 0.F, 1.F);
          data.alpha[0] = surface.blendAlpha ? 1.F : 0.F;
          data.alpha[1] = float(surface.alphaTest);
          data.alpha[2] = surface.threshold;
          data.alpha[3] = surface.diffuse && surface.hasUV ? 1.F : 0.F;
          std::memcpy(data.uv, surface.uv, sizeof(data.uv));
          D3D11_MAPPED_SUBRESOURCE mapped{};
          Check(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped));
          std::memcpy(mapped.pData, &data, sizeof(data));
          context->Unmap(constants.Get(), 0);
          context->IASetInputLayout(Layout(surface));
          auto *vb = surface.vertices.Get();
          const UINT offset = 0;
          context->IASetVertexBuffers(0, 1, &vb, &surface.stride, &offset);
          context->IASetIndexBuffer(surface.indices.Get(), DXGI_FORMAT_R16_UINT,
                                    0);
          auto *diffuse = surface.diffuse ? surface.diffuse.Get() : states.white.Get();
          context->PSSetShaderResources(1, 1, &diffuse);
          context->RSSetState(surface.twoSided ? states.twoSided.Get() : states.cull.Get());
          context->DrawIndexed(surface.indexCount, 0, 0);
        }
    }
  }
} objectPass;
} // namespace
void Publish(const void *owner, const Instance &instance) {
  std::scoped_lock lock(gate);
  instances[owner] = instance;
  WorldFrame::SetActive(WorldFrame::Consumer::ObjectPreview, true);
}
void Remove(const void *owner) {
  std::scoped_lock lock(gate);
  instances.erase(owner);
  WorldFrame::SetActive(WorldFrame::Consumer::ObjectPreview, !instances.empty());
}
void Render(Rendering::OverlayBackend &backend) {
  std::vector<Instance> draws;
  {
    std::scoped_lock lock(gate);
    for (const auto &[owner, instance] : instances)
      draws.push_back(instance);
  }
  if (draws.empty())
    return;
  try {
    objectPass.Draw(backend, draws);
  } catch (const std::exception &error) {
    if (!objectPass.failed)
      spdlog::error("Diffuse preview renderer: {}", error.what());
    objectPass.failed = true;
  }
}
} // namespace SkyBuild::DiffusePreview
