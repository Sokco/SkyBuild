#pragma once
#include "building/materials/ShellVoxelizer.h"
#include <functional>

namespace SkyBuild::UI::ThumbnailRenderer {
using Bytes = std::vector<std::uint8_t>;
using ResourceReader = std::function<Bytes(const std::string &)>;
struct TextureSwap {
  unsigned index;
  std::string shape, diffuse;
};
struct Input {
  std::string model;
  std::vector<TextureSwap> swaps;
  float yaw{}, brightness{1};
  bool resource{}, analyze{};
};
struct Result {
  Bytes pixels;
  MaterialShell::Mesh materials;
};
struct Bounds {
  std::array<float, 3> min, max;
};
// CPU-only NIF vertex bounds, including authored node transforms.
Bounds ReadBounds(const std::string &model, const ResourceReader &);
// CPU-only geometry/texture-name extraction, with form texture swaps applied.
// Does not load DDS images, initialize D3D, or generate pixels.
MaterialShell::Mesh ReadMaterials(const Input &, const ResourceReader &,
                                  const std::atomic_bool &cancel);
// All storage, including the D3D device/context, is private to this renderer.
// Called serially by the thumbnail worker. Pixels are straight-alpha BGRA.
Result Render(const Input &, const ResourceReader &,
              const std::atomic_bool &cancel);
} // namespace SkyBuild::UI::ThumbnailRenderer
