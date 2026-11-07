#include "building/preview/DiffusePreview.h"
#include "building/preview/DiffusePreviewShader.h"
#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include "terrain/brush-overlay/BrushOverlayMath.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace SkyBuild::DiffusePreview {
namespace {
using Microsoft::WRL::ComPtr;
namespace Scene = Terrain::BrushOverlay;
std::mutex gate;
std::unordered_map<const void *, Instance> instances;
struct Constants {
  float projection[4][4], model[4][4];
  float tint[4], alpha[4], uv[4], params[4], viewport[4], jitter[4];
};
struct Renderer {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext1> context1;
  ComPtr<ID3DDeviceContextState> state;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3DBlob> vertexCode;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11BlendState> blend, colorOff;
  ComPtr<ID3D11DepthStencilState> writeDepth, readDepth;
  ComPtr<ID3D11RasterizerState> cull, twoSided;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11ShaderResourceView> white;
  ComPtr<ID3D11Texture2D> depth;
  ComPtr<ID3D11DepthStencilView> depthView;
  std::unordered_map<unsigned, ComPtr<ID3D11InputLayout>> layouts;
  unsigned width{}, height{};
  bool initialized{}, failed{};
  void Check(HRESULT hr) {
    if (FAILED(hr))
      throw std::runtime_error(std::format("DX11 {:08X}", unsigned(hr)));
  }
  void Initialize(ID3D11DeviceContext *context) {
    ComPtr<ID3D11Device> current;
    context->GetDevice(&current);
    if (current != device) {
      *this = Renderer{};
      device = current;
    }
    if (initialized || failed)
      return;
    ComPtr<ID3D11Device1> extended;
    Check(device.As(&extended));
    Check(context->QueryInterface(IID_PPV_ARGS(&context1)));
    auto level = device->GetFeatureLevel();
    Check(extended->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                             __uuidof(ID3D11Device), nullptr,
                                             &state));
    ComPtr<ID3DBlob> pixelCode, errors;
    Check(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildDiffusePreview",
                     nullptr, nullptr, "VS", "vs_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexCode, &errors));
    Check(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildDiffusePreview",
                     nullptr, nullptr, "PS", "ps_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelCode, &errors));
    Check(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                     vertexCode->GetBufferSize(), nullptr,
                                     &vs));
    Check(device->CreatePixelShader(pixelCode->GetBufferPointer(),
                                    pixelCode->GetBufferSize(), nullptr, &ps));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Check(device->CreateBuffer(&cb, nullptr, &constants));
    D3D11_BLEND_DESC bd{};
    auto &b = bd.RenderTarget[0];
    b.BlendEnable = true;
    b.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.BlendOp = D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.DestBlendAlpha = D3D11_BLEND_ONE;
    b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(device->CreateBlendState(&bd, &blend));
    b.RenderTargetWriteMask = 0;
    Check(device->CreateBlendState(&bd, &colorOff));
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = true;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    Check(device->CreateDepthStencilState(&dd, &writeDepth));
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_EQUAL;
    Check(device->CreateDepthStencilState(&dd, &readDepth));
    // Native mesh indices use counter-clockwise front faces with this
    // projection.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = true;
    rd.DepthClipEnable = true;
    Check(device->CreateRasterizerState(&rd, &cull));
    rd.CullMode = D3D11_CULL_NONE;
    Check(device->CreateRasterizerState(&rd, &twoSided));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Check(device->CreateSamplerState(&sd, &sampler));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = td.MipLevels = td.ArraySize = td.SampleDesc.Count =
        1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const unsigned pixel = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA initial{&pixel, 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    Check(device->CreateTexture2D(&td, &initial, &texture));
    Check(device->CreateShaderResourceView(texture.Get(), nullptr, &white));
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
    Check(device->CreateInputLayout(elements, 2, vertexCode->GetBufferPointer(),
                                    vertexCode->GetBufferSize(), &layout));
    return layout.Get();
  }
  // Resolve preview depth before tinting, then restore the game's pipeline
  // state.
  void Draw(const std::vector<Instance> &draws) {
    auto *graphics = RE::BSGraphics::Renderer::GetSingleton();
    auto *camera = RE::Main::WorldRootCamera();
    if (!graphics || !camera || REL::Module::IsVR())
      return;
    auto &runtime = graphics->GetRuntimeData();
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(runtime.context);
    auto *target = reinterpret_cast<ID3D11RenderTargetView *>(
        runtime.renderWindows[0].renderView);
    auto *sceneDepth = reinterpret_cast<ID3D11ShaderResourceView *>(
        graphics->GetDepthStencilData()
            .depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN]
            .depthSRV);
    if (!context || !target || !sceneDepth)
      return;
    auto frame = Scene::Trace::SceneFrame();
    if (!frame.buffer)
      frame = Scene::ReadSceneFrame(context);
    Constants base{};
    base.jitter[2] = frame.buffer ? 1.F : 0.F;
    if (!frame.buffer) {
      auto *shadow = RE::BSGraphics::RendererShadowState::GetSingleton();
      if (!shadow)
        return;
      const auto view = shadow->GetRuntimeData().cameraData.getEye();
      float projection[4][4];
      std::memcpy(projection, &view.projMat, sizeof(projection));
      const auto &frustum = camera->GetRuntimeData2().viewFrustum;
      auto reversed =
          Scene::ReversedDepth(projection, frustum.fNear, frustum.fFar);
      if (!reversed)
        return;
      Scene::SceneDepthProjection(camera->GetRuntimeData().worldToCam,
                                  base.projection, *reversed);
      base.jitter[3] = *reversed ? 1.F : 0.F;
      base.jitter[0] = (view.projMat._31 - view.projMatrixUnjittered._31) /
                       view.projMat._34 * .5F;
      base.jitter[1] = (view.projMat._32 - view.projMatrixUnjittered._32) /
                       view.projMat._34 * -.5F;
      if (!std::isfinite(base.jitter[0]) || !std::isfinite(base.jitter[1]))
        return;
    }
    Initialize(context);
    if (!initialized)
      return;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    target->GetResource(&resource);
    Check(resource.As(&texture));
    D3D11_TEXTURE2D_DESC size{};
    texture->GetDesc(&size);
    if (!depth || width != size.Width || height != size.Height) {
      depthView.Reset();
      depth.Reset();
      D3D11_TEXTURE2D_DESC d{};
      d.Width = size.Width;
      d.Height = size.Height;
      d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
      d.Format = DXGI_FORMAT_D32_FLOAT;
      d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
      Check(device->CreateTexture2D(&d, nullptr, &depth));
      Check(device->CreateDepthStencilView(depth.Get(), nullptr, &depthView));
      width = d.Width;
      height = d.Height;
    }
    base.viewport[0] = float(width);
    base.viewport[1] = float(height);
    sceneDepth->GetResource(&resource);
    texture.Reset();
    Check(resource.As(&texture));
    texture->GetDesc(&size);
    base.viewport[2] = float(size.Width);
    base.viewport[3] = float(size.Height);
    if (auto *graphicsState = RE::BSGraphics::State::GetSingleton()) {
      const auto &s = graphicsState->GetRuntimeData();
      if (s.dynamicResolutionWidthRatio > 0 &&
          s.dynamicResolutionWidthRatio <= 1)
        base.viewport[2] *= s.dynamicResolutionWidthRatio;
      if (s.dynamicResolutionHeightRatio > 0 &&
          s.dynamicResolutionHeightRatio <= 1)
        base.viewport[3] *= s.dynamicResolutionHeightRatio;
    }
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(state.Get(), &previous);
    struct Restore {
      ID3D11DeviceContext1 *context;
      ID3DDeviceContextState *state;
      ~Restore() { context->SwapDeviceContextState(state, nullptr); }
    } restore{context1.Get(), previous.Get()};
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, depthView.Get());
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto *cb = constants.Get();
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);
    auto *frameBuffer = frame.buffer.Get();
    if (frame.count) {
      context1->VSSetConstantBuffers1(1, 1, &frameBuffer, &frame.first,
                                      &frame.count);
      context1->PSSetConstantBuffers1(1, 1, &frameBuffer, &frame.first,
                                      &frame.count);
    } else {
      context->VSSetConstantBuffers(1, 1, &frameBuffer);
      context->PSSetConstantBuffers(1, 1, &frameBuffer);
    }
    auto *sampling = sampler.Get();
    context->PSSetSamplers(0, 1, &sampling);
    context->PSSetShaderResources(0, 1, &sceneDepth);
    for (unsigned pass = 0; pass < 2; ++pass) {
      context->OMSetBlendState(pass ? blend.Get() : colorOff.Get(), nullptr,
                               0xFFFFFFFF);
      context->OMSetDepthStencilState(pass ? readDepth.Get() : writeDepth.Get(),
                                      0);
      for (const auto &instance : draws)
        for (const auto &surface : instance.model->surfaces) {
          auto data = base;
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
          auto *diffuse = surface.diffuse ? surface.diffuse.Get() : white.Get();
          context->PSSetShaderResources(1, 1, &diffuse);
          context->RSSetState(surface.twoSided ? twoSided.Get() : cull.Get());
          context->DrawIndexed(surface.indexCount, 0, 0);
        }
    }
    ID3D11ShaderResourceView *emptyViews[2]{};
    context->PSSetShaderResources(0, 2, emptyViews);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11Buffer *empty{};
    UINT zero = 0;
    context->IASetVertexBuffers(0, 1, &empty, &zero, &zero);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    context->VSSetConstantBuffers(1, 1, &empty);
    context->PSSetConstantBuffers(1, 1, &empty);
  }
} renderer;
} // namespace
void Publish(const void *owner, const Instance &instance) {
  std::scoped_lock lock(gate);
  instances[owner] = instance;
  Scene::Trace::CaptureScene(true);
}
void Remove(const void *owner) {
  std::scoped_lock lock(gate);
  instances.erase(owner);
  Scene::Trace::CaptureScene(!instances.empty());
}
void Render() {
  std::vector<Instance> draws;
  {
    std::scoped_lock lock(gate);
    for (const auto &[owner, instance] : instances)
      draws.push_back(instance);
  }
  if (draws.empty())
    return;
  try {
    renderer.Draw(draws);
  } catch (const std::exception &error) {
    if (!renderer.failed)
      spdlog::error("Diffuse preview renderer: {}", error.what());
    renderer.failed = true;
  }
}
} // namespace SkyBuild::DiffusePreview
