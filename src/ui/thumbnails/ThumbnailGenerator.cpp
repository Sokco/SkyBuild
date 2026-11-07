#include "ui/thumbnails/ThumbnailGenerator.h"
#include "models/ModelAsset.h"
#include "ui/imgui/UI.h"
#include "ui/imgui/Widgets.h"
#include "ui/thumbnails/ThumbnailRenderer.h"
#include "ui/thumbnails/ThumbnailPaths.h"
#include <deque>
#include <future>
#include <mutex>
#include <unordered_set>
#include <wincodec.h>

namespace SkyBuild::UI::ThumbnailGenerator {
namespace {
constexpr unsigned TEXTURE_SIZE = 512;
struct Job {
  RE::FormID form;
  std::string path;
  float yaw, brightness;
  bool analyze;
};
struct Output {
  std::shared_ptr<MaterialMesh> mesh;
  std::vector<std::uint8_t> pixels;
  std::string error;
};
ProgressTracker progress;
std::deque<Job> pending;
std::optional<Job> active;
std::future<Output> worker;
std::atomic_bool workerFinished{};
std::atomic_bool cancelled{};
std::atomic<std::shared_ptr<const MaterialMesh>> materialMesh;
bool renderingEnabled{}, manual{}, pathsReady{};
std::unordered_map<std::string, std::string> cachedPaths;
std::unordered_map<std::string, std::pair<float, float>> iconAppearance;
std::unordered_map<std::string, RE::FormID> iconForms;
std::unordered_set<std::string> requestedMissing;
struct TemporaryImage {
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
  std::uint64_t used;
};
std::mutex temporaryMutex;
std::unordered_map<std::string, TemporaryImage> temporaryImages;
std::uint64_t temporaryClock{};
// At most 64 MiB of raw 512-square thumbnails, plus in-flight readers.
constexpr std::size_t temporaryCapacity = 64;
void CachePixels(const std::string &path, std::vector<std::uint8_t> pixels) {
  std::scoped_lock lock(temporaryMutex);
  if (!temporaryImages.contains(path) &&
      temporaryImages.size() >= temporaryCapacity) {
    auto oldest = temporaryImages.end();
    for (auto it = temporaryImages.begin(); it != temporaryImages.end(); ++it)
      if (it->first != PICKER_PREVIEW_PATH &&
          (oldest == temporaryImages.end() ||
           it->second.used < oldest->second.used))
        oldest = it;
    if (oldest != temporaryImages.end())
      temporaryImages.erase(oldest);
  }
  temporaryImages[path] = {
      std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels)),
      ++temporaryClock};
}

std::filesystem::path Destination(const Job &job) {
  return std::filesystem::current_path() / "Data/SkyBuild/UI" / job.path;
}
std::filesystem::path Staged(const Job &job) {
  auto path = Destination(job);
  return path += L".pending";
}
ThumbnailRenderer::Bytes ReadResource(std::string path) {
  std::replace(path.begin(), path.end(), '/', '\\');
  RE::BSResourceNiBinaryStream stream(path);
  if (!stream.good() || !stream.stream)
    return {};
  ThumbnailRenderer::Bytes bytes(stream.stream->totalSize);
  if (!bytes.empty() && !stream.read(reinterpret_cast<char *>(bytes.data()),
                                     static_cast<std::uint32_t>(bytes.size())))
    return {};
  return bytes;
}
template <class T> void Release(T *&ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}
bool EncodePNG(const std::filesystem::path &a_path, std::vector<BYTE> pixels) {
  constexpr std::uint32_t outputStride = TEXTURE_SIZE * 4;
  auto temporaryPath = a_path;
  temporaryPath += L".tmp";
  const char *stage = "initialize COM";
  const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitializeCOM = comResult == S_OK || comResult == S_FALSE;
  IWICImagingFactory *factory = nullptr;
  IWICStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  IPropertyBag2 *properties = nullptr;

  HRESULT result = (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
                       ? S_OK
                       : comResult;
  if (SUCCEEDED(result)) {
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  }
  if (SUCCEEDED(result)) {
    result = factory->CreateStream(&stream);
  }
  if (SUCCEEDED(result)) {
    stage = "open output stream";
    result =
        stream->InitializeFromFilename(temporaryPath.c_str(), GENERIC_WRITE);
  }
  if (SUCCEEDED(result)) {
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(result)) {
    result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  }
  if (SUCCEEDED(result)) {
    result = encoder->CreateNewFrame(&frame, &properties);
  }
  if (SUCCEEDED(result)) {
    result = frame->Initialize(properties);
  }
  if (SUCCEEDED(result)) {
    result = frame->SetSize(TEXTURE_SIZE, TEXTURE_SIZE);
  }

  WICPixelFormatGUID requestedFormat = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(result)) {
    result = frame->SetPixelFormat(&requestedFormat);
  }
  if (SUCCEEDED(result) &&
      !IsEqualGUID(requestedFormat, GUID_WICPixelFormat32bppBGRA)) {
    result = E_FAIL;
  }
  if (SUCCEEDED(result)) {
    stage = "encode pixels";
    result =
        frame->WritePixels(TEXTURE_SIZE, outputStride,
                           static_cast<UINT>(pixels.size()), pixels.data());
  }
  if (SUCCEEDED(result)) {
    result = frame->Commit();
  }
  if (SUCCEEDED(result)) {
    stage = "commit PNG";
    result = encoder->Commit();
  }

  Release(properties);
  Release(frame);
  Release(encoder);
  Release(stream);
  Release(factory);
  if (uninitializeCOM) {
    CoUninitialize();
  }
  if (SUCCEEDED(result)) {
    stage = "replace existing PNG";
    if (!MoveFileExW(temporaryPath.c_str(), a_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING))
      result = HRESULT_FROM_WIN32(GetLastError());
  }
  if (FAILED(result)) {
    std::error_code ignored;
    std::filesystem::remove(temporaryPath, ignored);
    spdlog::error("Thumbnail save failed at {} for {}: HRESULT {:08X}.", stage,
                  a_path.string(), static_cast<std::uint32_t>(result));
  }
  return SUCCEEDED(result);
}

void StartNext() {
  while (!active && !pending.empty()) {
    auto job = std::move(pending.front());
    pending.pop_front();
    try {
      auto object = RE::TESForm::LookupByID<RE::TESBoundObject>(job.form);
      auto model = Models::WorldModel(object);
      if (!model || !model->GetModel() || !*model->GetModel())
        throw std::runtime_error("Form has no world model");
      ThumbnailRenderer::Input input;
      input.model = model->GetModel();
      input.yaw = job.yaw;
      input.brightness = job.brightness;
      input.analyze = job.analyze;
      input.resource =
          job.path.starts_with(std::string(ThumbnailPaths::CacheRoot) +
                               "resources/") ||
          job.path.starts_with("thumbnails/.resource-search/");
      if (auto swaps = model->GetAsModelTextureSwap();
          swaps && swaps->alternateTextures) {
        for (unsigned i = 0; i < swaps->numAlternateTextures; ++i) {
          const auto &s = swaps->alternateTextures[i];
          if (!s.textureSet)
            continue;
          const auto diffuse =
              s.textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
          input.swaps.push_back(
              {s.index3D, s.name3D.c_str(), diffuse ? diffuse : ""});
        }
      }
      cancelled = false;
      workerFinished = false;
      progress.SetStatus(std::format(
          "{} {}...", job.analyze ? "Reading materials for" : "Rendering",
          std::filesystem::path(input.model).stem().string()));
      const auto generation = progress.Get().generation;
      worker = std::async(std::launch::async, [job, input = std::move(input),
                                               generation] {
        Output output;
        try {
          if (job.analyze) {
            auto mesh = ThumbnailRenderer::ReadMaterials(input, ReadResource,
                                                         cancelled);
            output.mesh = std::make_shared<MaterialMesh>(
                MaterialMesh{generation, job.form, std::move(mesh), {}});
          } else {
            auto result =
                ThumbnailRenderer::Render(input, ReadResource, cancelled);
            if (!cancelled) {
              if (IsTemporary(job.path)) {
                output.pixels = std::move(result.pixels);
              } else {
                auto path = Staged(job);
                std::filesystem::create_directories(path.parent_path());
                if (!EncodePNG(path, std::move(result.pixels)))
                  throw std::runtime_error("Could not encode thumbnail");
              }
            }
          }
        } catch (const std::exception &e) {
          output.error = e.what();
        }
        workerFinished.store(true);
        SKSE::GetTaskInterface()->AddTask([] { Update(); });
        return output;
      });
      active = std::move(job);
    } catch (const std::exception &e) {
      spdlog::warn("Thumbnail {:08X}: {}", job.form, e.what());
      progress.Finish(false);
    }
  }
  if (!active && pending.empty())
    manual = false;
}

bool Enqueue(Job job) {
  if ((active && active->path == job.path) ||
      std::ranges::any_of(pending,
                          [&](auto &other) { return other.path == job.path; }))
    return false;
  pending.push_back(std::move(job));
  progress.Add(1);
  return true;
}
} // namespace
bool IsTemporary(std::string_view path) {
  return path.starts_with("thumbnails/.hover/") ||
         path.starts_with("thumbnails/.resource-search/") ||
         path == PICKER_PREVIEW_PATH;
}
std::shared_ptr<const std::vector<std::uint8_t>>
CachedPixels(std::string_view path) {
  std::scoped_lock lock(temporaryMutex);
  const auto found = temporaryImages.find(std::string(path));
  if (found == temporaryImages.end())
    return {};
  found->second.used = ++temporaryClock;
  return found->second.pixels;
}
bool HasThumbnail(std::string_view path) {
  if (IsTemporary(path))
    return bool(CachedPixels(path));
  std::error_code error;
  return std::filesystem::is_regular_file(
      std::filesystem::current_path() / "Data/SkyBuild/UI" / path, error);
}
bool SaveTemporary(std::string_view source,
                   const std::filesystem::path &destination) {
  const auto pixels = CachedPixels(source);
  if (!pixels)
    return false;
  std::filesystem::create_directories(destination.parent_path());
  return EncodePNG(destination, *pixels);
}
std::optional<std::string> GetRelativePath(RE::FormID a_formID,
                                           std::string_view pack) {
  // Index stable paths and placement types; reset when the editor reloads
  // packs.
  if (!pathsReady) {
    for (const auto &placeable : PlacementPreview::GetPlaceables()) {
      auto stem = std::filesystem::path{placeable.pluginName}.stem().string();
      for (auto &character : stem)
        if (!std::isalnum(static_cast<unsigned char>(character)) &&
            character != '-' && character != '_')
          character = '_';
      if (!stem.empty()) {
        const auto path =
            ThumbnailPaths::Relative(placeable.packName, placeable.pluginName,
                                     placeable.localFormID, placeable.iconYaw,
                                     placeable.iconLight)
                .generic_string();
        cachedPaths.try_emplace(std::format("{:08X}", placeable.formID), path);
        cachedPaths.try_emplace(
            std::format("{}:{:08X}", placeable.packName, placeable.formID),
            path);

        iconAppearance.try_emplace(path, placeable.iconYaw,
                                   placeable.iconLight);
        iconForms.try_emplace(path, placeable.formID);
      }
    }
    pathsReady = true;
  }
  const auto found =
      cachedPaths.find(pack.empty() ? std::format("{:08X}", a_formID)
                                    : std::format("{}:{:08X}", pack, a_formID));
  return found != cachedPaths.end() ? std::optional{found->second}
                                    : std::nullopt;
}
std::shared_ptr<const MaterialMesh> TakeMaterialMesh() {
  return materialMesh.exchange(nullptr);
}
void ResetPathCache() {
  cachedPaths.clear();
  iconAppearance.clear();
  // Retain temporary identities so an evicted image can be requested again.
  std::erase_if(iconForms,
                [](const auto &entry) { return !IsTemporary(entry.first); });
  requestedMissing.clear();
  pathsReady = false;
}
void QueueEditorIcon(RE::FormID form, const std::string &path,
                     PlacementPreview::PlaceableType, float yaw, float light) {
  if (active || !pending.empty())
    return;
  manual = true;
  RegisterAppearance(form, path, yaw, light);
  Enqueue({form, path, yaw, std::clamp(light, .5F, 2.F), false});
  StartNext();
}
void RegisterAppearance(RE::FormID form, const std::string &path, float yaw,
                        float light) {
  iconForms[path] = form;
  iconAppearance[path] = {yaw, light};
}
void QueueMaterialAnalysis(RE::FormID form) {
  if (active || !pending.empty())
    return;
  manual = true;
  materialMesh.store(nullptr);
  Enqueue({form, {}, 0.F, 1.F, true});
  StartNext();
}
void QueueRegeneration(const std::vector<IconRequest> &forms) {
  if (forms.empty() || cancelled.load() && active)
    return;
  manual = true;
  for (const auto &[form, path] : forms) {
    const auto installedPath = GetRelativePath(form);
    auto it = iconAppearance.find(path);
    // Search/hover thumbnails have temporary paths, but can still represent
    // an installed item. Prefer the exact pack, then its installed defaults.
    if (it == iconAppearance.end() && installedPath)
      it = iconAppearance.find(*installedPath);
    const bool resource =
        path.starts_with(std::string(ThumbnailPaths::CacheRoot) +
                         "resources/") ||
        path.starts_with("thumbnails/.resource-search/");
    const auto [yaw, light] = resource || it == iconAppearance.end()
                                  ? std::pair{0.F, 1.F}
                                  : it->second;
    iconForms[path] = form;
    Enqueue({form, path, yaw, light, false});
  }
  StartNext();
}
void QueueRegeneration(const std::vector<RE::FormID> &forms) {
  std::vector<IconRequest> jobs;
  for (auto form : forms)
    if (auto path = GetRelativePath(form))
      jobs.emplace_back(form, *path);
  QueueRegeneration(jobs);
}
void RequestMissing(std::string path) {
  SKSE::GetTaskInterface()->AddTask([path = std::move(path)] {
    (void)GetRelativePath(0); // Populate the pack-specific index if needed.
    const auto found = iconForms.find(path);
    if (found == iconForms.end() || (active && cancelled.load()) ||
        !requestedMissing.insert(path).second)
      return;
    QueueRegeneration(std::vector<IconRequest>{{found->second, path}});
  });
}
bool Queue(RE::FormID form) {
  if (!renderingEnabled || (active && cancelled.load()))
    return false;
  const auto path = GetRelativePath(form);
  if (!path)
    return false;
  const auto [yaw, light] = iconAppearance.at(*path);
  if (!Enqueue({form, *path, yaw, light, false}))
    return false;
  StartNext();
  return true;
}
Progress GetProgress() { return progress.Get(); }
void SetProgressStatus(std::string_view status) { progress.SetStatus(status); }
void Cancel() {
  if (!progress.Get().running)
    return;
  progress.RequestCancel();
  cancelled = true;
  pending.clear();
  if (!active) {
    manual = false;
    progress.FinishCancel();
  }
}
void SetRenderingEnabled(bool enabled) {
  renderingEnabled = enabled;
  if (!enabled && !manual)
    Cancel();
}
// Publish completed work before advancing the queue; discard cancelled results.
void Update() {
  if (!active || !workerFinished.load())
    return;
  const auto job = std::move(*active);
  active.reset();
  auto result = worker.get();
  requestedMissing.erase(job.path);
  if (job.analyze) {
    const bool success = !cancelled && result.error.empty() && result.mesh;
    if (!cancelled) {
      if (!result.mesh)
        result.mesh = std::make_shared<MaterialMesh>(MaterialMesh{
            progress.Get().generation, job.form, {}, result.error});
      materialMesh.store(std::move(result.mesh));
    }
    progress.Finish(success);
    if (cancelled)
      progress.FinishCancel();
    StartNext();
    return;
  }
  if (IsTemporary(job.path)) {
    const bool success =
        !cancelled && result.error.empty() && !result.pixels.empty();
    if (success) {
      CachePixels(job.path, std::move(result.pixels));
      Widgets::Invalidate(Destination(job), true);
    }
    progress.Finish(success);
    if (cancelled)
      progress.FinishCancel();
    StartNext();
    return;
  }
  auto file = Destination(job), staged = Staged(job);
  bool success = false;
  if (!cancelled) {
    success = result.error.empty() && MoveFileExW(staged.c_str(), file.c_str(),
                                                  MOVEFILE_REPLACE_EXISTING);
    if (success) {
      if (job.path.starts_with(std::string(ThumbnailPaths::CacheRoot) +
                               "resources/"))
        Widgets::Invalidate(file, true);
      else if (job.path != PICKER_PREVIEW_PATH &&
               job.path != "thumbnails/.shell-preview.png")
        SetThumbnailPreview(job.form, job.path);
    } else
      spdlog::warn("Thumbnail {:08X} ({}): {}", job.form, job.path,
                   result.error.empty() ? "Could not publish PNG"
                                        : result.error);
  }
  if (!success) {
    std::error_code ignored;
    std::filesystem::remove(staged, ignored);
  }
  progress.Finish(success);
  if (cancelled)
    progress.FinishCancel();
  StartNext();
}
} // namespace SkyBuild::UI::ThumbnailGenerator
