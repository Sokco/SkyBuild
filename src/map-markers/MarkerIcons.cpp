#include "map-markers/MarkerIcons.h"
#include "map-markers/CoMAPConfig.h"
#include "ui/imgui/NativeUI.h"
#include <atomic>
#include <charconv>
#include <d3d11.h>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <wrl/client.h>

namespace SkyBuild::MapMarkers::Icons {
namespace {
// Read-only ABI from Exit-9B/MapMarkerFramework's MIT-licensed public API v1.
// Copyright (c) Parapets. See THIRD_PARTY.md in this directory.
struct MarkerInfo {
  const char *source, *symbol, *undiscovered;
  std::int32_t vanilla;
  float scale;
  bool hidden;
};
struct Interface {
  std::uint32_t version;
  std::uint16_t (*vanilla)(RE::TESObjectREFR *);
  const char *(*name)(RE::TESObjectREFR *);
  void (*info)(RE::TESObjectREFR *, MarkerInfo *);
  void (*localInfo)(RE::BGSLocation *, MarkerInfo *);
};
static_assert(sizeof(Interface) == 0x28 && sizeof(MarkerInfo) == 0x28);
const Interface *comap{};
std::atomic<std::shared_ptr<const std::vector<Icon>>> catalog;
std::atomic_uint64_t catalogRevision{};
std::string ReadResource(const std::string &path) {
  RE::BSResourceNiBinaryStream stream(path.c_str());
  if (!stream.good() || !stream.stream ||
      stream.stream->totalSize > MaxRecordBytes)
    return {};
  std::string data(stream.stream->totalSize, '\0');
  if (!stream.read(data.data(), static_cast<std::uint32_t>(data.size())))
    return {};
  return data;
}
std::string Label(std::string name) {
  if (name.ends_with("Marker"))
    name.resize(name.size() - 6);
  std::string label;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const auto c = static_cast<unsigned char>(name[i]);
    if (i && std::isupper(c) &&
        std::islower(static_cast<unsigned char>(name[i - 1])))
      label += ' ';
    label += c == '_' ? ' ' : static_cast<char>(c);
  }
  return label;
}
RE::GFxSpriteDef *Compass(RE::GFxMovieDef *movie) {
  const auto resource = movie ? movie->GetResource("Compass Marker") : nullptr;
  return resource && resource->GetResourceType() ==
                         RE::GFxResource::ResourceType::kSpriteDef
             ? static_cast<RE::GFxSpriteDef *>(resource)
             : nullptr;
}

// Each source has one isolated movie and one clip. No original HUD stage
// scripts run, and no game-owned movie is moved, stopped, or edited for the
// picker.
struct Preview {
  RE::GPtr<RE::GFxMovieView> movie;
  RE::GFxValue root, clip;
  std::string symbol;
  int locationFrame{};
};
std::unordered_map<std::string, std::unique_ptr<Preview>> previews;
struct DrawIcon {
  Icon icon;
  ImVec2 pos;
  float size;
};
std::vector<std::unique_ptr<DrawIcon>> draws;
int drawFrame{-1};
std::uint64_t previewRevision{};

Preview *GetPreview(const Icon &icon) {
  const auto key = icon.source;
  if (const auto found = previews.find(key); found != previews.end())
    return found->second.get();
  const auto unavailable = [&]() -> Preview * {
    spdlog::warn("Map icon preview source is unavailable: {}",
                 key.empty() ? "HUD compass" : key);
    previews.emplace(key, nullptr);
    return nullptr;
  };
  auto preview = std::make_unique<Preview>();
  RE::GPtr<RE::GFxMovieDef> source;
  if (key.empty()) {
    const auto ui = RE::UI::GetSingleton();
    const auto hud = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
    if (hud && hud->uiMovie)
      source.reset(hud->uiMovie->GetMovieDef());
    const auto compass = Compass(source.get());
    if (!compass || !compass->GetLabeledFrame("LocationMarkers",
                                              preview->locationFrame, false))
      return unavailable();
  } else {
    const auto manager = RE::BSScaleformManager::GetSingleton();
    if (!manager || !manager->loader)
      return unavailable();
    source.reset(manager->loader->CreateMovie(key.c_str()));
    if (source)
      source->Release(); // CreateMovie returns an owned reference.
  }
  if (!source)
    return unavailable();
  auto instance = source->CreateInstance(false);
  if (!instance)
    return unavailable();
  preview->movie.reset(instance);
  instance->Release();
  preview->movie->SetBackgroundAlpha(0);
  preview->movie->SetViewScaleMode(RE::GFxMovieView::ScaleModeType::kNoScale);
  preview->movie->SetViewAlignment(RE::GFxMovieView::AlignType::kTopLeft);
  preview->movie->SetPlayState(RE::GFxMovie::PlayState::kStopped);
  if (!preview->movie->GetVariable(&preview->root, "_root") ||
      !preview->root.IsDisplayObject())
    return unavailable();
  auto result = preview.get();
  previews.emplace(key, std::move(preview));
  return result;
}
void RenderThumbnail(const ImDrawList *, const ImDrawCmd *command) {
  const auto &request =
      *static_cast<const DrawIcon *>(command->UserCallbackData);
  auto *preview = GetPreview(request.icon);
  if (!preview)
    return;
  const auto symbol =
      request.icon.custom ? request.icon.symbol : "Compass Marker";
  if (preview->symbol != symbol) {
    if (preview->clip.IsDisplayObject())
      preview->clip.Invoke("removeMovieClip", nullptr);
    if (!preview->root.AttachMovie(&preview->clip, symbol.c_str(),
                                   "SkyBuildIcon", 1))
      return;
    preview->symbol = symbol;
  }
  RE::GFxValue::DisplayInfo identity;
  identity.Set(0, 0, 0, 100, 100, 100, true);
  preview->clip.SetDisplayInfo(identity);
  // Pass a numeric AS frame, rather than treating its number as a frame label.
  const RE::GFxValue frame(static_cast<double>(
      request.icon.custom ? 1 : preview->locationFrame + request.icon.type));
  if (!preview->clip.Invoke("gotoAndStop", nullptr, &frame, 1))
    return;
  RE::GFxValue bounds;
  if (!preview->clip.Invoke("getBounds", &bounds, &preview->root, 1))
    return;
  RE::GFxValue x0, y0, x1, y1;
  if (!bounds.GetMember("xMin", &x0) || !bounds.GetMember("yMin", &y0) ||
      !bounds.GetMember("xMax", &x1) || !bounds.GetMember("yMax", &y1))
    return;
  const double width = x1.GetNumber() - x0.GetNumber();
  const double height = y1.GetNumber() - y0.GetNumber();
  if (width <= 0 || height <= 0)
    return;
  const double scale = request.size / std::max(width, height);
  identity.Set(request.pos.x + (request.size - width * scale) * .5 -
                   x0.GetNumber() * scale,
               request.pos.y + (request.size - height * scale) * .5 -
                   y0.GetNumber() * scale,
               0, scale * 100, scale * 100, 100, true);
  preview->clip.SetDisplayInfo(identity);
  const auto screen = ImGui::GetIO().DisplaySize;
  const auto &clip = command->ClipRect;
  RE::GViewport viewport(
      static_cast<int>(screen.x), static_cast<int>(screen.y), 0, 0,
      static_cast<int>(screen.x), static_cast<int>(screen.y),
      static_cast<int>(clip.x), static_cast<int>(clip.y),
      static_cast<int>(clip.z - clip.x), static_cast<int>(clip.w - clip.y),
      RE::GViewport::kUseScissorRect);
  preview->movie->SetViewport(viewport);
  auto &renderer = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData();
  auto *context = reinterpret_cast<ID3D11DeviceContext *>(renderer.context);
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
  context->OMGetRenderTargets(1, &target, &depth);
  preview->movie->Display();
  auto *original = target.Get();
  context->OMSetRenderTargets(1, &original, depth.Get());
}
} // namespace
void Listen() {
  SKSE::GetMessagingInterface()->RegisterListener(
      "MapMarkerFramework", [](SKSE::MessagingInterface::Message *message) {
        if (message && message->type == 0 && message->data &&
            message->dataLen >= sizeof(Interface)) {
          const auto api = static_cast<const Interface *>(message->data);
          if (api->version == 1)
            comap = api;
        }
      });
}
void Refresh() {
  auto result = std::make_shared<std::vector<Icon>>();
  for (std::size_t i = 0; i < VanillaNames.size(); ++i)
    result->push_back({std::to_string(i + 1),
                       VanillaNames[i],
                       {},
                       {},
                       static_cast<std::uint16_t>(i + 1),
                       false});
  if (comap) {
    std::map<std::string, std::pair<std::size_t, Icon>> custom;
    std::vector<std::pair<RE::TESObjectREFR *, std::string>> assignments;
    std::size_t count{};
    const auto handler = RE::TESDataHandler::GetSingleton();
    if (handler)
      for (const auto file : handler->files) {
        if (!file)
          continue;
        const auto configPath =
            std::filesystem::path(file->fileName).replace_extension("json");
        const auto text = ReadResource("MapMarkers/" + configPath.string());
        if (text.empty())
          continue;
        try {
          const auto config = ParseCoMAPConfig(text);
          if (config.contains("iconDefinitions"))
            for (const auto &def : config["iconDefinitions"]) {
              if (!def.is_object() || !def.contains("source") ||
                  !def["source"].is_object())
                continue;
              const auto name = def.value("name", "");
              if (name.empty())
                continue;
              const auto &source = def["source"];
              auto path = source.value("path", "");
              if (path.empty()) {
                char resource[MAX_PATH]{};
                GetPrivateProfileStringA(
                    "Resources", "sResourceFile", "", resource, MAX_PATH,
                    ".\\Data\\SKSE\\Plugins\\MapMarkerFramework.ini");
                if (!resource[0])
                  GetPrivateProfileStringA("Resources", "sResourceFile", "",
                                           resource, MAX_PATH,
                                           ".\\Data\\SKSE\\Plugins\\CoMAP.ini");
                path = resource;
              }
              std::replace(path.begin(), path.end(), '\\', '/');
              const auto exports =
                  source.value("exportNames", nlohmann::json::array());
              Icon icon{name,
                        Label(name),
                        "MapMarkers/" + path,
                        exports.is_array() && exports.size() == 2
                            ? exports[0].get<std::string>()
                            : "",
                        0,
                        true};
              custom[name] = {count++, std::move(icon)};
            }
          if (config.contains("mapMarkers"))
            for (const auto &assignment : config["mapMarkers"]) {
              const auto id = assignment.value("refID", "");
              const auto separator = id.find('|');
              if (separator == std::string::npos)
                continue;
              std::uint32_t local{};
              const auto digits = std::string_view(id).substr(separator + 1);
              if (std::from_chars(digits.data(), digits.data() + digits.size(),
                                  local, 16)
                      .ec != std::errc{})
                continue;
              if (auto ref = handler->LookupForm<RE::TESObjectREFR>(
                      local, id.substr(0, separator)))
                assignments.emplace_back(ref, assignment.value("iconName", ""));
            }
        } catch (const std::exception &error) {
          spdlog::warn("Map marker icon catalog could not read {}: {}",
                       configPath.string(), error.what());
        }
      }
    std::size_t base{};
    // CoMAP appends its imports to the installed compass timeline. Inspect the
    // actual patched layout, including mods that insert reserved/empty frames.
    const auto ui = RE::UI::GetSingleton();
    const auto hud = ui ? ui->GetMenu<RE::HUDMenu>() : nullptr;
    const auto sprite =
        hud && hud->uiMovie ? Compass(hud->uiMovie->GetMovieDef()) : nullptr;
    std::int32_t first{}, last{};
    if (sprite && sprite->GetLabeledFrame("LocationMarkers", first, false) &&
        sprite->GetLabeledFrame("UndiscoveredMarkers", last, false) &&
        last > first) {
      auto end = last - 1;
      while (end > first + 66 && end < sprite->frameCount &&
             !sprite->frames[end].data)
        --end;
      if (end - first + 1 >= count + 67)
        base = end - first + 1 - count;
    }
    // Public API assignments provide an independent index check and also work
    // when Infinity UI replaced the compass's original resource definition.
    for (const auto &[ref, name] : assignments) {
      const auto found = custom.find(name);
      if (found == custom.end() || !comap->name)
        continue;
      const auto assigned = comap->name(ref);
      if (!assigned || name != assigned)
        continue;
      const auto extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
      if (extra && extra->mapData) {
        const auto type = extra->mapData->type.underlying();
        if (type >= 67 + found->second.first) {
          base = type - found->second.first;
          break;
        }
      }
    }
    if (base && base + count <= UINT16_MAX)
      for (auto &[key, entry] : custom) {
        entry.second.type = static_cast<std::uint16_t>(base + entry.first);
        if (!entry.second.symbol.empty())
          result->push_back(std::move(entry.second));
      }
  }
  catalog.store(std::move(result));
  ++catalogRevision;
}
std::shared_ptr<const std::vector<Icon>> Catalog() { return catalog.load(); }
std::uint16_t Resolve(const Marker &marker) {
  if (!marker.customIcon.empty())
    if (const auto icons = Catalog())
      for (const auto &icon : *icons)
        if (icon.custom && icon.key == marker.customIcon)
          return icon.type;
  return marker.vanillaIcon;
}
void Thumbnail(const Icon &icon, float x, float y, float size) {
  const auto currentRevision = catalogRevision.load();
  if (previewRevision != currentRevision) {
    previews.clear();
    previewRevision = currentRevision;
  }
  if (drawFrame != ImGui::GetFrameCount()) {
    draws.clear();
    drawFrame = ImGui::GetFrameCount();
  }
  auto request = std::make_unique<DrawIcon>(DrawIcon{icon, {x, y}, size});
  auto draw = ImGui::GetWindowDrawList();
  if (const auto source = previews.find(icon.source);
      source != previews.end() && !source->second) {
    const auto extent = ImGui::CalcTextSize("?");
    draw->AddText({x + (size - extent.x) * .5F, y + (size - extent.y) * .5F},
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");
    return;
  }
  // Respect the tile's clip rectangle and restore ImGui's bindings immediately
  // after Scaleform draws. Neither renderer shares a mutable movie instance.
  draw->AddCallback(RenderThumbnail, request.get());
  draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  draws.push_back(std::move(request));
}
} // namespace SkyBuild::MapMarkers::Icons
