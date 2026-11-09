#pragma once
#include "models/ModelData.h"
#include <atomic>
#include <functional>

namespace SkyBuild::UI::ThumbnailRenderer {
using Bytes = std::vector<std::uint8_t>;
using ResourceReader = std::function<Bytes(const std::string &)>;
struct Input {
  std::string model;
  std::vector<Models::TextureSwap> swaps;
  float yaw{}, brightness{1};
  bool resource{}, analyze{};
};
struct Result {
  Bytes pixels;
  Models::Mesh materials;
};
// CPU-only NIF vertex bounds, including authored node transforms.
Models::Bounds ReadBounds(const std::string &model, const ResourceReader &);
// CPU-only geometry/texture-name extraction, with form texture swaps applied.
// Does not load DDS images, initialize D3D, or generate pixels.
Models::Mesh ReadMaterials(const Input &, const ResourceReader &,
                                  const std::atomic_bool &cancel);
// All storage, including the D3D device/context, is private to this renderer.
// Called serially by the thumbnail worker. Pixels are straight-alpha BGRA.
Result Render(const Input &, const ResourceReader &,
              const std::atomic_bool &cancel);
} // namespace SkyBuild::UI::ThumbnailRenderer
