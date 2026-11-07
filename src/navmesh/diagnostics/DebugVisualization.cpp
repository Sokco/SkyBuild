#include "navmesh/diagnostics/DebugVisualization.h"
#include "navmesh/diagnostics/DebugMath.h"
#include "navmesh/diagnostics/Diagnostics.h"
#include "navmesh/publication/NativeReplacement.h"
#include "navmesh/runtime/NativeStreaming.h"
#include "navmesh/runtime/NativeSynchronization.h"
#include "navmesh/runtime/RuntimeNavmesh.h"
#include "ui/imgui/NativeUI.h"
#include "ui/imgui/Widgets.h"
#include "ui/settings/Settings.h"
#include "ui/settings/SettingsFields.h"
#include "ui/shared/MenuLayout.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
namespace SkyBuild::Navmesh::DebugVisualization {
namespace {
constexpr std::size_t MAX_EDGES = 40000;
constexpr auto REFRESH_INTERVAL = std::chrono::milliseconds(500);
enum class EdgeKind { kInterior, kBoundary, kPortal };
struct Edge {
  Point a, b;
  EdgeKind kind;
};
struct Geometry {
  std::vector<Edge> edges;
  std::vector<std::array<Point, 3>> faces;
  std::size_t meshes{}, triangles{}, skipped{};
  bool truncated{};
};
struct Frame {
  float matrix[4][4]{};
  RE::NiRect<float> viewport;
  float nearPlane{.01F};
  std::shared_ptr<const Geometry> geometry;
  RuntimeStatus generation;
  Point player;
  Point camera;
};
struct Capture {
  // Incremental drawing may span a native scene transition. Never keep native
  // mesh ownership across frames: it prevents ordinary cell retirement.
  std::vector<RE::FormID> meshes;
  std::size_t mesh{}, triangle{};
  Point origin;
  float radius{};
  std::uint64_t epoch{};
  std::set<std::uint32_t> included;
  std::shared_ptr<Geometry> geometry = std::make_shared<Geometry>();
};
std::atomic_bool enabled{}, sceneLoaded{};
std::atomic_bool fillTriangles{true};
std::atomic<float> horizontalDistance{2048}, verticalDistance{512};
std::atomic<float> opacity{.85F}, fillOpacity{.14F}, lineWidth{1.25F};
std::atomic_uint64_t refreshEpoch{};
std::atomic_bool refreshRequested{};
using DrawLine = void (*)(ImGui::ImDrawList *, const ImGui::ImVec2,
                          const ImGui::ImVec2, ImGui::ImU32, float);
DrawLine drawLine{};
std::unique_ptr<SkyBuild::UI::Native::Model::HudElement> hud;
std::optional<Capture> capture;
std::shared_ptr<const Geometry> geometry;
std::mutex frameMutex;
std::shared_ptr<const Frame> frame;
std::chrono::steady_clock::time_point nextRefresh, completionUntil;
bool wasGenerating{};
std::atomic_bool drawing{};
void Clear() {
  drawing.store(false);
  capture.reset();
  geometry.reset();
  nextRefresh = {};
  const std::scoped_lock lock(frameMutex);
  frame.reset();
}
void BeginCapture(RE::NiCamera *camera) {
  const auto pathing = RE::Pathing::GetSingleton();
  if (!pathing)
    return;
  Capture pending;
  const auto origin = camera->world.translate;
  pending.origin = {origin.x, origin.y, origin.z};
  pending.radius = std::max(horizontalDistance.load(), verticalDistance.load());
  pending.epoch = refreshEpoch.load();
  // The native enumeration temporarily owns meshes. Release those references
  // before returning; only stable lookup keys survive into the next update.
  RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> loaded;
  if (!pathing->GetAllLoadedNavmeshes5(loaded))
    return;
  pending.meshes.reserve(loaded.size());
  for (const auto &mesh : loaded)
    if (mesh)
      pending.meshes.push_back(mesh->QNavmeshID());
  capture = std::move(pending);
}
void CaptureTriangle(Capture &pending, RE::BSNavmesh &mesh, std::size_t index) {
  if (index >= mesh.triangles.size())
    return;
  const auto triangle = mesh.triangles[index];
  std::uint16_t flags{};
  std::memcpy(&flags, &triangle.triangleFlags, sizeof(flags));
  if (!DebugMath::VisibleTriangle(flags))
    return;
  std::array<Point, 3> points;
  for (int vertex = 0; vertex < 3; ++vertex) {
    if (triangle.vertices[vertex] >= mesh.vertices.size()) {
      ++pending.geometry->skipped;
      return;
    }
    const auto point = mesh.vertices[triangle.vertices[vertex]].location;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z)) {
      ++pending.geometry->skipped;
      return;
    }
    points[vertex] = {point.x, point.y, point.z};
  }
  if (!DebugMath::InRange(points, pending.origin, pending.radius))
    return;
  ++pending.geometry->triangles;
  pending.geometry->faces.push_back(points);
  pending.included.insert(mesh.QNavmeshID());
  for (int edge = 0; edge < 3; ++edge) {
    const bool portal = (flags & (1U << edge)) != 0;
    const auto neighbor = triangle.triangles[edge];
    bool boundary =
        !portal && (neighbor == 65535 || neighbor >= mesh.triangles.size());
    if (!portal && !boundary) {
      std::uint16_t otherFlags{};
      std::memcpy(&otherFlags, &mesh.triangles[neighbor].triangleFlags,
                  sizeof(otherFlags));
      boundary = !DebugMath::VisibleTriangle(otherFlags);
      if (!boundary && neighbor < index)
        continue;
    }
    if (pending.geometry->edges.size() >= MAX_EDGES) {
      pending.geometry->truncated = true;
      return;
    }
    pending.geometry->edges.push_back({points[edge], points[(edge + 1) % 3],
                                       portal     ? EdgeKind::kPortal
                                       : boundary ? EdgeKind::kBoundary
                                                  : EdgeKind::kInterior});
  }
}
ImGui::ImU32 Color(EdgeKind kind) {
  const std::uint32_t alpha =
      static_cast<std::uint32_t>(std::clamp(opacity.load(), 0.0F, 1.0F) * 255);
  const auto rgb = kind == EdgeKind::kBoundary ? 0x004BD9FFU
                   : kind == EdgeKind::kPortal ? 0x00FFD24BU
                                               : 0x0075E17AU;
  return rgb | (alpha << 24);
}
bool Project(const Frame &view, Point point, DebugMath::ScreenPoint &screen) {
  float x{}, y{}, z{};
  if (!RE::NiCamera::WorldPtToScreenPt3(view.matrix, view.viewport,
                                        {point.x, point.y, point.z}, x, y, z,
                                        .000001F))
    return false;
  screen = {x, 1 - y};
  return std::isfinite(x) && std::isfinite(y);
}
void __stdcall Draw() {
  if (!drawing.load() || !sceneLoaded.load())
    return;
  std::shared_ptr<const Frame> view;
  {
    const std::scoped_lock lock(frameMutex);
    view = frame;
  }
  if (!view)
    return;
  const bool generating = view->generation.generating;
  if (!(Diagnostics::Enabled() && enabled.load()) && !generating)
    return;
  const float time = static_cast<float>(ImGui::GetTime());
  if (generating) {
    UI::Widgets::WorkspaceTheme theme;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * .5F, display.y * .82F},
                            ImGui::ImGuiCond_Always, {.5F, .5F});
    ImGui::SetNextWindowSize({440, 0}, ImGui::ImGuiCond_Always);
    constexpr auto flags =
        ImGui::ImGuiWindowFlags_NoInputs | ImGui::ImGuiWindowFlags_NoCollapse |
        ImGui::ImGuiWindowFlags_NoResize | ImGui::ImGuiWindowFlags_NoMove |
        ImGui::ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Updating Navmesh", nullptr, flags)) {
      const auto stage = view->generation.stage;
      const char *phase =
          view->generation.generationSucceeded ? "Navigation ready"
          : stage == RuntimeStage::kCapturing  ? "Gathering geometry"
          : stage == RuntimeStage::kBuilding   ? "Building navigation"
          : stage == RuntimeStage::kGenerated  ? "Installing navigation"
                                               : "Preparing navigation";
      ImGui::TextUnformatted(phase);
      const auto &progress = view->generation;
      const auto label = std::format("{}% of steps",
                                     static_cast<int>(progress.progress * 100));
      ImGui::ProgressBar(progress.progress, {-1, 0}, label.c_str());
      if (progress.commandCells)
        ImGui::Text("Cells built: %zu / %zu", progress.completedCells,
                    progress.commandCells);
      if (stage == RuntimeStage::kCapturing && progress.total)
        ImGui::Text("Objects collected: %zu / %zu", progress.sampled,
                    progress.total);
      ImGui::TextWrapped("Gameplay will resume when navigation is ready.");
    }
    ImGui::End();
  }
  if (!view->geometry)
    return;
  const auto &io = ImGui::GetIO();
  const auto size = io.DisplaySize;
  if (size.x <= 0 || size.y <= 0)
    return;
  auto draw = ImGui::GetBackgroundDrawList();
  if (!draw)
    return;
  const std::array<float, 4> depth{view->matrix[3][0], view->matrix[3][1],
                                   view->matrix[3][2], view->matrix[3][3]};
  const float thickness = lineWidth.load();
  const float horizontalRange = horizontalDistance.load();
  const float verticalRange = verticalDistance.load();
  const auto distanceFade = [&](Point point) {
    const float dx = point.x - view->camera.x;
    const float dy = point.y - view->camera.y;
    const float dz = point.z - view->camera.z;
    // An ellipsoidal range gives height its own limit without a hard cutoff.
    const float distance =
        std::sqrt((dx * dx + dy * dy) / (horizontalRange * horizontalRange) +
                  dz * dz / (verticalRange * verticalRange));
    const float t = std::clamp((distance - .25F) / .75F, 0.F, 1.F);
    return 1.F - t * t * (3.F - 2.F * t);
  };
  if (fillTriangles.load()) {
    for (const auto &face : view->geometry->faces) {
      // Clip the triangle against the near plane before projection. A clipped
      // triangle has at most four vertices; ImGui handles screen clipping.
      std::array<Point, 4> polygon{};
      std::size_t count = 0;
      const auto depthAt = [&](Point p) {
        return p.x * depth[0] + p.y * depth[1] + p.z * depth[2] + depth[3];
      };
      for (int i = 0; i < 3; ++i) {
        const auto a = face[i], b = face[(i + 1) % 3];
        const float da = depthAt(a), db = depthAt(b);
        if (da >= view->nearPlane)
          polygon[count++] = a;
        if ((da >= view->nearPlane) != (db >= view->nearPlane))
          polygon[count++] =
              DebugMath::Mix(a, b, (view->nearPlane - da) / (db - da));
      }
      if (count < 3)
        continue;
      std::array<ImGui::ImVec2, 4> projected{};
      bool valid = true;
      for (std::size_t i = 0; i < count; ++i) {
        DebugMath::ScreenPoint screen;
        if (!Project(*view, polygon[i], screen)) {
          valid = false;
          break;
        }
        projected[i] = {screen.x * size.x, screen.y * size.y};
      }
      if (!valid)
        continue;
      const Point center{(face[0].x + face[1].x + face[2].x) / 3.F,
                         (face[0].y + face[1].y + face[2].y) / 3.F,
                         (face[0].z + face[1].z + face[2].z) / 3.F};
      const auto alpha = static_cast<std::uint32_t>(fillOpacity.load() *
                                                    distanceFade(center) * 255);
      if (alpha)
        ImGui::ImDrawListManager::AddConvexPolyFilled(
            draw, projected.data(), static_cast<int>(count),
            (generating ? 0x00FFE0A0U : 0x0075E17AU) | (alpha << 24));
    }
  }
  for (const auto &edge : view->geometry->edges) {
    auto a = edge.a, b = edge.b;
    if (!DebugMath::ClipNear(a, b, depth, view->nearPlane))
      continue;
    DebugMath::ScreenPoint first, second;
    if (!Project(*view, a, first) || !Project(*view, b, second) ||
        !DebugMath::ClipViewport(first, second))
      continue;
    auto color = Color(edge.kind);
    if (generating) {
      const float dx = (edge.a.x + edge.b.x) * .5F - view->player.x;
      const float dy = (edge.a.y + edge.b.y) * .5F - view->player.y;
      const float wave =
          .5F + .5F * std::sin(std::hypot(dx, dy) / 180.F - time * 3.F);
      const auto alpha = static_cast<std::uint32_t>((.25F + .65F * wave) * 255);
      color = 0x00FFE0A0U | (alpha << 24); // ImGui ABGR: light blue.
    }
    // Keep the nearby quarter of the view clear, then smoothly fade to its
    // limit.
    const float fade =
        distanceFade({(a.x + b.x) * .5F, (a.y + b.y) * .5F, (a.z + b.z) * .5F});
    const auto alpha = static_cast<std::uint32_t>((color >> 24) * fade);
    if (!alpha)
      continue;
    color = (color & 0x00FFFFFFU) | (alpha << 24);
    drawLine(draw, {first.x * size.x, first.y * size.y},
             {second.x * size.x, second.y * size.y}, color, thickness);
  }
  if (generating)
    return;
  const auto label =
      std::format("NAVMESH  {} meshes / {} triangles{}", view->geometry->meshes,
                  view->geometry->triangles,
                  view->geometry->truncated ? " (display limit)" : "");
  ImGui::ImDrawListManager::AddText(draw, {16, 16}, 0xFFFFFFFFU, label.c_str());
}
} // namespace
void Initialize() {
  if (hud)
    return;
  drawLine = ImGui::ImDrawListManager::AddLine;
  hud = std::make_unique<SkyBuild::UI::Native::Model::HudElement>(Draw);
  spdlog::info("Registered read-only ImGui navmesh debug overlay.");
}
void SetSceneLoaded(bool loaded) {
  sceneLoaded.store(loaded);
  ++refreshEpoch;
  Clear();
  wasGenerating = false;
  completionUntil = {};
}
void Update() {
  if (!(Diagnostics::Enabled() && enabled.load()) &&
      !GenerationPresentationActive() && !wasGenerating &&
      completionUntil == std::chrono::steady_clock::time_point{}) {
    if (drawing.load())
      Clear();
    return;
  }
  if (refreshRequested.exchange(false)) {
    // Refresh the back buffer without blanking the displayed copied geometry.
    // Scene teardown still clears both; publication only replaces this picture
    // once the incremental capture of installed navigation is complete.
    capture.reset();
    nextRefresh = {};
  }
  auto generation = GetStatus();
  const auto now = std::chrono::steady_clock::now();
  if (wasGenerating && !generation.generating && generation.generationSucceeded)
    completionUntil = now + std::chrono::milliseconds(650);
  wasGenerating = generation.generating;
  if (!generation.generating && now < completionUntil)
    generation.generating = true; // Briefly show confirmed publication at 100%.
  else if (!generation.generating)
    completionUntil = {};
  if ((!(Diagnostics::Enabled() && enabled.load()) && !generation.generating) ||
      !sceneLoaded.load()) {
    Clear();
    return;
  }
  const auto ui = RE::UI::GetSingleton();
  const auto player = RE::PlayerCharacter::GetSingleton();
  const auto camera = RE::Main::WorldRootCamera();
  if (!camera || !player || !player->GetParentCell() ||
      !player->GetParentCell()->IsAttached() ||
      (ui && ui->IsMenuOpen("Loading Menu"))) {
    Clear();
    return;
  }
  if (capture && capture->epoch != refreshEpoch.load()) {
    capture.reset();
    nextRefresh = {};
  }
  if (!capture && std::chrono::steady_clock::now() >= nextRefresh)
    BeginCapture(camera);
  if (capture) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    do {
      if (capture->mesh >= capture->meshes.size() ||
          capture->geometry->truncated) {
        capture->geometry->meshes = capture->included.size();
        geometry = std::move(capture->geometry);
        capture.reset();
        nextRefresh = std::chrono::steady_clock::now() + REFRESH_INTERVAL;
        break;
      }
      // Resolve again within this game-thread update. A cell may have unloaded
      // since enumeration; a missing form is simply skipped by the gizmo.
      RE::BSTSmartPointer<RE::BSNavmesh> mesh{
          RE::TESForm::LookupByID<RE::NavMesh>(capture->meshes[capture->mesh])};
      if (!mesh || capture->triangle >= mesh->triangles.size()) {
        ++capture->mesh;
        capture->triangle = 0;
        continue;
      }
      CaptureTriangle(*capture, *mesh, capture->triangle++);
    } while (std::chrono::steady_clock::now() < deadline);
  }
  auto updated = std::make_shared<Frame>();
  std::memcpy(updated->matrix, camera->GetRuntimeData().worldToCam,
              sizeof(updated->matrix));
  updated->viewport = camera->GetRuntimeData2().port;
  updated->nearPlane =
      std::max(.01F, camera->GetRuntimeData2().viewFrustum.fNear);
  updated->geometry = geometry;
  updated->generation = generation;
  const auto position = player->GetPosition();
  updated->player = {position.x, position.y, position.z};
  const auto cameraPosition = camera->world.translate;
  updated->camera = {cameraPosition.x, cameraPosition.y, cameraPosition.z};
  {
    const std::scoped_lock lock(frameMutex);
    frame = std::move(updated);
    drawing.store(true);
  }
}
void __stdcall RenderNavigationSettings() {
  const auto generation = GetStatus();
  const bool busy =
      NativeSynchronization::PendingAction() ||
      NativeSynchronization::ProbeState() == PauseProbeState::kWaiting ||
      generation.stage == RuntimeStage::kCapturing ||
      generation.stage == RuntimeStage::kBuilding;
  Settings::Fields::NavigationState state{
      busy,
      generation.cachedCells && NativeSynchronization::Available(),
      NativeReplacement::Installed(),
      generation.cachedCells,
      generation.message,
      NativeReplacement::Status()};
  const auto action = Settings::Fields::Navigation(state);
  using Action = Settings::Fields::NavigationAction;
  if (action == Action::Generate) {
    refreshRequested.store(true);
    RequestGeneration();
  } else if (action == Action::Apply) {
    refreshRequested.store(true);
    RequestCachedApply();
  } else if (action == Action::Revert)
    NativeSynchronization::RequestOriginalRestore();
}
void RefreshInstalledGeometry() {
  ++refreshEpoch;
  refreshRequested.store(true);
}
} // namespace SkyBuild::Navmesh::DebugVisualization
