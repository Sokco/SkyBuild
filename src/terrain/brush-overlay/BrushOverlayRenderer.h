#pragma once
#include "terrain/brush-overlay/BrushOverlayVertex.h"
#include "terrain/brush-overlay/SceneFrameSnapshot.h"
#include <d3d11_1.h>
#include <span>
#include <wrl/client.h>
namespace SkyBuild::Terrain::BrushOverlay {
// Self-contained renderer; all GPU objects are bounded and reused. Render
// targets/depth views are borrowed for the draw and never retained across
// resize.
class Renderer {
public:
  static bool HasSceneFrame(ID3D11DeviceContext *context);
  bool Draw(ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
            ID3D11ShaderResourceView *depth,
            std::span<const OverlayVertex> triangles, const float matrix[4][4],
            const Brush &brush, float depthWidthRatio = 1.F,
            float depthHeightRatio = 1.F, float jitterU = 0.F,
            float jitterV = 0.F, bool reversedDepth = false,
            const SceneFrameBinding &drawFrame = {});

private:
  bool Initialize(ID3D11Device *device);
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  Ptr<ID3D11Device> owner;
  Ptr<ID3D11DeviceContext1> context1;
  Ptr<ID3DDeviceContextState> state;
  Ptr<ID3D11VertexShader> vs;
  Ptr<ID3D11PixelShader> ps, compositePS;
  Ptr<ID3D11VertexShader> compositeVS;
  Ptr<ID3D11Texture2D> mask;
  Ptr<ID3D11RenderTargetView> maskRTV;
  Ptr<ID3D11ShaderResourceView> maskSRV;
  unsigned maskWidth{}, maskHeight{};
  Ptr<ID3D11InputLayout> layout;
  Ptr<ID3D11Buffer> vertices, constants;
  Ptr<ID3D11BlendState> blend, maximum;
  Ptr<ID3D11RasterizerState> raster;
  Ptr<ID3D11DepthStencilState> noDepth;
  unsigned capacity{};
  bool initialized{};
};
} // namespace SkyBuild::Terrain::BrushOverlay
