#pragma once
#include "building/materials/ShellVoxelizer.h"
#include <memory>
#include <filesystem>

#include "ui/thumbnails/ThumbnailProgress.h"
#include "building/PlacementPreview.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SkyBuild::UI::ThumbnailGenerator {
[[nodiscard]] std::optional<std::string>
GetRelativePath(RE::FormID a_formID, std::string_view pack = {});
bool Queue(RE::FormID a_formID);
void ResetPathCache();
// Safe from the image decoder thread; resolves pack settings on the game
// thread.
void RequestMissing(std::string relativePath);
bool IsTemporary(std::string_view path);
// Thread-safe pixels for temporary thumbnails; BGRA, 512 x 512.
std::shared_ptr<const std::vector<std::uint8_t>>
CachedPixels(std::string_view path);
bool HasThumbnail(std::string_view path);
bool SaveTemporary(std::string_view source,
                   const std::filesystem::path &destination);
void RegisterAppearance(RE::FormID form, const std::string &path, float yaw,
                        float light);
void QueueEditorIcon(RE::FormID form, const std::string &relativePath,
                     PlacementPreview::PlaceableType type, float yaw = 0,
                     float light = 1);
void QueueMaterialAnalysis(RE::FormID form);
struct MaterialMesh {
  std::uint64_t generation{};
  RE::FormID form{};
  MaterialShell::Mesh mesh;
  std::string error;
};
std::shared_ptr<const MaterialMesh> TakeMaterialMesh();
Progress GetProgress();
void Cancel();
void SetProgressStatus(std::string_view status);
using IconRequest = std::pair<RE::FormID, std::string>;
void QueueRegeneration(const std::vector<RE::FormID> &forms);
void QueueRegeneration(const std::vector<IconRequest> &forms);
void SetRenderingEnabled(bool a_enabled);
void Update();
} // namespace SkyBuild::UI::ThumbnailGenerator
