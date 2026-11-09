#include "ui/fine-adjustments/FineAdjustments.h"
#include "building/BuildingMode.h"
#include "building/ControlBindings.h"
#include "building/FineAdjustmentMath.h"
#include "building/KeyRepeat.h"
#include "building/PlacementPreview.h"
#include "building/PlacementRotation.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include <algorithm>
#include <atomic>
#include <nlohmann/json.hpp>
#include <numbers>
namespace SkyBuild::UI::FineAdjustments {
namespace {
std::atomic_bool open{};
std::unique_ptr<SkyBuild::UI::Native::Model::HudElement> hud;
std::atomic<std::shared_ptr<const nlohmann::json>> frame;
RE::NiPoint3 originalPosition{}, originalRotation{}, fineAngles{};
RE::NiMatrix3 fineRotation;
FineAdjustmentMath::Vector localForward{0, 1, 0};
RE::NiPoint3 forward{0, 1, 0}, right{1, 0, 0}, up{0, 0, 1};
RE::NiPoint3 FineRotation() {
  RE::NiPoint3 result;
  PlacementRotation::ToEulerXYZ(fineRotation, result);
  return result;
}
nlohmann::json lastPayload;
constexpr std::array<const char *, 16> controls{
    "left",         "right",         "forward",  "backward",
    "up",           "down",          "yawLeft",  "yawRight",
    "pitchForward", "pitchBackward", "rollLeft", "rollRight",
    "scaleDown",    "scaleUp",       "confirm",  "cancel"};
void CaptureMovementAxes() {
  const auto camera = RE::PlayerCamera::GetSingleton();
  localForward = {0, 1, 0};
  if (camera && camera->cameraRoot) {
    auto direction = PlacementPreview::GetState().position - camera->cameraRoot->world.translate;
    if (direction.x * direction.x + direction.y * direction.y < .000001F)
      direction = camera->cameraRoot->world.rotate.GetVectorY();
    localForward = FineAdjustmentMath::PlacementForward(
        direction.x, direction.y, fineAngles.z);
  }
  const auto localRight = FineAdjustmentMath::Right(localForward);
  forward = {localForward[0], localForward[1], 0};
  right = {localRight[0], localRight[1], 0};
  up = {0, 0, 1};
}
void Present() {
  if (!open)
    return;
  const auto &s = PlacementPreview::GetState();
  const auto delta = s.position - originalPosition;
  constexpr float degrees = 180.0F / std::numbers::pi_v<float>;
  nlohmann::json bindings = nlohmann::json::object();
  for (std::size_t i = 0; i < ControlBindings::fineControls.size(); ++i)
    bindings[controls[i]] = ControlBindings::fineControls[i].Icon();
  bindings["confirm"] = ControlBindings::fineConfirm.Icon();
  bindings["cancel"] = ControlBindings::fineCancel.Icon();
  const auto payload = nlohmann::json{
      {"position", {delta.Dot(right), delta.Dot(forward), delta.Dot(up)}},
      {"rotation",
       {(fineAngles.z - originalRotation.z) * degrees,
        (fineAngles.x - originalRotation.x) * degrees,
        (fineAngles.y - originalRotation.y) * degrees}},
      {"scale", s.scale},
      {"bindings", bindings},
      {"opacity", GetPlaceableAppearance().value("opacity", 1.F)}};
  if (payload != lastPayload) {
    frame.store(std::make_shared<const nlohmann::json>(payload));
    lastPayload = payload;
  }
}
void __stdcall Draw() {
  if (!open.load())
    return;
  const auto view = frame.load();
  if (!view)
    return;
  Widgets::BeginFrame();
  Widgets::Theme theme(view->value("opacity", 1.F));
  const auto screen = ImGui::GetIO().DisplaySize;
  ImGui::ImDrawListManager::AddCircleFilled(ImGui::GetForegroundDrawList(),
                                            {screen.x * .5F, screen.y * .5F},
                                            2.2F, 0xFFF2F4F2, 12);
  constexpr std::array<const char *, 16> labels{
      "Left",          "Right",          "Forward",     "Backward",
      "Raise",         "Lower",          "Rotate left", "Rotate right",
      "Pitch forward", "Pitch backward", "Roll left", "Roll right",
      "Scale down",    "Scale up",       "Confirm",       "Cancel adjustments"};
  const auto &keys = (*view)["bindings"];
  float leftWidth = 0, rightWidth = 0;
  for (std::size_t i = 0; i < labels.size(); i += 2) {
    leftWidth =
        std::max(leftWidth, Widgets::KeyWidth(keys.value(controls[i], "")) +
                                10.F + ImGui::CalcTextSize(labels[i]).x);
    rightWidth = std::max(rightWidth,
                          Widgets::KeyWidth(keys.value(controls[i + 1], "")) +
                              10.F + ImGui::CalcTextSize(labels[i + 1]).x);
  }
  const float column = leftWidth + 24.F;
  const float panelWidth = column + rightWidth + 32.F;
  if (Widgets::Panel("Fine adjustments",
                     {screen.x - panelWidth - 25.F, screen.y * .18F},
                     {panelWidth, 0})) {
    ImGui::TextDisabled("FINE ADJUSTMENTS");
    ImGui::Separator();
    for (std::size_t i = 0; i < labels.size(); i += 2) {
      if (i == 0) {
        ImGui::TextUnformatted("Position - 4 units");
        const auto &position = view->at("position");
        ImGui::TextDisabled(
            "Across %.0f / Forward %.0f / Up %.0f", position[0].get<double>(),
            position[1].get<double>(), position[2].get<double>());
      }
      if (i == 6) {
        ImGui::Separator();
        ImGui::TextUnformatted("Rotation - 5 degrees");
        const auto &angles = view->at("rotation");
        ImGui::TextDisabled("Rotate %.0f / Pitch %.0f / Roll %.0f",
                            angles[0].get<double>(), angles[1].get<double>(),
                            angles[2].get<double>());
      }
      if (i == 12) {
        ImGui::Separator();
        ImGui::Text("Scale - %.0f%%", view->value("scale", 1.F) * 100);
      }
      if (i == 14)
        ImGui::Separator();
      Widgets::Key(keys.value(controls[i], ""), labels[i]);
      ImGui::SameLine(column);
      Widgets::Key(keys.value(controls[i + 1], ""), labels[i + 1]);
    }
  }
  ImGui::End();
}
void Adjust(std::size_t action) {
  CaptureMovementAxes();
  const auto &s = PlacementPreview::GetState();
  auto position = s.position;
  auto scale = s.scale;
  constexpr float angle = 5.0F * std::numbers::pi_v<float> / 180.0F;
  constexpr float displacement = 4.F;
  switch (action) {
  case 0:
    position = position - right * displacement;
    break;
  case 1:
    position = position + right * displacement;
    break;
  case 2:
    position = position + forward * displacement;
    break;
  case 3:
    position = position - forward * displacement;
    break;
  case 4:
    position = position + up * displacement;
    break;
  case 5:
    position = position - up * displacement;
    break;
  case 6:
    fineAngles.z -= angle;
    break;
  case 7:
    fineAngles.z += angle;
    break;
  case 8:
    fineAngles.x -= angle;
    break;
  case 9:
    fineAngles.x += angle;
    break;
  case 10:
    fineAngles.y -= angle;
    break;
  case 11:
    fineAngles.y += angle;
    break;
  case 12:
    scale = FineAdjustmentMath::StepScale(scale, -1);
    break;
  case 13:
    scale = FineAdjustmentMath::StepScale(scale, 1);
    break;
  case 14:
    PlacementPreview::FinishFineAdjustments(true);
    Close();
    PlacementPreview::PlacePreview();
    return;
  case 15:
    Close();
    return;
  default:
    return;
  }
  if (action >= 6 && action <= 11) {
    const int axis = action < 8 ? 2 : action < 10 ? 0 : 1;
    FineAdjustmentMath::RotateLocal(fineRotation, axis,
                                     action % 2 == 0 ? -angle : angle);
  }
  PlacementPreview::SetFineTransform(position, FineRotation(), scale);
  Present();
}
} // namespace
void Open() {
  if (open || !PlacementPreview::BeginFineAdjustments())
    return;
  if (!hud)
    hud = std::make_unique<SkyBuild::UI::Native::Model::HudElement>(Draw);
  originalPosition = PlacementPreview::GetState().position;
  RE::NiMatrix3 initial;
  initial.SetEulerAnglesXYZ(PlacementPreview::GetState().rotation);
  fineAngles = {0, 0, PlacementPreview::GetState().rotation.z};
  fineRotation = initial;
  originalRotation = fineAngles;
  CaptureMovementAxes();
  open = true;
  lastPayload = nullptr;
  Present();
  RefreshPresentation();
}
void Close() {
  if (!open)
    return;
  open = false;
  PlacementPreview::FinishFineAdjustments(false);
  frame.store(nullptr);
  RefreshPresentation();
}
void Update() {
  if (!open)
    return;
  if (!BuildingMode::IsHammerReady() || !PlacementPreview::GetState().active ||
      IsGameMenuBlocking(true)) {
    Close();
    return;
  }
  CaptureMovementAxes();
  Present();
}
bool HandleButton(RE::ButtonEvent *button) {
  if (!open || !button)
    return false;
  const auto matches = [button](const auto &binding) {
    return ControlBindings::Matches(*button, binding);
  };
  const bool cancel =
      matches(ControlBindings::fineCancel) ||
      (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard &&
       button->GetIDCode() == RE::BSKeyboardDevice::Key::kEscape);
  if (cancel || matches(ControlBindings::fineConfirm)) {
    if (button->IsDown())
      Adjust(cancel ? 15 : 14);
    return true;
  }
  for (std::size_t i = 0; i < ControlBindings::fineControls.size(); ++i) {
    const auto &b = ControlBindings::fineControls[i];
    if (ControlBindings::Matches(*button, b)) {
      static std::array<KeyRepeat, 14> repeats;
      if (i < repeats.size()
              ? repeats[i].Pulse(button->IsDown(), button->IsPressed(),
                                 button->HeldDuration())
              : button->IsDown())
        Adjust(i);
      return true;
    }
  }
  // Consume other SkyBuild actions without changing the selected item/tool.
  using namespace ControlBindings;
  for (const auto *b :
       {&fineAdjustments, &rotate, &placeAction, &switchMode, &toggleSnapping,
        &packPrevious, &packNext, &categoryLeft, &categoryRight, &scrollLeft,
        &scrollRight})
    if (ControlBindings::Matches(*button, *b))
      return true;
  return false;
}
bool IsOpen() { return open; }
} // namespace SkyBuild::UI::FineAdjustments
