#include "terrain/brush-overlay/BrushOverlay.h"
#include "terrain/LandscapeGeometry.h"
#include "terrain/TerrainSurface.h"
#include "terrain/TerrainTool.h"
#include "terrain/brush-overlay/BrushOverlayMath.h"
#include "terrain/brush-overlay/BrushOverlayRenderer.h"
#include "terrain/brush-overlay/BrushOverlayTrace.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/UI.h"
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace SkyBuild::Terrain::BrushOverlay {
namespace {
struct Frame {
  Brush brush;
  std::vector<OverlayVertex> triangles;
  std::vector<std::uint64_t> keys;
  std::chrono::steady_clock::time_point captured =
      std::chrono::steady_clock::now();
};
struct Original {
  RE::NiPointer<RE::BSTriShape> owner;
  LandscapeGeometry surface;
};
std::unordered_map<RE::BSTriShape *, Original> originals;
std::unordered_set<RE::BSTriShape *> used;
std::shared_ptr<Frame> building;
std::shared_ptr<const Frame> published;
std::mutex mutex;
Renderer renderer;
bool captureWarning{}, renderWarning{}, projectionWarning{};

void AddTriangle(std::array<OverlayVertex, 3> triangle) {
  if (!building || building->triangles.size() >= 180000 ||
      !Intersects({triangle[0], triangle[1], triangle[2]}, building->brush))
    return;
  building->triangles.insert(building->triangles.end(), triangle.begin(),
                             triangle.end());
}
} // namespace
void Begin(const Brush &brush) {
  building = std::make_shared<Frame>();
  building->brush = brush;
  used.clear();
  Trace::Capture(true);
}
void ExpectChunk(std::uint64_t key) {
  if (building)
    building->keys.push_back(key);
}
void AddOriginal(RE::NiAVObject *root) {
  if (!root || !building)
    return;
  const auto &bound = root->worldBound;
  if (bound.radius > 0 &&
      Length(Vec{bound.center.x, bound.center.y, bound.center.z} -
             building->brush.center) > bound.radius + building->brush.radius)
    return;
  if (auto *shape = netimmerse_cast<RE::BSTriShape *>(root)) {
    try {
      used.insert(shape);
      auto found = originals.find(shape);
      if (found == originals.end()) {
        // Capture only source positions and exact native triangle indices.
        // Conversion retains the original material; the overlay needs only
        // geometry.
        auto captured = CaptureLandscapeGeometry(shape);
        found =
            originals
                .emplace(shape, Original{RE::NiPointer<RE::BSTriShape>(shape),
                                         std::move(captured)})
                .first;
      }
      const auto &surface = found->second.surface;
      for (const auto &t : surface.triangles)
        AddTriangle({SurfaceVertex(surface.vertices[t[0]]),
                     SurfaceVertex(surface.vertices[t[1]]),
                     SurfaceVertex(surface.vertices[t[2]])});
    } catch (const std::exception &error) {
      if (!captureWarning) {
        spdlog::warn("Brush overlay original LAND capture unavailable: {}",
                     error.what());
        captureWarning = true;
      }
    }
  } else if (const auto *node = root->AsNode()) {
    for (const auto &child : node->GetChildren())
      if (child)
        AddOriginal(child.get());
  }
}
void Publish() {
  std::erase_if(originals,
                [](const auto &entry) { return !used.contains(entry.first); });
  std::scoped_lock lock(mutex);
  published = std::move(building);
}
void Clear() {
  Trace::Capture(false);
  building.reset();
  originals.clear();
  used.clear();
  std::scoped_lock lock(mutex);
  published.reset();
}
void Render() {
  if (!Terrain::Ready() || Terrain::GetTool() == Tool::Idle ||
      UI::Native::IsAnyBlockingWindowOpened() || UI::IsToolWheelOpen())
    return;
  std::shared_ptr<const Frame> frame;
  {
    std::scoped_lock lock(mutex);
    frame = published;
  }
  if (!frame || (frame->triangles.empty() && frame->keys.empty()) ||
      std::chrono::steady_clock::now() - frame->captured >
          std::chrono::milliseconds(250))
    return;
  // Pick only revisions submitted to the scene this frame. The resource's
  // immutable CPU surface has the same lifetime/identity as its vertex buffer.
  const auto selected = Trace::Select(frame->keys);
  // Render-thread scratch storage: retain capacity, not terrain resources.
  static std::vector<OverlayVertex> triangles;
  triangles.assign(frame->triangles.begin(), frame->triangles.end());
  for (const auto &drawn : selected) {
    const auto &mesh = drawn.surface->Geometry();
    for (const auto &t : mesh.triangles) {
      const auto &a = mesh.vertices[t[0]], &b = mesh.vertices[t[1]],
                 &c = mesh.vertices[t[2]];
      if (triangles.size() < 180000 &&
          Intersects({a.position, b.position, c.position}, frame->brush)) {
        triangles.push_back(SurfaceVertex(a.position));
        triangles.push_back(SurfaceVertex(b.position));
        triangles.push_back(SurfaceVertex(c.position));
      }
    }
  }

  if (triangles.empty())
    return;
  auto *camera = RE::Main::WorldRootCamera();
  auto *graphics = RE::BSGraphics::Renderer::GetSingleton();
  if (!camera || !graphics || REL::Module::IsVR())
    return;
  auto &data = graphics->GetRuntimeData();
  auto *context = reinterpret_cast<ID3D11DeviceContext *>(data.context);
  auto *target = reinterpret_cast<ID3D11RenderTargetView *>(
      data.renderWindows[0].renderView);
  auto *depth = reinterpret_cast<ID3D11ShaderResourceView *>(
      graphics->GetDepthStencilData()
          .depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN]
          .depthSRV);
  if (!context || !target || !depth)
    return;
  auto *graphicsState = RE::BSGraphics::State::GetSingleton();
  float widthRatio = 1.F, heightRatio = 1.F;
  if (graphicsState) {
    const auto &state = graphicsState->GetRuntimeData();
    if (std::isfinite(state.dynamicResolutionWidthRatio) &&
        state.dynamicResolutionWidthRatio > 0)
      widthRatio = std::min(state.dynamicResolutionWidthRatio, 1.F);
    if (std::isfinite(state.dynamicResolutionHeightRatio) &&
        state.dynamicResolutionHeightRatio > 0)
      heightRatio = std::min(state.dynamicResolutionHeightRatio, 1.F);
  }
  const auto drawFrame = Trace::SceneFrame();
  const bool gpuFrame = drawFrame.buffer || Renderer::HasSceneFrame(context);
  float sceneProjection[4][4]{};
  float jitterU{}, jitterV{};
  bool fallbackReversed{};
  // GPU frame data already contains the actual depth convention and temporal
  // matrices. CPU fallback validation must not suppress this path.
  if (!gpuFrame) {
    auto *shadow = RE::BSGraphics::RendererShadowState::GetSingleton();
    if (!shadow)
      return;
    const auto view = shadow->GetRuntimeData().cameraData.getEye();
    float projection[4][4];
    static_assert(sizeof(view.projMat) == sizeof(projection));
    std::memcpy(projection, &view.projMat, sizeof(projection));
    const auto &frustum = camera->GetRuntimeData2().viewFrustum;
    const auto reversed =
        ReversedDepth(projection, frustum.fNear, frustum.fFar);
    if (!reversed) {
      if (!projectionWarning) {
        spdlog::warn("Brush overlay waiting for a valid perspective scene "
                     "projection; no depth convention assumed.");
        projectionWarning = true;
      }
      return;
    }

    fallbackReversed = *reversed;
    SceneDepthProjection(camera->GetRuntimeData().worldToCam, sceneProjection,
                         *reversed);
    // CPU camera projection is unjittered. Sample the scene depth at the
    // matching jittered location while keeping the visible overlay stable.
    const float inverseW = 1.F / view.projMat._34;
    jitterU =
        (view.projMat._31 - view.projMatrixUnjittered._31) * inverseW * .5F;
    jitterV =
        (view.projMat._32 - view.projMatrixUnjittered._32) * inverseW * -.5F;
    if (!std::isfinite(jitterU) || !std::isfinite(jitterV))
      return;
  }
  if (!renderer.Draw(context, target, depth, triangles, sceneProjection,
                     frame->brush, widthRatio, heightRatio, jitterU, jitterV,
                     fallbackReversed, drawFrame)) {
    if (!renderWarning) {
      spdlog::warn("Brush overlay skipped: unsupported DX11 context/depth view "
                   "or GPU resource creation failed.");
      renderWarning = true;
    }
  }
}
} // namespace SkyBuild::Terrain::BrushOverlay
