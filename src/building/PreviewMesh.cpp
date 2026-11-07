#include "building/PreviewMesh.h"
#include "models/VertexBounds.h"

namespace SkyBuild {
PreviewMesh::~PreviewMesh() { Reset(); }
bool PreviewMesh::EnsureCreated(RE::TESBoundObject *base,
                                const Appearance &settings) {
  if (!base) {
    Reset();
    return false;
  }
  appearance = settings;
  if (model && baseObject == base)
    return true;
  Reset();
  appearance = settings;
  model = DiffusePreview::Load(base);
  if (!model)
    return false;
  baseObject = base;
  return true;
}
void PreviewMesh::SetTransform(const RE::NiPoint3 &position,
                               const RE::NiPoint3 &rotation, float scale) {
  placement.translate = position;
  placement.rotate.SetEulerAnglesXYZ(rotation);
  placement.scale = scale;
  if (visible)
    Publish();
}
void PreviewMesh::Publish() {
  if (!model || !visible)
    return;
  DiffusePreview::Publish(this, {model, placement, appearance.tint,
                                 std::clamp(appearance.alpha, 0.F, 1.F),
                                 appearance.tintStrength,
                                 appearance.selectionHighlight ? 2.F : 0.05F});
}
void PreviewMesh::SetVisible(bool show) {
  visible = show;
  if (show)
    Publish();
  else
    DiffusePreview::Remove(this);
}
void PreviewMesh::Reset() {
  DiffusePreview::Remove(this);
  visible = false;
  model.reset();
  baseObject = nullptr;
}
bool PreviewMesh::IsCreated() const { return bool(model); }
std::optional<PreviewMesh::Bounds> PreviewMesh::GetLocalBounds() {
  if (!baseObject)
    return {};
  const auto bounds = Models::GetVertexBounds(baseObject);
  if (!bounds)
    return {};
  return Bounds{{bounds->min[0], bounds->min[1], bounds->min[2]},
                {bounds->max[0], bounds->max[1], bounds->max[2]}};
}
} // namespace SkyBuild
