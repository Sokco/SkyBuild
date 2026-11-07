#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace SkyBuild::Models {
// Read a private graph from the asset rather than copying mutable state from
// BSModelDB's cached graph. Call only when creating a visual, on the game
// thread.
inline RE::NiPointer<RE::NiNode> LoadAsset(const char *modelPath) {
  if (!modelPath || !*modelPath)
    return {};
  std::string path(modelPath);
  std::replace(path.begin(), path.end(), '/', '\\');
  const bool hasPrefix =
      path.size() >= 7 && _strnicmp(path.c_str(), "meshes\\", 7) == 0;
  if (!hasPrefix)
    path.insert(0, "meshes\\");
  RE::BSResourceNiBinaryStream file(path);
  if (!file.good()) {
    spdlog::warn("Could not open visual model asset '{}'.", path);
    return {};
  }
  RE::BSModelDB::DBTraits::ArgsType args{};
  RE::NiPointer<RE::NiNode> model;
  std::uint32_t type{};
  // Native BSModelDB resource-load callback, below the cache lookup. It owns
  // stream construction/destruction, texture setup and model post-processing.
  // Verified AE 1.6.1170: 75773 at E03300; five arguments, output at arg 4.
  using Load = void (*)(const RE::BSTSmartPointer<RE::BSResource::Stream> &,
                        const char *, const RE::BSModelDB::DBTraits::ArgsType &,
                        RE::NiPointer<RE::NiNode> &, std::uint32_t &);
  static REL::Relocation<Load> load{RELOCATION_ID(74031, 75773)};
  load(file.stream, path.c_str(), args, model, type);
  if (!model)
    spdlog::warn("Could not load visual model asset '{}'.", path);
  return model;
}

inline RE::TESModel *WorldModel(RE::TESBoundObject *object) {
  if (auto armor = object ? object->As<RE::TESObjectARMO>() : nullptr) {
    auto &male = armor->worldModels[RE::TESBipedModelForm::Sexes::kMale];
    return male.GetModel() && *male.GetModel()
               ? &male
               : &armor->worldModels[RE::TESBipedModelForm::Sexes::kFemale];
  }
  return object ? object->As<RE::TESModel>() : nullptr;
}

inline RE::NiPointer<RE::NiNode> LoadWorldModel(RE::TESBoundObject *object) {
  auto component = WorldModel(object);
  auto root = LoadAsset(component ? component->GetModel() : nullptr);
  auto swaps = component ? component->GetAsModelTextureSwap() : nullptr;
  if (!root || !swaps || !swaps->alternateTextures ||
      !swaps->numAlternateTextures)
    return root;
  std::uint32_t index = 0;
  RE::BSVisit::TraverseScenegraphGeometries(
      root.get(), [&](RE::BSGeometry *geometry) {
        const auto shapeIndex = index++;
        for (std::uint32_t i = 0; i < swaps->numAlternateTextures; ++i) {
          const auto &swap = swaps->alternateTextures[i];
          if (swap.index3D != shapeIndex || !swap.textureSet)
            continue;
          auto &data = geometry->GetGeometryRuntimeData();
          auto prop = netimmerse_cast<RE::BSLightingShaderProperty *>(
              data.shaderProperty.get());
          if (!prop || !prop->material ||
              prop->material->GetType() !=
                  RE::BSShaderMaterial::Type::kLighting)
            break;
          RE::NiPointer<RE::BSShaderTextureSet> textures{
              RE::BSShaderTextureSet::Create()};
          if (!textures)
            break;
          for (unsigned slot = 0; slot < RE::BSTextureSet::Texture::kUsedTotal;
               ++slot) {
            auto channel = static_cast<RE::BSTextureSet::Texture>(slot);
            if (auto path = swap.textureSet->GetTexturePath(channel);
                path && *path)
              textures->SetTexturePath(channel, path);
          }
          auto copy = static_cast<RE::BSLightingShaderMaterialBase *>(
              prop->material->Create());
          if (!copy)
            break;
          copy->CopyMembers(prop->material);
          copy->ClearTextures();
          copy->SetTextureSet(textures);
          copy->OnLoadTextureSet(0, textures.get());
          prop->SetMaterial(copy, true);
          prop->DoClearRenderPasses();
          geometry->SetMaterialNeedsUpdate(true);
          break;
        }
        return RE::BSVisit::BSVisitControl::kContinue;
      });
  return root;
}
} // namespace SkyBuild::Models
