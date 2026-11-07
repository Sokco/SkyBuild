#pragma once
#include "navmesh/generation/AuthoredTopology.h"
#include "navmesh/cuts/CutUndoReplay.h"
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
namespace SkyBuild::Navmesh {
struct Mesh;
struct SurfaceOwnership;
struct NativeCutDataSource;
enum class RuntimeStage {
  kAwaitingEngineBackend,
  kIdle,
  kCapturing,
  kBuilding,
  kGenerated,
  kFailed
};
struct RuntimeStatus {
  RuntimeStage stage{RuntimeStage::kAwaitingEngineBackend};
  std::size_t sampled{}, total{}, generatedCells{}, cachedCells{};
  float
      progress{}; // Fraction of generation steps; not elapsed-time prediction.
  std::size_t completedCells{}, commandCells{};
  bool generationSucceeded{};
  bool generating{}; // Explicit command, through native publication.
  bool installedInEngine{};
  std::size_t vertices{}, triangles{};
  std::size_t pendingNativeForms{};
  std::string message;
};
void Initialize();
void SetSceneLoaded(bool loaded);
void Reset();
void ReferenceChanged(RE::TESObjectREFR *reference);
void ReferenceRemoved(RE::FormID id);
void Update();
void AbortGeneration(std::string_view reason);
void NotifyStreamingDetach();
[[nodiscard]] std::map<std::uint32_t, AuthoredMesh> CopyValidatedSessionBasis();
void RequestGeneration();
void RequestCachedApply();
void SuppressCommittedRestoration();
void SaveCache(SKSE::SerializationInterface *serialization);
void LoadCache(SKSE::SerializationInterface *serialization,
               std::uint32_t version, std::uint32_t length);
inline constexpr std::uint32_t CELL_CACHE_RECORD = 0x4843564E; // NVCH
void ConfirmInstalledCache();
[[nodiscard]] bool CopyGeneratedForPreparation(
    Mesh &mesh, SurfaceOwnership &ownership,
    std::map<std::uint32_t, AuthoredMesh> *topology = nullptr,
    NativeCutSnapshot *cuts = nullptr,
    std::vector<NativeCutDataSource> *cutData = nullptr,
    std::vector<RE::FormID> *islandCells = nullptr,
    std::set<RE::FormID> *replacedCells = nullptr);
// Internal ready-pause callback. Captures immutable native baseline data only.
[[nodiscard]] bool CapturePendingBaseline(void *) noexcept;
[[nodiscard]] bool RestoreCommittedNavigation(void *) noexcept;
[[nodiscard]] RuntimeStatus GetStatus();
[[nodiscard]] bool GenerationPresentationActive() noexcept;
} // namespace SkyBuild::Navmesh
