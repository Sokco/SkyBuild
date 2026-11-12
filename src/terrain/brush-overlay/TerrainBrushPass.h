#pragma once
#include "terrain/brush-overlay/BrushOverlayVertex.h"
#include "rendering/OverlayBackend.h"
#include <d3d11_1.h>
#include <span>
#include <wrl/client.h>
namespace SkyBuild::Terrain::BrushOverlay {
class TerrainBrushPass {
public:
  bool Draw(Rendering::OverlayBackend &backend,
            std::span<const OverlayVertex> triangles, const Brush &brush);

private:
  bool Initialize(Rendering::OverlayBackend &backend);
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  Ptr<ID3D11Device> owner;
  Rendering::Program program, composite;
  Ptr<ID3D11Texture2D> mask;
  Ptr<ID3D11RenderTargetView> maskRTV;
  Ptr<ID3D11ShaderResourceView> maskSRV;
  unsigned maskWidth{}, maskHeight{};
  Ptr<ID3D11InputLayout> layout;
  Ptr<ID3D11Buffer> vertices, constants;
  unsigned capacity{};
  bool initialized{};
};
} // namespace SkyBuild::Terrain::BrushOverlay
