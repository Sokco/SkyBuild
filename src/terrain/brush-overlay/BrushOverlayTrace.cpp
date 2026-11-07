#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include "terrain/TerrainSurface.h"
#include "terrain/TerrainTool.h"
#include <MinHook.h>
#include <atomic>
#include <d3d11.h>
#include <map>
#include <mutex>
#include <wrl/client.h>
namespace SkyBuild::Terrain::BrushOverlay::Trace {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID stampID{0x9535c324,
                       0x66a2,
                       0x4b0d,
                       {0xa0, 0x77, 0x17, 0x58, 0xa3, 0xc4, 0x25, 0xd1}};
using DrawFn = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT,
                                         INT);
DrawFn original{};
ID3D11DeviceContext *immediate{}; // Borrowed engine identity, never owns it.
std::atomic_bool capturing{};
std::atomic_bool sceneCapturing{};
std::mutex gate;
FrameDraws<TerrainSurface> sceneSurfaces;
SceneFrameSnapshot sceneFrame;
constexpr GUID payloadID{0x647c52fe,
                         0xb717,
                         0x4776,
                         {0x9f, 0x48, 0x20, 0x6d, 0x5e, 0xc8, 0x7f, 0xb2}};
// The buffer owns a CPU surface, never another GPU resource. No reference
// cycle.
struct Payload final : IUnknown {
  std::atomic_ulong references{1};
  Stamp stamp;
  std::shared_ptr<const TerrainSurface> surface;
  Payload(Stamp stamp, std::shared_ptr<const TerrainSurface> surface)
      : stamp(stamp), surface(std::move(surface)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown))
      return E_NOINTERFACE;
    *out = static_cast<IUnknown *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references;
    if (!remaining)
      delete this;
    return remaining;
  }
};
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext *context, UINT indices,
                            UINT start, INT base) {
  // Object previews also need the scene's pre-postprocess camera matrices,
  // including in interiors with no terrain. Stop inspecting draws once copied.
  if (sceneCapturing.load(std::memory_order_relaxed) && context == immediate &&
      !sceneFrame.Captured()) {
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11PixelShader> pixel;
    context->OMGetRenderTargets(1, &target, &dsv);
    if (dsv && target) {
      context->PSGetShader(&pixel, nullptr, nullptr);
      ComPtr<ID3D11Resource> depth;
      dsv->GetResource(&depth);
      auto *graphics = RE::BSGraphics::Renderer::GetSingleton();
      const auto &data = graphics->GetDepthStencilData();
      bool scene = false;
      for (auto index : {RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN,
                         RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY,
                         RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY})
        scene |= depth.Get() == reinterpret_cast<ID3D11Resource *>(
                                    data.depthStencils[index].texture);
      if (scene && pixel)
        sceneFrame.Capture(context);
    }
  }
  if (capturing.load(std::memory_order_relaxed) && context == immediate &&
      Terrain::Ready()) {
    ComPtr<ID3D11Buffer> buffer;
    UINT stride{}, offset{};
    context->IAGetVertexBuffers(0, 1, &buffer, &stride, &offset);
    Stamp stamp{};
    UINT bytes = sizeof(stamp);
    if (buffer && SUCCEEDED(buffer->GetPrivateData(stampID, &bytes, &stamp)) &&
        bytes == sizeof(stamp) && stamp.key && stamp.revision) {
      // Exclude shadow/cubemap passes: only draws targeting current scene depth
      // or its main copy provide evidence for the overlay's depth comparison.
      ComPtr<ID3D11DepthStencilView> dsv;
      context->OMGetRenderTargets(0, nullptr, &dsv);
      if (dsv) {
        ComPtr<ID3D11Resource> depth;
        dsv->GetResource(&depth);
        auto *graphics = RE::BSGraphics::Renderer::GetSingleton();
        const auto &data = graphics->GetDepthStencilData();
        bool scene = false;
        for (auto index : {RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN,
                           RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY,
                           RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY})
          scene |= depth.Get() == reinterpret_cast<ID3D11Resource *>(
                                      data.depthStencils[index].texture);
        if (scene) {
          ComPtr<Payload> payload;
          UINT payloadBytes = sizeof(Payload *);
          if (FAILED(buffer->GetPrivateData(payloadID, &payloadBytes,
                                            payload.GetAddressOf())) ||
              !payload || payload->stamp.key != stamp.key ||
              payload->stamp.revision != stamp.revision) {
            original(context, indices, start, base);
            return;
          }
          // A depth-only prepass may leave unrelated pixel constants bound.
          // Capture only a draw with a pixel shader and a color output.
          if (!sceneFrame.Captured()) {
            ComPtr<ID3D11PixelShader> pixelShader;
            ComPtr<ID3D11RenderTargetView> colorTarget;
            context->PSGetShader(&pixelShader, nullptr, nullptr);
            context->OMGetRenderTargets(1, &colorTarget, nullptr);
            if (pixelShader && colorTarget) {
              sceneFrame.Capture(context);
            }
          }
          std::scoped_lock lock(gate);
          sceneSurfaces.Record(stamp, payload->surface);
        }
      }
    }
  }
  original(context, indices, start, base);
}
} // namespace
void Initialize(ID3D11DeviceContext *context) {
  if (original || !context)
    return;
  const auto init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    return;
  // ID3D11DeviceContext::DrawIndexed is slot 12 in the SDK interface.
  auto *target = (*reinterpret_cast<void ***>(context))[12];
  if (MH_CreateHook(target, reinterpret_cast<void *>(&Draw),
                    reinterpret_cast<void **>(&original)) != MH_OK) {
    spdlog::warn("Terrain draw tracking could not hook DrawIndexed.");
    original = nullptr;
    return;
  }
  immediate = context;
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    original = nullptr;
    immediate = nullptr;
    return;
  }
}
void Tag(ID3D11Buffer *buffer, Stamp stamp,
         std::shared_ptr<const TerrainSurface> surface) {
  if (buffer && FAILED(buffer->SetPrivateData(stampID, sizeof(stamp), &stamp)))
    spdlog::warn("Terrain draw tracking could not tag terrain buffer.");
  if (buffer && surface) {
    auto *payload = new Payload(stamp, std::move(surface));
    const auto result = buffer->SetPrivateDataInterface(payloadID, payload);
    payload->Release();
    if (FAILED(result))
      spdlog::warn("Terrain draw selection could not bind immutable surface to "
                   "vertex buffer.");
  }
}

void Capture(bool enabled) { capturing.store(enabled); }
void CaptureScene(bool enabled) { sceneCapturing.store(enabled); }
SceneFrameBinding SceneFrame() { return sceneFrame.Current(); }
std::vector<DrawnPayload<TerrainSurface>>
Select(std::span<const std::uint64_t> keys) {
  std::scoped_lock lock(gate);
  return sceneSurfaces.Select(keys);
}
void FrameBoundary() {
  sceneFrame.FrameBoundary();
  std::scoped_lock lock(gate);
  sceneSurfaces.Clear();
}
} // namespace SkyBuild::Terrain::BrushOverlay::Trace
