#include "ui/imgui/TexturePixels.h"
#include "ui/imgui/Widgets.h"
#include "ui/thumbnails/ThumbnailGenerator.h"
#include "ui/thumbnails/ThumbnailPaths.h"
#include <d3d11.h>
#include <DDSTextureLoader.h>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <unordered_map>
#include <wincodec.h>
#include <wrl/client.h>
namespace SkyBuild::UI::Widgets {
namespace {
using Microsoft::WRL::ComPtr;
struct Image {
  std::filesystem::path path;
  ComPtr<ID3D11ShaderResourceView> texture;
  std::uint64_t revision{};
  int used{}, brightness{100};
  bool requested{};
  ImGui::ImVec2 dimensions{};
  ImGui::ImVec2 visibleCenter{.5F, .5F};
};
struct Request {
  std::string key;
  std::filesystem::path path;
  std::uint64_t revision{};
  int brightness{};
  UINT targetWidth{}, targetHeight{};
};
struct Pixels {
  Request request;
  UINT width{}, height{};
  std::vector<std::uint8_t> rgba;
  std::vector<std::uint8_t> dds;
};
std::unordered_map<std::string, Image> images;
std::deque<Request> requests;
std::deque<std::pair<int, ComPtr<ID3D11ShaderResourceView>>> retired;
std::future<Pixels> loading;
std::mutex invalidationMutex;
std::map<std::filesystem::path, bool> invalidations;
int frame = -1;
constexpr std::size_t capacity = 512;
Pixels Decode(Request request) {
  Pixels result;
  result.request = std::move(request);
  const auto thumbnailPath =
      result.request.path.lexically_relative(Root()).generic_string();
  if (ThumbnailGenerator::IsTemporary(thumbnailPath)) {
    if (const auto pixels = ThumbnailGenerator::CachedPixels(thumbnailPath)) {
      result.width = result.height = 512;
      result.rgba = *pixels;
      for (std::size_t i = 0; i + 3 < result.rgba.size(); i += 4)
        std::swap(result.rgba[i], result.rgba[i + 2]);
      ApplyImageBrightness(result.rgba, result.request.brightness * .01F);
    } else
      ThumbnailGenerator::RequestMissing(thumbnailPath);
    return result;
  }
  // Only images actually requested by a visible widget reach the decoder.
  // Check once here, off the render thread, rather than scanning packs/frames.
  if (result.request.path.extension() == L".png") {
    std::error_code error;
    if (!std::filesystem::is_regular_file(result.request.path, error)) {
      const auto relative =
          result.request.path.lexically_relative(Root()).generic_string();
      if (relative.starts_with("thumbnails/") ||
          relative.starts_with(ThumbnailPaths::CacheRoot))
        ThumbnailGenerator::RequestMissing(relative);
      return result;
    }
  }
  if (result.request.path.extension() == L".dds") {
    const auto asset =
        result.request.path
            .lexically_relative(std::filesystem::current_path() / "Data")
            .generic_string();
    if (asset.empty() || asset.starts_with("../") ||
        asset.find("/../") != std::string::npos)
      return result;
    RE::BSResourceNiBinaryStream stream(asset);
    if (!stream.good() || !stream.stream ||
        stream.stream->totalSize > 64 * 1024 * 1024)
      return result;
    result.dds.resize(stream.stream->totalSize);
    if (!result.dds.empty() &&
        !stream.read(result.dds.data(),
                     static_cast<std::uint32_t>(result.dds.size())))
      result.dds.clear();
    return result;
  }

  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct COM {
    bool release;
    ~COM() {
      if (release)
        CoUninitialize();
    }
  } scope{SUCCEEDED(initialized)};
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    return result;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> source;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateDecoderFromFilename(
          result.request.path.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrame(0, &source)) ||
      FAILED(source->GetSize(&result.width, &result.height)) || !result.width ||
      !result.height || result.width > 4096 || result.height > 4096 ||
      FAILED(factory->CreateFormatConverter(&converter)))
    return result;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> premultiplied;
  IWICBitmapSource *bitmap = source.Get();
  const auto targetWidth = result.request.targetWidth;
  const auto targetHeight = result.request.targetHeight;
  float scale = std::min(1.F, 512.F / std::max(result.width, result.height));
  if (targetWidth || targetHeight) {
    scale = targetWidth && targetHeight
                ? std::min(float(targetWidth) / result.width,
                           float(targetHeight) / result.height)
            : targetWidth ? float(targetWidth) / result.width
                          : float(targetHeight) / result.height;
  }
  scale = std::min(scale, 4096.F / std::max(result.width, result.height));
  const UINT width =
      std::max(1U, static_cast<UINT>(std::round(result.width * scale)));
  const UINT height =
      std::max(1U, static_cast<UINT>(std::round(result.height * scale)));
  if (width != result.width || height != result.height) {
    result.width = width;
    result.height = height;
    // Filter premultiplied colors so invisible RGB does not bleed into edges.
    if (FAILED(factory->CreateFormatConverter(&premultiplied)) ||
        FAILED(premultiplied->Initialize(bitmap, GUID_WICPixelFormat32bppPRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)))
      return result;
    bitmap = premultiplied.Get();
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap, result.width, result.height,
                                  WICBitmapInterpolationModeFant)))
      return result;
    bitmap = scaler.Get();
  }
  if (FAILED(converter->Initialize(bitmap, GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return result;
  result.rgba.resize(static_cast<std::size_t>(result.width) * result.height *
                     4);
  if (FAILED(converter->CopyPixels(nullptr, result.width * 4,
                                   static_cast<UINT>(result.rgba.size()),
                                   result.rgba.data())))
    result.rgba.clear();
  ApplyImageBrightness(result.rgba, result.request.brightness * .01F);
  return result;
}
void Upload(Pixels result) {
  const auto found = images.find(result.request.key);
  if (found == images.end() ||
      found->second.revision != result.request.revision ||
      (result.rgba.empty() && result.dds.empty()))
    return;
  const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
  auto *device = renderer ? reinterpret_cast<ID3D11Device *>(
                                renderer->GetRuntimeData().forwarder)
                          : nullptr;
  if (!device)
    return;
  if (!result.dds.empty()) {
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(DirectX::CreateDDSTextureFromMemory(device, result.dds.data(),
                                                   result.dds.size(), &resource,
                                                   &view, 256)))
      return;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture)))
      return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.ArraySize != 1)
      return;
    if (found->second.texture)
      retired.emplace_back(frame, std::move(found->second.texture));
    found->second.texture = std::move(view);
    found->second.dimensions = {float(desc.Width), float(desc.Height)};
    found->second.visibleCenter = {.5F, .5F};
    return;
  }

  D3D11_TEXTURE2D_DESC descriptor{};
  descriptor.Width = result.width;
  descriptor.Height = result.height;
  descriptor.MipLevels = 1;
  descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
  descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  descriptor.Usage = D3D11_USAGE_IMMUTABLE;
  descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  D3D11_SUBRESOURCE_DATA data{};
  data.pSysMem = result.rgba.data();
  data.SysMemPitch = result.width * 4;
  if (FAILED(device->CreateTexture2D(&descriptor, &data, &texture)) ||
      FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
    return;
  if (found->second.texture)
    retired.emplace_back(frame, std::move(found->second.texture));
  found->second.texture = std::move(view);
  found->second.dimensions = {float(result.width), float(result.height)};
  // Ignore transparent canvas padding when centering wheel artwork. Compute
  // this once at upload, using the pixels already decoded for the texture.
  UINT left = result.width, top = result.height, right = 0, bottom = 0;
  for (UINT y = 0; y < result.height; ++y)
    for (UINT x = 0; x < result.width; ++x)
      if (result.rgba[(std::size_t(y) * result.width + x) * 4 + 3] > 8) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + 1);
        bottom = std::max(bottom, y + 1);
      }
  found->second.visibleCenter =
      right > left && bottom > top
          ? ImGui::ImVec2{(left + right) * .5F / result.width,
                          (top + bottom) * .5F / result.height}
          : ImGui::ImVec2{.5F, .5F};
}
} // namespace
void Invalidate(const std::filesystem::path &path, bool discard) {
  std::scoped_lock lock(invalidationMutex);
  invalidations[path] |= discard;
}
void BeginFrame() {
  const auto current = ImGui::GetFrameCount();
  if (current == frame)
    return;
  frame = current;
  std::map<std::filesystem::path, bool> pending;
  {
    std::scoped_lock lock(invalidationMutex);
    pending.swap(invalidations);
  }
  for (auto &[key, image] : images)
    if (const auto invalid = pending.find(image.path);
        invalid != pending.end()) {
      if (invalid->second && image.texture)
        retired.emplace_back(frame, std::move(image.texture));
      ++image.revision;
      image.requested = false;
    }
  while (!retired.empty() && retired.front().first < frame - 2)
    retired.pop_front();
  if (loading.valid() &&
      loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      Upload(loading.get());
    } catch (const std::exception &error) {
      spdlog::warn("UI image loading failed: {}", error.what());
    }
  }
  if (!loading.valid())
    while (!requests.empty()) {
      auto request = std::move(requests.front());
      requests.pop_front();
      const auto found = images.find(request.key);
      if (found == images.end() || found->second.revision != request.revision)
        continue;
      if (found->second.used < frame - 2) {
        found->second.requested = false;
        continue;
      }
      loading = std::async(std::launch::async,
                           [request = std::move(request)]() mutable {
                             return Decode(std::move(request));
                           });
      break;
    }
}
ImTextureID Texture(const std::filesystem::path &path, float brightness,
                    ImGui::ImVec2 *dimensions, ImGui::ImVec2 *visibleCenter,
                    ImGui::ImVec2 displaySize) {
  if (visibleCenter)
    *visibleCenter = {.5F, .5F};
  if (dimensions)
    *dimensions = {};
  if (path.empty())
    return nullptr;
  brightness =
      std::isfinite(brightness) ? std::clamp(brightness, 0.F, 2.F) : 1.F;
  const int light =
      std::clamp(static_cast<int>(std::round(brightness * 100)), 0, 200);
  // Size-specific variants live only in the bounded GPU cache; source assets
  // are decoded read-only. Include framebuffer scaling in the pixel target.
  const auto framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  const auto pixels = [](float size, float scale) -> UINT {
    if (!std::isfinite(size) || size <= 0 || !std::isfinite(scale) ||
        scale <= 0)
      return 0;
    return static_cast<UINT>(std::clamp(std::round(size * scale), 1.F, 4096.F));
  };
  const auto width = pixels(displaySize.x, framebufferScale.x);
  const auto height = pixels(displaySize.y, framebufferScale.y);
  const auto key = path.string() + "\n" + std::to_string(light) + ":" +
                   std::to_string(width) + "x" + std::to_string(height);
  auto found = images.find(key);
  if (found == images.end()) {
    if (images.size() >= capacity) {
      const auto oldest = std::min_element(
          images.begin(), images.end(), [](const auto &a, const auto &b) {
            return a.second.used < b.second.used;
          });
      if (oldest->second.used >= frame - 2)
        return nullptr;
      if (oldest->second.texture)
        retired.emplace_back(frame, std::move(oldest->second.texture));
      images.erase(oldest);
    }
    found = images.emplace(key, Image{path, {}, 0, frame, light, false}).first;
  }
  auto &image = found->second;
  image.used = frame;
  if (!image.requested && requests.size() < capacity) {
    image.requested = true;
    requests.push_back({key, path, image.revision, light, width, height});
  }
  if (dimensions && image.texture)
    *dimensions = image.dimensions;
  if (visibleCenter && image.texture)
    *visibleCenter = image.visibleCenter;
  return image.texture.Get();
}
} // namespace SkyBuild::UI::Widgets
