#pragma once
#include "building/preview/DiffusePreview.h"

namespace SkyBuild {
class PreviewMesh {
public:
  struct Appearance {
    float alpha{1.0F};
    RE::NiColor tint{1.0F, 1.0F, 1.0F};
    float tintStrength{0.0F};
    bool selectionHighlight{};
  };

  PreviewMesh() = default;
  ~PreviewMesh();

  PreviewMesh(const PreviewMesh &) = delete;
  PreviewMesh(PreviewMesh &&) = delete;
  PreviewMesh &operator=(const PreviewMesh &) = delete;
  PreviewMesh &operator=(PreviewMesh &&) = delete;

  [[nodiscard]] bool EnsureCreated(RE::TESBoundObject *a_baseObject,
                                   const Appearance &a_appearance);
  void SetTransform(const RE::NiPoint3 &a_position,
                    const RE::NiPoint3 &a_rotation, float a_scale = 1.0F);
  void SetVisible(bool a_visible);
  void Reset();

  [[nodiscard]] bool IsCreated() const;
  struct Bounds {
    RE::NiPoint3 min, max;
  };
  [[nodiscard]] std::optional<Bounds> GetLocalBounds();

private:
  std::shared_ptr<const DiffusePreview::Model> model;
  RE::TESBoundObject *baseObject{};
  RE::NiTransform placement;
  Appearance appearance;
  bool visible{};
  void Publish();
};
} // namespace SkyBuild
