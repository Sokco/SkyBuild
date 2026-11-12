#include "rendering/WorldFrame.h"
#include "rendering/OverlayBackend.h"
#include "terrain/brush-overlay/BrushOverlayMath.h"
#include "rendering/WorldFrameMath.h"
#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include <atomic>
#include <cstring>

namespace SkyBuild::Rendering::WorldFrame {
namespace {
using Microsoft::WRL::ComPtr;
// CameraStateData's runtime accessor starts eight bytes before its aligned view.
struct alignas(16) CameraEntry {
  RE::NiCamera *camera{};
  std::byte cameraPadding[8]{};
  alignas(16) RE::BSGraphics::ViewData view;
  RE::NiPoint3 adjust, currentAdjust, previousAdjust;
  bool jitter{};
  std::byte tailPadding[11]{};
};
static_assert(offsetof(CameraEntry, view) == 0x10);
static_assert(offsetof(CameraEntry, jitter) == 0x284);
static_assert(sizeof(CameraEntry) == 0x290);

std::atomic_uint active{};
Frame frame;
OverlayBackend backend;
ComPtr<ID3D11Texture2D> depthCopy;
ComPtr<ID3D11Device> device;
DWORD owner{};
bool valid{};
REL::Relocation<void (*)(std::uint64_t, std::uint32_t)> original;

bool Finite(const Matrix &matrix) {
  for (const auto &row : matrix.m)
    for (float x : row)
      if (!std::isfinite(x))
        return false;
  return true;
}
bool Camera(RE::BSGraphics::State &state, CameraConstants &out,
            float width, float height) {
  auto *player = RE::PlayerCamera::GetSingleton();
  if (!player || !player->cameraRoot)
    return false;
  const auto &entries = state.GetRuntimeData().cameraDataCacheA;
  CameraEntry selected{};
  RE::NiPointer<RE::NiCamera> selectedCamera;
  bool ambiguous = false;
  auto visit = [&](auto &&self, RE::NiAVObject *object, unsigned depth) -> void {
    if (!object || depth > 8 || ambiguous)
      return;
    if (auto *node = object->AsNode()) {
      for (const auto &child : node->GetChildren())
        self(self, child.get(), depth + 1);
      return;
    }
    auto *camera = netimmerse_cast<RE::NiCamera *>(object);
    if (!camera)
      return;
    const auto *bytes = reinterpret_cast<const std::byte *>(entries.data());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      CameraEntry entry;
      std::memcpy(&entry, bytes + i * sizeof(entry), sizeof(entry));
      if (entry.camera != camera)
        continue;
      // Prefer the jittered world view; accept an unjittered-only cache.
      if (selectedCamera && selected.jitter && !entry.jitter)
        continue;
      if (selectedCamera && selected.jitter == entry.jitter) {
        ambiguous = true;
        return;
      }
      selected = entry;
      selectedCamera.reset(camera);
    }
  };
  visit(visit, player->cameraRoot.get(), 0);
  if (!selectedCamera || ambiguous)
    return false;
  Matrix view, projection, unjittered;
  std::memcpy(&view, &selected.view.viewMat, sizeof(view));
  std::memcpy(&projection, &selected.view.projMat, sizeof(projection));
  std::memcpy(&unjittered, &selected.view.projMatrixUnjittered, sizeof(unjittered));
  if (!Finite(view) || !Finite(projection) || !Finite(unjittered) ||
      projection._11 <= 0 ||
      std::abs(projection._22 / projection._11 - width / height) > .03F)
    return false;
  const auto &frustum = selectedCamera->GetRuntimeData2().viewFrustum;
  if (!Terrain::BrushOverlay::ReversedDepth(projection.m, frustum.fNear, frustum.fFar))
    return false;
  const auto position = selectedCamera->world.translate;
  if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
    return false;
  BuildCameraMatrices(view, projection, unjittered, position.x, position.y, position.z, out);
  return true;
}
bool Depth(ID3D11Device *current, ID3D11Texture2D *source,
           D3D11_TEXTURE2D_DESC &desc, DXGI_FORMAT &viewFormat) {
  if (!source)
    return false;
  source->GetDesc(&desc);
  if (!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 ||
      desc.SampleDesc.Count != 1 || !(desc.BindFlags & D3D11_BIND_DEPTH_STENCIL))
    return false;
  ComPtr<ID3D11Device> sourceDevice;
  source->GetDevice(&sourceDevice);
  if (sourceDevice.Get() != current)
    return false;
  switch (desc.Format) {
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    return true;
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    return true;
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT:
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    viewFormat = DXGI_FORMAT_R32_FLOAT;
    return true;
  default:
    return false;
  }
}
void Capture() {
  valid = false;
  if (!active.load(std::memory_order_relaxed))
    return;
  auto *graphics = RE::BSGraphics::Renderer::GetSingleton();
  auto *state = RE::BSGraphics::State::GetSingleton();
  if (!graphics || !state)
    return;
  auto *context = reinterpret_cast<ID3D11DeviceContext *>(graphics->GetRuntimeData().context);
  if (!context)
    return;
  ComPtr<ID3D11Device> current;
  context->GetDevice(&current);
  if (current != device) {
    frame = {};
    depthCopy.Reset();
    device = current;
  }
  auto &depths = graphics->GetDepthStencilData().depthStencils;
  // ENB can leave the post-prepass copy empty. Snapshot the world depth itself
  // here, before later passes can reuse it.
  auto *source = reinterpret_cast<ID3D11Texture2D *>(
      depths[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN].texture);
  D3D11_TEXTURE2D_DESC desc{};
  DXGI_FORMAT format{};
  if (!Depth(device.Get(), source, desc, format)) {
    return;
  }
  const auto &runtime = state->GetRuntimeData();
  CameraConstants constants{};
  constants.scale[0] = runtime.dynamicResolutionWidthRatio;
  constants.scale[1] = runtime.dynamicResolutionHeightRatio;
  for (unsigned i = 0; i < 2; ++i)
    if (!std::isfinite(constants.scale[i]) || constants.scale[i] <= 0 || constants.scale[i] > 1) {
      return;
    }
  if (!Camera(*state, constants, desc.Width * constants.scale[0], desc.Height * constants.scale[1])) {
    return;
  }
  D3D11_TEXTURE2D_DESC previous{};
  if (depthCopy)
    depthCopy->GetDesc(&previous);
  if (!depthCopy || previous.Width != desc.Width || previous.Height != desc.Height ||
      previous.Format != desc.Format) {
    depthCopy.Reset();
    frame.depth.Reset();
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &depthCopy))) {
      return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(depthCopy.Get(), &srv, &frame.depth))) {
      depthCopy.Reset();
      return;
    }
  }
  if (!frame.camera) {
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &frame.camera))) {
      return;
    }
  }
  context->CopyResource(depthCopy.Get(), source);
  context->UpdateSubresource(frame.camera.Get(), 0, nullptr, &constants, 0, 0);
  frame.number = state->GetFrameCount();
  owner = GetCurrentThreadId();
  valid = true;

}
void Complete(std::uint64_t a1, std::uint32_t a2) {
  original(a1, a2);
  Capture();
  // The interface target is bound here, before Scaleform draws its menus.
  if (const auto *snapshot = Current()) {
    auto &runtime = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData();
    backend.Draw(reinterpret_cast<ID3D11DeviceContext *>(runtime.context),
                 *snapshot);
  }
  Terrain::BrushOverlay::Trace::FrameBoundary();
  EndFrame();
}
}
bool Install() {
  if (REL::Module::IsVR())
    return false;
  const auto version = REL::Module::get().version();
  if (version != REL::Version(1, 5, 97, 0) &&
      version != REL::Version(1, 6, 1170, 0) &&
      version != REL::Version(1, 7, 104, 0)) {
    spdlog::error("Preview world capture: unsupported runtime");
    return false;
  }
  const auto address = REL::RelocationID(79947, 82084).address() +
                       (REL::Module::IsSE() ? 0x16F : 0x17A);
  if (*reinterpret_cast<const std::uint8_t *>(address) != 0xE8) {
    spdlog::error("Preview world capture: unexpected completion call");
    return false;
  }
  original = SKSE::GetTrampoline().write_call<5>(address, Complete);
  return true;
}
void SetActive(Consumer consumer, bool enabled) {
  const auto bit = static_cast<unsigned>(consumer);
  if (enabled)
    active.fetch_or(bit, std::memory_order_relaxed);
  else
    active.fetch_and(~bit, std::memory_order_relaxed);
}
const Frame *Current() {
  auto *state = RE::BSGraphics::State::GetSingleton();
  return valid && owner == GetCurrentThreadId() && state &&
                 frame.number == state->GetFrameCount() ? &frame : nullptr;
}
void EndFrame() { valid = false; }

}
