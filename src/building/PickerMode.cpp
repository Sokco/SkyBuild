#include "building/PickerMode.h"
#include "building/PreviewMesh.h"
#include "physics/Raycast.h"
#include "ui/imgui/UI.h"
#include "ui/pack-editor/PackEditor.h"
#include "ui/imgui/NativeUI.h"
#include <atomic>

namespace SkyBuild::PickerMode {
namespace {
bool active{};
PreviewMesh highlight;
struct Overlay {
  std::string label;
};
std::atomic<std::shared_ptr<const Overlay>> overlay;
std::unique_ptr<UI::Native::Model::HudElement> overlayHUD;

void __stdcall DrawOverlay() {
  const auto value = overlay.load();
  if (!value)
    return;
  using namespace ImGui;
  const auto size = GetIO().DisplaySize;
  auto *draw = GetForegroundDrawList();
  constexpr auto color = IM_COL32(65, 220, 255, 230);
  const auto cursor = GetIO().MouseDrawCursor
                          ? GetIO().MousePos
                          : ImVec2{size.x * .5F, size.y * .5F};
  const auto extent = CalcTextSize(value->label.c_str());
  const ImVec2 pos{std::clamp(cursor.x - extent.x * .5F, 8.F,
                              std::max(8.F, size.x - extent.x - 8.F)),
                   std::min(cursor.y + 24.F, size.y - extent.y - 12.F)};
  draw->AddRectFilled({pos.x - 6, pos.y - 4},
                      {pos.x + extent.x + 6, pos.y + extent.y + 4},
                      IM_COL32(0, 0, 0, 190), 3.F);
  draw->AddText(pos, color, value->label.c_str());
}

void UpdateOverlay(RE::TESObjectREFR *ref) {
  auto value = std::make_shared<Overlay>();
  auto *base = ref->GetBaseObject();
  using Lookup = const char *(*)(std::uint32_t);
  static auto lookup = [] {
    auto dll = GetModuleHandleW(L"po3_Tweaks.dll");
    return dll ? reinterpret_cast<Lookup>(
                     GetProcAddress(dll, "GetFormEditorID"))
               : nullptr;
  }();
  const char *editor = base->GetFormEditorID();
  if ((!editor || !*editor) && lookup)
    editor = lookup(base->GetFormID());
  value->label =
      editor && *editor
          ? editor
          : std::format("Editor ID unavailable ({:08X})", base->GetFormID());
  overlay.store(std::move(value));
}

RE::NiPointer<RE::TESObjectREFR> target;
void Clear() {
  overlay.store(nullptr);
  target.reset();
  highlight.Reset();
}
} // namespace
void Start() {
  if (!overlayHUD)
    overlayHUD = std::make_unique<UI::Native::Model::HudElement>(DrawOverlay);
  active = true;
  Clear();
}
void Stop() {
  active = false;
  Clear();
}
bool HasTarget() { return active && target; }
void Update() {
  if (!active || UI::IsToolWheelOpen() || UI::IsGameMenuBlocking()) {
    Clear();
    return;
  }
  const auto hits =
      Physics::CastSelectionRay(2048.F, [](RE::TESObjectREFR *ref) {
        const auto base = ref->GetBaseObject();
        return base && UI::PackEditor::CanPickObject(base->GetFormID());
      });
  // Stop at the nearest surface, including unsupported geometry. The picker
  // must not select an object through a wall or other obstruction.
  const auto candidate = hits.empty() ? nullptr : hits.front().hitReference;
  auto *base = candidate ? candidate->GetBaseObject() : nullptr;
  if (!candidate || candidate->IsDeleted() || candidate->IsDisabled() ||
      !base || !UI::PackEditor::CanPickObject(base->GetFormID()) ||
      !highlight.EnsureCreated(base, {.65F, {.1F, .65F, 1.F}, 1.5F, true})) {
    Clear();
    return;
  }
  target = candidate;
  highlight.SetTransform(target->GetPosition(), target->GetAngle(),
                         target->GetScale());
  highlight.SetVisible(true);
  UpdateOverlay(target.get());
}
void Use() {
  Update();
  if (!HasTarget())
    return;
  const auto id = target->GetBaseObject()->GetFormID();
  Clear();
  UI::PackEditor::OpenForObject(id);
}
} // namespace SkyBuild::PickerMode
