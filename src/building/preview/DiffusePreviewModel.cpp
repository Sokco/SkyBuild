#include "building/preview/DiffusePreview.h"
#include "models/ModelAsset.h"
#include <list>
#include <cstring>

namespace SkyBuild::DiffusePreview {
// Retain a private scene graph so cached GPU buffers outlive each preview.
std::shared_ptr<const Model> Load(RE::TESBoundObject *base) {
  struct Cached {
    RE::FormID form;
    std::shared_ptr<const Model> model;
  };
  static std::list<Cached> cache;
  for (auto it = cache.begin(); it != cache.end(); ++it)
    if (it->form == base->GetFormID()) {
      auto model = it->model;
      cache.splice(cache.begin(), cache, it);
      return model;
    }
  auto root = Models::LoadWorldModel(base);
  if (!root)
    return {};
  // Never attach this asset to the game scene. No lights, collision, shadow
  // lists, animated material controllers, or native preview passes are run.
  RE::BSVisit::TraverseScenegraphObjects(
      root.get(), [](RE::NiAVObject *object) {
        while (auto *controller = object->GetControllers())
          object->RemoveController(controller);
        if (auto *geometry = object->AsGeometry()) {
          if (auto *shader =
                  geometry->GetGeometryRuntimeData().shaderProperty.get())
            while (auto *controller = shader->GetControllers())
              shader->RemoveController(controller);
        }
        return RE::BSVisit::BSVisitControl::kContinue;
      });
  RE::NiUpdateData update{};
  update.flags.set(RE::NiUpdateData::Flag::kDirty);
  root->Update(update);
  auto model = std::make_shared<Model>();
  model->owner = root;
  unsigned unsupported{};
  RE::BSVisit::TraverseScenegraphGeometries(
      root.get(), [&](RE::BSGeometry *geometry) {
        for (auto *node = static_cast<RE::NiAVObject *>(geometry); node;
             node = node->parent)
          if (node->GetAppCulled() ||
              netimmerse_cast<RE::NiBillboardNode *>(node))
            return RE::BSVisit::BSVisitControl::kContinue;
        const auto &data = geometry->GetGeometryRuntimeData();
        auto *property = netimmerse_cast<RE::BSLightingShaderProperty *>(
            data.shaderProperty.get());
        // Flames/particles are effects, not the object's textured surfaces.
        if (!property)
          return RE::BSVisit::BSVisitControl::kContinue;
        auto *shape = geometry->AsTriShape();
        auto *gpu = data.rendererData;
        if (!shape || !gpu || !gpu->vertexBuffer || !gpu->indexBuffer ||
            !property->material ||
            property->material->GetType() !=
                RE::BSShaderMaterial::Type::kLighting ||
            !gpu->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
          ++unsupported;
          return RE::BSVisit::BSVisitControl::kContinue;
        }
        auto *material =
            static_cast<RE::BSLightingShaderMaterialBase *>(property->material);
        Surface surface;
        surface.vertices = reinterpret_cast<ID3D11Buffer *>(gpu->vertexBuffer);
        surface.indices = reinterpret_cast<ID3D11Buffer *>(gpu->indexBuffer);
        if (material->diffuseTexture &&
            material->diffuseTexture->rendererTexture)
          surface.diffuse = reinterpret_cast<ID3D11ShaderResourceView *>(
              material->diffuseTexture->rendererTexture->resourceView);
        std::uint64_t descriptor{};
        std::memcpy(&descriptor, &gpu->vertexDesc, sizeof(descriptor));
        surface.stride = unsigned(descriptor & 15) * 4;
        surface.hasUV = gpu->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV);
        surface.uvOffset = gpu->vertexDesc.GetAttributeOffset(
            RE::BSGraphics::Vertex::VA_TEXCOORD0);
        // SSE NIF stream 100 stores full-precision positions even when its
        // authored FULLPREC bit is absent. The UV offset also describes this.
        surface.fullPrecision =
            gpu->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ||
            !surface.hasUV || surface.uvOffset >= 12;
        surface.indexCount =
            unsigned(shape->GetTrishapeRuntimeData().triangleCount) * 3;
        D3D11_BUFFER_DESC vb{}, ib{};
        surface.vertices->GetDesc(&vb);
        surface.indices->GetDesc(&ib);
        if (!surface.indexCount || surface.indexCount > ib.ByteWidth / 2 ||
            surface.stride < 8 ||
            std::uint64_t(shape->GetTrishapeRuntimeData().vertexCount) *
                    surface.stride >
                vb.ByteWidth ||
            (surface.hasUV && surface.uvOffset + 4 > surface.stride)) {
          ++unsupported;
          return RE::BSVisit::BSVisitControl::kContinue;
        }
        surface.local = geometry->world;
        surface.twoSided = property->flags.any(
            RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
        surface.alpha = std::clamp(material->materialAlpha, 0.F, 1.F);
        surface.uv[0] = material->texCoordScale[0].x;
        surface.uv[1] = material->texCoordScale[0].y;
        surface.uv[2] = material->texCoordOffset[0].x;
        surface.uv[3] = material->texCoordOffset[0].y;
        if (auto *alpha = data.alphaProperty.get()) {
          surface.blendAlpha = alpha->GetAlphaBlending();
          if (alpha->GetAlphaTesting())
            surface.alphaTest = (alpha->alphaFlags >> 10) & 7;
          surface.threshold = alpha->alphaThreshold / 255.F;
        }
        model->surfaces.push_back(std::move(surface));
        return RE::BSVisit::BSVisitControl::kContinue;
      });
  if (unsupported)
    spdlog::warn("Diffuse preview {:08X}: {} unsupported geometry surfaces",
                 base->GetFormID(), unsupported);
  if (model->surfaces.empty())
    return {};
  cache.push_front({base->GetFormID(), model});
  if (cache.size() > 8)
    cache.pop_back();
  return model;
}
} // namespace SkyBuild::DiffusePreview
