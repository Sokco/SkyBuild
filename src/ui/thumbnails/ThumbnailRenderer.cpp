#include "ui/thumbnails/ThumbnailRenderer.h"
#include <NifFile.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <directxtk/DDSTextureLoader.h>
#include <format>
#include <limits>
#include <numbers>
#include <spanstream>
#include <sstream>
#include <unordered_set>
#include <wrl/client.h>

namespace SkyBuild::UI::ThumbnailRenderer {
namespace {
using Microsoft::WRL::ComPtr;
using nifly::Vector3;
constexpr UINT size = 512;
struct Vertex {
  Vector3 position, normal;
  nifly::Vector2 uv;
  nifly::Color4 color;
};
struct Surface {
  std::string texture;
  std::vector<Vertex> vertices;
  std::vector<nifly::Triangle> triangles;
  float cutoff = -1;
  bool blend = false;
};

void Check(HRESULT hr, const char *operation) {
  if (FAILED(hr))
    throw std::runtime_error(std::format("{} failed ({:08X})", operation,
                                         static_cast<unsigned>(hr)));
}
std::string ResourcePath(std::string path, std::string_view folder) {
  std::replace(path.begin(), path.end(), '\\', '/');
  std::ranges::transform(path, path.begin(),
                         [](unsigned char c) { return char(std::tolower(c)); });
  if (!path.empty() && !path.starts_with(folder))
    path.insert(0, folder);
  return path;
}

// Apply authored transforms and form texture swaps for rendering and analysis.
std::vector<Surface> ReadSurfaces(const Input &input,
                                  const ResourceReader &read,
                                  MaterialShell::Mesh &materials,
                                  const std::atomic_bool &cancel,
                                  bool geometryOnly = false) {
  auto bytes = read(ResourcePath(input.model, "meshes/"));
  if (bytes.empty())
    throw std::runtime_error("Model resource unavailable: " + input.model);
  std::ispanstream stream(std::span<const char>{
      reinterpret_cast<const char *>(bytes.data()), bytes.size()});
  nifly::NifFile file;
  if (file.Load(stream))
    throw std::runtime_error("Unsupported or invalid NIF: " + input.model);
  std::vector<Surface> surfaces;
  const auto shapes = file.GetShapes();
  for (size_t i = 0; i < shapes.size(); ++i) {
    if (cancel)
      throw std::runtime_error("Cancelled");
    auto shape = shapes[i];
    auto transform = shape->GetTransformToParent();
    bool hidden = (shape->flags & 1) != 0;
    std::unordered_set<nifly::NiNode *> ancestors;
    for (auto parent = file.GetParentNode(shape); parent;
         parent = file.GetParentNode(parent)) {
      if (!ancestors.insert(parent).second)
        throw std::runtime_error("Cycle in NIF hierarchy");
      hidden |= (parent->flags & 1) != 0;
      // Camera-facing effects are not part of the preview's solid model.
      hidden |= dynamic_cast<nifly::NiBillboardNode *>(parent) != nullptr;
      transform = parent->GetTransformToParent().ComposeTransforms(transform);
    }
    if (hidden)
      continue;
    nifly::MatTransform globalToSkin;
    if (shape->IsSkinned() &&
        file.GetShapeTransformGlobalToSkin(shape, globalToSkin))
      transform = globalToSkin.InverseTransform();

    Surface surface;
    auto positions = file.GetVertsForShape(shape);
    if (!positions || positions->empty() ||
        !shape->GetTriangles(surface.triangles) || surface.triangles.empty())
      continue;
    auto normals = geometryOnly ? nullptr : file.GetNormalsForShape(shape);
    auto uv = geometryOnly ? nullptr : file.GetUvsForShape(shape);
    auto colors = geometryOnly ? nullptr : file.GetColorsForShape(shape);
    auto shader = file.GetShader(shape);
    const auto scale = shader ? shader->GetUVScale() : nifly::Vector2{1, 1};
    const auto offset = shader ? shader->GetUVOffset() : nifly::Vector2{};
    if (auto alpha = file.GetAlphaProperty(shape)) {
      surface.blend = (alpha->flags & 1) != 0;
      if (alpha->flags & (1 << 9))
        surface.cutoff = alpha->threshold / 255.F;
    }
    file.GetTextureSlot(shape, surface.texture);
    for (auto &swap : input.swaps) {
      if (swap.index == i &&
          (swap.shape.empty() ||
           _stricmp(shape->name.get().c_str(), swap.shape.c_str()) == 0)) {
        surface.texture = swap.diffuse;
        break;
      }
    }
    surface.texture = ResourcePath(surface.texture, "textures/");
    surface.vertices.reserve(positions->size());
    for (size_t v = 0; v < positions->size(); ++v) {
      Vertex vertex{};
      vertex.position = transform.ApplyTransform((*positions)[v]);
      if (normals && v < normals->size())
        vertex.normal = transform.ApplyTransformToDir((*normals)[v]);
      if (uv && v < uv->size())
        vertex.uv = {(*uv)[v].u * scale.u + offset.u,
                     (*uv)[v].v * scale.v + offset.v};
      vertex.color = colors && v < colors->size() ? (*colors)[v]
                                                  : nifly::Color4{1, 1, 1, 1};
      if (shader && !shader->HasVertexColors())
        vertex.color.r = vertex.color.g = vertex.color.b = 1;
      if (shader && !shader->HasVertexAlpha())
        vertex.color.a = 1;
      vertex.color.a *= shader ? shader->GetAlpha() : 1.F;
      surface.vertices.push_back(vertex);
    }
    auto textureId = static_cast<unsigned>(materials.textures.size());
    if (input.analyze) {
      auto it = std::ranges::find(materials.textures, surface.texture);
      textureId = static_cast<unsigned>(it - materials.textures.begin());
      if (it == materials.textures.end())
        materials.textures.push_back(surface.texture);
    }
    for (const auto &tri : surface.triangles) {
      if (std::max({tri.p1, tri.p2, tri.p3}) >= surface.vertices.size())
        throw std::runtime_error("Invalid NIF triangle index");
      if (!geometryOnly && (!normals || normals->size() != positions->size())) {
        auto &v = surface.vertices;
        auto n = (v[tri.p2].position - v[tri.p1].position)
                     .cross(v[tri.p3].position - v[tri.p1].position);
        for (auto index : {tri.p1, tri.p2, tri.p3})
          v[index].normal += n;
      }
      if (input.analyze) {
        MaterialShell::Triangle triangle{};
        triangle.texture = textureId;
        triangle.mesh = static_cast<unsigned>(i + 1);
        size_t j = 0;
        for (auto index : {tri.p1, tri.p2, tri.p3}) {
          auto p = surface.vertices[index].position;
          triangle.points[j++] = {p.x, p.y, p.z};
        }
        materials.triangles.push_back(triangle);
      }
    }
    surfaces.push_back(std::move(surface));
  }
  if (surfaces.empty())
    throw std::runtime_error("No visible triangle surfaces in " + input.model);
  return surfaces;
}

constexpr char shaderSource[] = R"(
cbuffer Material : register(b0) { float brightness; float cutoff; uint textured; uint useAlpha; };
struct V { float3 p:POSITION; float3 n:NORMAL; float2 uv:TEXCOORD; float4 c:COLOR; };
struct P { float4 p:SV_POSITION; float3 n:NORMAL; float2 uv:TEXCOORD; float4 c:COLOR; };
P vs(V v) { P o; o.p=float4(v.p,1); o.n=v.n; o.uv=v.uv; o.c=v.c; return o; }
Texture2D diffuse : register(t0); SamplerState samp : register(s0);
float4 ps(P v) : SV_TARGET {
    float4 tex = textured ? diffuse.Sample(samp,v.uv) : float4(1,1,1,1);
    float a = v.c.a * (useAlpha ? tex.a : 1);
    clip(a-max(cutoff,0.003));
    float3 n=v.n / max(length(v.n),0.001);
    float light=0.44+0.62*saturate(dot(n,float3(-0.46,0.70,-0.55)))+0.22*saturate(dot(n,float3(0.81,0.17,-0.56)));
    return float4(tex.rgb*v.c.rgb*brightness*light,a);
})";

struct Pipeline {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<ID3D11Texture2D> color, staging;
  ComPtr<ID3D11RenderTargetView> target;
  ComPtr<ID3D11DepthStencilView> depth;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> raster;
  ComPtr<ID3D11BlendState> blend;

  Pipeline() {
    auto create = [&](D3D_DRIVER_TYPE type) {
      return D3D11CreateDevice(
          nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
          D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr,
          ctx.ReleaseAndGetAddressOf());
    };
    if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE)))
      Check(create(D3D_DRIVER_TYPE_WARP), "Create thumbnail device");
    auto compile = [](const char *entry, const char *profile) {
      ComPtr<ID3DBlob> code, errors;
      Check(D3DCompile(shaderSource, sizeof(shaderSource), nullptr, nullptr,
                       nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                       0, &code, &errors),
            "Compile thumbnail shader");
      return code;
    };
    auto vertex = compile("vs", "vs_4_0"), pixel = compile("ps", "ps_4_0");
    Check(device->CreateVertexShader(vertex->GetBufferPointer(),
                                     vertex->GetBufferSize(), nullptr, &vs),
          "Create VS");
    Check(device->CreatePixelShader(pixel->GetBufferPointer(),
                                    pixel->GetBufferSize(), nullptr, &ps),
          "Create PS");
    const D3D11_INPUT_ELEMENT_DESC fields[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
         offsetof(Vertex, position)},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal)},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv)},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
         offsetof(Vertex, color)}};
    Check(device->CreateInputLayout(fields, 4, vertex->GetBufferPointer(),
                                    vertex->GetBufferSize(), &layout),
          "Create vertex layout");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Check(device->CreateTexture2D(&desc, nullptr, &color),
          "Create color target");
    Check(device->CreateRenderTargetView(color.Get(), nullptr, &target),
          "Create color view");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateTexture2D(&desc, nullptr, &staging),
          "Create thumbnail readback");
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    ComPtr<ID3D11Texture2D> z;
    Check(device->CreateTexture2D(&desc, nullptr, &z), "Create depth target");
    Check(device->CreateDepthStencilView(z.Get(), nullptr, &depth),
          "Create depth view");
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Check(device->CreateBuffer(&cb, nullptr, &constants),
          "Create material constants");
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Check(device->CreateSamplerState(&sm, &sampler), "Create texture sampler");
    D3D11_RASTERIZER_DESC rs{};
    rs.CullMode = D3D11_CULL_NONE;
    rs.FillMode = D3D11_FILL_SOLID;
    rs.DepthClipEnable = true;
    Check(device->CreateRasterizerState(&rs, &raster), "Create rasterizer");
    D3D11_BLEND_DESC bd{};
    auto &rt = bd.RenderTarget[0];
    rt.BlendEnable = true;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(device->CreateBlendState(&bd, &blend), "Create alpha blend");
  }
  ComPtr<ID3D11Buffer> Buffer(const void *data, size_t bytes, UINT bind) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = static_cast<UINT>(bytes);
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial{data};
    ComPtr<ID3D11Buffer> buffer;
    Check(device->CreateBuffer(&d, &initial, &buffer), "Upload thumbnail mesh");
    return buffer;
  }
  Bytes Draw(std::vector<Surface> &surfaces, const Input &input,
             const ResourceReader &read, const std::atomic_bool &cancel) {
    const float radians = std::numbers::pi_v<float> / 180.F;
    const float az = ((input.resource ? 45.F : 35.F) - input.yaw) * radians,
                el = (input.resource ? 45.F : 25.F) * radians;
    auto project = [&](Vector3 p) {
      const float h = p.x * std::sin(az) + p.y * std::cos(az);
      return Vector3{p.x * std::cos(az) - p.y * std::sin(az),
                     p.z * std::cos(el) - h * std::sin(el),
                     -p.z * std::sin(el) - h * std::cos(el)};
    };
    Vector3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (auto &s : surfaces) {
      for (auto &v : s.vertices) {
        v.position = project(v.position);
        v.normal = project(v.normal);
        const auto p = v.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          throw std::runtime_error("Non-finite mesh position");
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
    }
    const float scale = 1.84F / std::max({hi.x - lo.x, hi.y - lo.y, 0.0001F});
    const float clear[4]{};
    ctx->OMSetRenderTargets(1, target.GetAddressOf(), depth.Get());
    ctx->ClearRenderTargetView(target.Get(), clear);
    ctx->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(raster.Get());
    ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    ctx->OMSetBlendState(blend.Get(), nullptr, ~0u);
    std::stable_partition(surfaces.begin(), surfaces.end(),
                          [](auto &s) { return !s.blend; });
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> textures;
    for (auto &s : surfaces) {
      if (cancel)
        throw std::runtime_error("Cancelled");
      for (auto &v : s.vertices) {
        auto &p = v.position;
        p.x = (p.x - (lo.x + hi.x) * .5F) * scale;
        p.y = (p.y - (lo.y + hi.y) * .5F) * scale;
        p.z = .01F + .98F * (p.z - lo.z) / std::max(hi.z - lo.z, .0001F);
      }
      auto [entry, inserted] = textures.try_emplace(s.texture);
      if (inserted && !s.texture.empty()) {
        auto bytes = read(s.texture);
        if (!bytes.empty())
          Check(DirectX::CreateDDSTextureFromMemory(
                    device.Get(), bytes.data(), bytes.size(), nullptr,
                    entry->second.GetAddressOf()),
                "Load DDS");
      }
      struct {
        float brightness, cutoff;
        UINT textured, useAlpha;
      } material{input.brightness, s.cutoff, entry->second ? 1u : 0u,
                 (s.blend || s.cutoff >= 0) ? 1u : 0u};
      ctx->UpdateSubresource(constants.Get(), 0, nullptr, &material, 0, 0);
      ctx->PSSetShaderResources(0, 1, entry->second.GetAddressOf());
      static_assert(sizeof(nifly::Triangle) == 6);
      auto vb = Buffer(s.vertices.data(), s.vertices.size() * sizeof(Vertex),
                       D3D11_BIND_VERTEX_BUFFER);
      auto ib = Buffer(s.triangles.data(), s.triangles.size() * 6,
                       D3D11_BIND_INDEX_BUFFER);
      UINT stride = sizeof(Vertex), offset = 0;
      ctx->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
      ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
      ctx->DrawIndexed(static_cast<UINT>(s.triangles.size() * 3), 0, 0);
    }
    ctx->ClearState();
    ctx->CopyResource(staging.Get(), color.Get());
    Bytes pixels(size * size * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "Read thumbnail");
    for (UINT y = 0; y < size; ++y)
      std::memcpy(pixels.data() + y * size * 4,
                  static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch,
                  size * 4);
    ctx->Unmap(staging.Get(), 0);
    bool visible = false;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      unsigned a = pixels[i + 3];
      if (!a)
        continue;
      visible = true;
      if (a < 255)
        for (size_t j = i; j < i + 3; ++j)
          pixels[j] =
              static_cast<BYTE>(std::min(255u, (pixels[j] * 255u + a / 2) / a));
    }
    if (!visible)
      throw std::runtime_error("No visible pixels in " + input.model);
    return pixels;
  }
};
} // namespace
Bounds ReadBounds(const std::string &model, const ResourceReader &read) {
  Input input;
  input.model = model;
  MaterialShell::Mesh unused;
  std::atomic_bool cancel{};
  const auto surfaces = ReadSurfaces(input, read, unused, cancel);
  const auto infinity = std::numeric_limits<float>::infinity();
  Bounds bounds{{infinity, infinity, infinity},
                {-infinity, -infinity, -infinity}};
  for (const auto &surface : surfaces)
    for (const auto &vertex : surface.vertices) {
      const std::array<float, 3> point{vertex.position.x, vertex.position.y,
                                       vertex.position.z};
      for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(point[axis]))
          throw std::runtime_error("Invalid model vertex");
        bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
        bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
      }
    }
  return bounds;
}
MaterialShell::Mesh ReadMaterials(const Input &input,
                                  const ResourceReader &read,
                                  const std::atomic_bool &cancel) {
  auto analysis = input;
  analysis.analyze = true;
  MaterialShell::Mesh materials;
  ReadSurfaces(analysis, read, materials, cancel, true);
  return materials;
}
Result Render(const Input &input, const ResourceReader &read,
              const std::atomic_bool &cancel) {
  Result result;
  auto surfaces = ReadSurfaces(input, read, result.materials, cancel);
  static Pipeline pipeline;
  result.pixels = pipeline.Draw(surfaces, input, read, cancel);
  return result;
}
} // namespace SkyBuild::UI::ThumbnailRenderer
