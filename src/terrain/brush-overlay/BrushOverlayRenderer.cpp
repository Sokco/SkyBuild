#include "terrain/brush-overlay/BrushOverlayRenderer.h"
#include <cstring>
#include <d3dcompiler.h>
namespace SkyBuild::Terrain::BrushOverlay {
namespace {
// Embedded source keeps deployment to a single plugin; compiled lazily once.
constexpr char shader[] = R"(
cbuffer BrushData : register(b0) {
  row_major float4x4 worldToClip;
  float4 brush;
  float4 viewport; // target width/height, depth active width/height
  float4 depthJitter; // scene depth UV jitter; overlay geometry stays unjittered
};
// Standard scene PerFrame layout (engine/Community/Bottled Shaders b12).
// Borrow it at b1 in this private pass; never modify the game buffer.
cbuffer SceneFrame : register(b1) {
  row_major float4x4 sceneView : packoffset(c0);
  row_major float4x4 sceneProj : packoffset(c4);
  row_major float4x4 sceneViewProj : packoffset(c8);
  row_major float4x4 sceneViewProjUnjittered : packoffset(c12);
  float4 sceneCameraPosition : packoffset(c40);
  float4 sceneDynamicResolution : packoffset(c43);
};
Texture2D<float> sceneDepth : register(t0);
struct Fragment {
  float4 position : SV_POSITION;
  float3 world : TEXCOORD0;
  noperspective float4 scene : TEXCOORD1; // NDC and reciprocal clip W.
};
Fragment VS(float3 p : POSITION) {
  Fragment o;
  o.world = depthJitter.z > 0.5 ? p - sceneCameraPosition.xyz : p;
  o.position = depthJitter.z > 0.5
      ? mul(sceneViewProjUnjittered, float4(o.world,1))
      : mul(worldToClip, float4(p, 1));
  // Project once at vertices and interpolate NDC in screen space. Reprojecting
  // perspective-interpolated world positions per pixel introduces a second
  // rounded projection, especially on grazing triangles.
  precise float4 sceneClip = depthJitter.z > 0.5
      ? mul(sceneViewProj, float4(o.world,1)) : o.position;
  o.scene = float4(sceneClip.xyz,1) / sceneClip.w;
  return o;
}
float4 PS(Fragment i) : SV_TARGET {
  float3 center = depthJitter.z > 0.5 ? brush.xyz - sceneCameraPosition.xyz : brush.xyz;
  float distance = length(i.world - center) / brush.w;
  clip(1 - distance);
  uint w, h;
  sceneDepth.GetDimensions(w, h);
  float2 sampleUV;
  float expectedDepth;
  bool reversed = depthJitter.w > 0.5;
  float bias = 0.00003;
  float2 activeSize = viewport.zw;
  if (depthJitter.z > 0.5) {
    // Project the SAME camera-relative position with the actual jittered GPU
    // matrix that wrote scene depth, not NiCamera plus an estimated offset.
    float3 actualNDC = i.scene.xyz;
    sampleUV = actualNDC.xy * float2(0.5,-0.5) + 0.5;
    expectedDepth = actualNDC.z;
    float facing = sceneProj[3][2] < 0 ? -1 : 1;
    float4 closeClip = mul(sceneProj,float4(0,0,facing,1));
    float4 distantClip = mul(sceneProj,float4(0,0,2*facing,1));
    reversed = distantClip.z * closeClip.w < closeClip.z * distantClip.w;
    // Move the visibility test about 0.75 game units toward the camera.
    // Convert that distance through this projection instead of using a large
    // constant NDC offset whose world-space size grows with distance.
    float derivative = abs(sceneProj[2][2] * sceneProj[3][3] -
                           sceneProj[2][3] * sceneProj[3][2]);
    bias = clamp(0.75 * derivative * i.scene.w * i.scene.w,
                 0.000002,0.0002);
    activeSize = float2(w,h) * sceneDynamicResolution.xy;
  } else {
    sampleUV = i.position.xy / viewport.xy + depthJitter.xy;
    expectedDepth = i.position.z;
  }
  float2 depthCoord = sampleUV * activeSize;
  int2 first = int2(floor(depthCoord - 0.5));
  // Derivatives in depth-texture coordinates, accounting for both jittered
  // projection and render scaling. Avoid a screen-size approximation.
  float2 uvDx = ddx(depthCoord), uvDy = ddy(depthCoord);
  float zDx = ddx(expectedDepth), zDy = ddy(expectedDepth);
  float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
  float2 slope = abs(determinant) > 1e-8
      ? float2(zDx * uvDy.y - zDy * uvDx.y, uvDx.x * zDy - uvDy.x * zDx) / determinant
      : float2(0,0);
  float clearance = -1e10;
  // Compare the local depth plane at each actual depth texel center. This
  // handles subpixel jitter/scaling without interpolating foreground depth
  // across an occlusion edge or enlarging the accepted depth range.
  [unroll] for (int y=0; y<2; ++y)
    [unroll] for (int x=0; x<2; ++x) {
      int2 pixel = clamp(first + int2(x,y), int2(0,0), int2(w,h)-1);
      float expected = expectedDepth + dot(slope, float2(pixel) + 0.5 - depthCoord);
      float depth = sceneDepth.Load(int3(pixel,0));
      float separation = reversed ? expected - depth : depth - expected;
      clearance = max(clearance,separation);
    }
  // A depth test, not an equality window: surfaces nearer than scene depth
  // remain visible. Only terrain hidden behind scene depth is rejected, with
  // a small camera-facing bias and a soft transition at its far limit.
  float coverage = smoothstep(-bias,-0.5*bias,clearance);
  float edge = (1 - smoothstep(0.86, 1, distance));
  float rim = smoothstep(0.78, 0.92, distance);
  return float4(coverage * edge * (0.24 + rim * 0.25), 0, 0, 1);
}
float4 CompositeVS(uint id : SV_VertexID) : SV_POSITION {
  return float4(float2((id << 1) & 2, id & 2) * float2(2,-2) + float2(-1,1),0,1);
}
float4 CompositePS(float4 p : SV_POSITION) : SV_TARGET {
  return float4(0.08, 0.46, 1, sceneDepth.Load(int3(int2(p.xy),0)));
}
)";
struct Constants {
  float matrix[4][4];
  float brush[4];
  float viewport[4];
  float jitter[4];
};
} // namespace
bool Renderer::Initialize(ID3D11Device *device) {
  if (owner.Get() == device)
    return initialized;
  // Drop all old-device resources before rebuilding.
  owner = device;
  initialized = false;
  context1.Reset();
  state.Reset();
  vs.Reset();
  ps.Reset();
  compositePS.Reset();
  compositeVS.Reset();
  mask.Reset();
  maskRTV.Reset();
  maskSRV.Reset();
  maskWidth = maskHeight = 0;
  maximum.Reset();
  layout.Reset();
  vertices.Reset();
  constants.Reset();
  blend.Reset();
  raster.Reset();
  noDepth.Reset();
  capacity = 0;
  Ptr<ID3D11Device1> device1;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
    return false;
  Ptr<ID3D11DeviceContext> immediate;
  device->GetImmediateContext(&immediate);
  if (FAILED(immediate.As(&context1)))
    return false;
  const auto level = device->GetFeatureLevel();
  if (FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                               __uuidof(ID3D11Device), nullptr,
                                               &state)))
    return false;
  Ptr<ID3DBlob> vertexCode, pixelCode, errors;
  if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildBrushOverlay",
                        nullptr, nullptr, "VS", "vs_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexCode,
                        &errors)) ||
      FAILED(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildBrushOverlay",
                        nullptr, nullptr, "PS", "ps_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelCode, &errors)))
    return false;
  if (FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
                                        vertexCode->GetBufferSize(), nullptr,
                                        &vs)) ||
      FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(),
                                       pixelCode->GetBufferSize(), nullptr,
                                       &ps)))
    return false;
  const D3D11_INPUT_ELEMENT_DESC input{
      "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
      0,          0, D3D11_INPUT_PER_VERTEX_DATA,
      0};
  static_assert(sizeof(OverlayVertex) == 12);
  if (FAILED(device->CreateInputLayout(&input, 1,
                                       vertexCode->GetBufferPointer(),
                                       vertexCode->GetBufferSize(), &layout)))
    return false;
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(Constants);
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cb, nullptr, &constants)))
    return false;
  D3D11_BLEND_DESC blending{};
  auto &b = blending.RenderTarget[0];
  b.BlendEnable = true;
  b.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  b.BlendOp = D3D11_BLEND_OP_ADD;
  b.SrcBlendAlpha = D3D11_BLEND_ZERO;
  b.DestBlendAlpha = D3D11_BLEND_ONE;
  b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  D3D11_RASTERIZER_DESC rs{};
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_NONE;
  rs.DepthClipEnable = true;
  D3D11_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = false;
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(device->CreateBlendState(&blending, &blend)) ||
      FAILED(device->CreateRasterizerState(&rs, &raster)) ||
      FAILED(device->CreateDepthStencilState(&ds, &noDepth)))
    return false;
  b.SrcBlend = b.DestBlend = D3D11_BLEND_ONE;
  b.BlendOp = D3D11_BLEND_OP_MAX;
  if (FAILED(device->CreateBlendState(&blending, &maximum)))
    return false;
  Ptr<ID3DBlob> compositeVertexCode, compositePixelCode;
  if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildBrushOverlay",
                        nullptr, nullptr, "CompositeVS", "vs_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &compositeVertexCode,
                        &errors)) ||
      FAILED(D3DCompile(shader, sizeof(shader) - 1, "SkyBuildBrushOverlay",
                        nullptr, nullptr, "CompositePS", "ps_5_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &compositePixelCode,
                        &errors)))
    return false;
  if (FAILED(device->CreateVertexShader(compositeVertexCode->GetBufferPointer(),
                                        compositeVertexCode->GetBufferSize(),
                                        nullptr, &compositeVS)) ||
      FAILED(device->CreatePixelShader(compositePixelCode->GetBufferPointer(),
                                       compositePixelCode->GetBufferSize(),
                                       nullptr, &compositePS)))
    return false;
  initialized = true;
  return true;
}
bool Renderer::HasSceneFrame(ID3D11DeviceContext *context) {
  return bool(ReadSceneFrame(context).buffer);
}
bool Renderer::Draw(ID3D11DeviceContext *context,
                    ID3D11RenderTargetView *target,
                    ID3D11ShaderResourceView *depth,
                    std::span<const OverlayVertex> triangles,
                    const float matrix[4][4], const Brush &brush,
                    float depthWidthRatio, float depthHeightRatio,
                    float jitterU, float jitterV, bool reversedDepth,
                    const SceneFrameBinding &drawFrame) {
  if (!context || !target || !depth || triangles.empty() ||
      triangles.size() > 180000)
    return false;
  const auto frameBinding =
      drawFrame.buffer ? drawFrame : ReadSceneFrame(context);
  const auto &sceneFrame = frameBinding.buffer;
  Ptr<ID3D11Device> device;
  context->GetDevice(&device);
  if (!Initialize(device.Get()))
    return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
  depth->GetDesc(&srv);
  if (srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
    return false;
  Ptr<ID3D11Resource> resource;
  target->GetResource(&resource);
  Ptr<ID3D11Texture2D> texture;
  if (FAILED(resource.As(&texture)))
    return false;
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  Ptr<ID3D11Resource> depthResource;
  depth->GetResource(&depthResource);
  Ptr<ID3D11Texture2D> depthTexture;
  if (FAILED(depthResource.As(&depthTexture)))
    return false;
  D3D11_TEXTURE2D_DESC depthDescription{};
  depthTexture->GetDesc(&depthDescription);
  if (!mask || maskWidth != description.Width ||
      maskHeight != description.Height) {
    mask.Reset();
    maskRTV.Reset();
    maskSRV.Reset();
    D3D11_TEXTURE2D_DESC md{};
    md.Width = description.Width;
    md.Height = description.Height;
    md.MipLevels = md.ArraySize = md.SampleDesc.Count = 1;
    md.Format = DXGI_FORMAT_R8_UNORM;
    md.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&md, nullptr, &mask)) ||
        FAILED(device->CreateRenderTargetView(mask.Get(), nullptr, &maskRTV)) ||
        FAILED(
            device->CreateShaderResourceView(mask.Get(), nullptr, &maskSRV))) {
      mask.Reset();
      return false;
    }
    maskWidth = md.Width;
    maskHeight = md.Height;
  }
  if (triangles.size() > capacity) {
    capacity =
        std::min(180000U, std::max(4096U, unsigned(triangles.size()) * 2));
    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = capacity * sizeof(OverlayVertex);
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertices.Reset();
    if (FAILED(device->CreateBuffer(&vb, nullptr, &vertices))) {
      capacity = 0;
      return false;
    }
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(
          context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return false;
  std::memcpy(mapped.pData, triangles.data(), triangles.size_bytes());
  context->Unmap(vertices.Get(), 0);
  Constants data{};
  std::memcpy(data.matrix, matrix, sizeof(data.matrix));
  data.jitter[2] = sceneFrame ? 1.F : 0.F;
  data.jitter[3] = reversedDepth ? 1.F : 0.F;
  data.jitter[0] = jitterU;
  data.jitter[1] = jitterV;
  data.brush[0] = brush.center.x;
  data.brush[1] = brush.center.y;
  data.brush[2] = brush.center.z;
  data.brush[3] = brush.radius;
  data.viewport[0] = float(description.Width);
  data.viewport[1] = float(description.Height);
  data.viewport[2] = float(depthDescription.Width) * depthWidthRatio;
  data.viewport[3] = float(depthDescription.Height) * depthHeightRatio;
  if (FAILED(context->Map(constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                          &mapped)))
    return false;
  std::memcpy(mapped.pData, &data, sizeof(data));
  context->Unmap(constants.Get(), 0);
  Ptr<ID3DDeviceContextState> previous;
  context1->SwapDeviceContextState(state.Get(), &previous);
  // Restore the full pipeline, including SRVs/UAVs, even with other render
  // mods.
  struct Restore {
    ID3D11DeviceContext1 *context;
    ID3DDeviceContextState *state;
    ~Restore() { context->SwapDeviceContextState(state, nullptr); }
  } restore{context1.Get(), previous.Get()};
  const D3D11_VIEWPORT viewport{
      0, 0, float(description.Width), float(description.Height), 0, 1};
  context->RSSetViewports(1, &viewport);
  context->RSSetState(raster.Get());
  auto *maskTarget = maskRTV.Get();
  const float clear[4]{};
  context->ClearRenderTargetView(maskTarget, clear);
  context->OMSetRenderTargets(1, &maskTarget, nullptr);
  context->OMSetBlendState(maximum.Get(), nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(noDepth.Get(), 0);
  context->IASetInputLayout(layout.Get());
  const UINT stride = sizeof(OverlayVertex), offset = 0;
  auto *vb = vertices.Get();
  context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->PSSetShader(ps.Get(), nullptr, 0);
  auto *cb = constants.Get();
  context->VSSetConstantBuffers(0, 1, &cb);
  context->PSSetConstantBuffers(0, 1, &cb);
  auto *frameBuffer = sceneFrame.Get();
  if (frameBinding.count) {
    context1->VSSetConstantBuffers1(1, 1, &frameBuffer, &frameBinding.first,
                                    &frameBinding.count);
    context1->PSSetConstantBuffers1(1, 1, &frameBuffer, &frameBinding.first,
                                    &frameBinding.count);
  } else {
    context->VSSetConstantBuffers(1, 1, &frameBuffer);
    context->PSSetConstantBuffers(1, 1, &frameBuffer);
  }
  context->PSSetShaderResources(0, 1, &depth);
  context->Draw(unsigned(triangles.size()), 0);
  // Resolve MAX coverage once per pixel, so duplicated/overlapping triangles
  // cannot accumulate alpha. The terrain mask is private and resize-aware.
  context->OMSetRenderTargets(1, &target, nullptr);
  context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);
  auto *maskView = maskSRV.Get();
  context->PSSetShaderResources(0, 1, &maskView);
  context->IASetInputLayout(nullptr);
  context->VSSetShader(compositeVS.Get(), nullptr, 0);
  context->PSSetShader(compositePS.Get(), nullptr, 0);
  context->Draw(3, 0);
  // Don't keep borrowed game views alive inside our inactive context state.
  ID3D11ShaderResourceView *empty{};
  context->PSSetShaderResources(0, 1, &empty);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  ID3D11Buffer *emptyBuffer{};
  context->VSSetConstantBuffers(1, 1, &emptyBuffer);
  context->PSSetConstantBuffers(1, 1, &emptyBuffer);
  return true;
}
} // namespace SkyBuild::Terrain::BrushOverlay
