#pragma once
#include "navmesh/publication/NativeFormOwnership.h"
#include "navmesh/native/NativeGraphDraft.h"
#include "navmesh/native/NativeLocationCache.h"
#include "navmesh/publication/NativeOriginalView.h"
namespace SkyBuild::Navmesh {
// Owns both graphs throughout installation and restoration. Prepare allocates
// all replacement buffers first. Commit/Restore require a ready search pause;
// callers must retain this owner after publication, including retired caches.
class NativePublication {
public:
  struct RetainedMemory {
    std::size_t forms{}, infos{}, vertices{}, triangles{}, knownBytes{},
        comparisonBytesReleased{};
    bool sampled{};
  };
  // Captured only while sealing inside the native pause. Reading this report
  // never traverses engine objects from the settings/render thread.
  [[nodiscard]] const RetainedMemory &RetirementMemory() const {
    return retainedMemory;
  }
  struct RetirementAuditContext {
    std::set<const RE::BSNavmesh *> loadedForms, undoTargets;
    std::set<const RE::BSNavmeshInfo *> registeredInfos, cutMemberships,
        derivedWorkInfos;
    std::map<std::uintptr_t, std::set<std::uint32_t>> portalTargets;
  };
  void AppendCutSources(std::vector<const RE::BSNavmesh *> &out) const;
  std::size_t ReclaimRetiredCore(const RetirementAuditContext &context);
  [[nodiscard]] NativeLocationCache::Obsolete EmptyRetiredInfos() const;
  [[nodiscard]] std::size_t ReleasedCoreBytes() const {
    return releasedCoreBytes;
  }
  ~NativePublication();
  [[nodiscard]] static std::unique_ptr<NativePublication>
  Prepare(std::unique_ptr<NativeGraphDraft> graph, std::string &error);
  [[nodiscard]] bool Commit(std::string &error);
  [[nodiscard]] bool Restore(std::string &error);
  [[nodiscard]] bool NeedsMembershipReconciliation() const;
  [[nodiscard]] bool ReconcileMembership(std::string &error);
  // Irreversible after successful restoration. Native allocations remain
  // retained for cached raw references; rollback/validation storage is freed.
  [[nodiscard]] bool SealRetired(std::string &error);
  [[nodiscard]] bool Installed() const { return installed; }
  // Includes borrowed info associations to native-created successors. This
  // identifies a cohort; it does not prove we retain that mesh allocation.
  // Retention also contains authored originals, so it is not a source of
  // generated-mesh provenance.
  [[nodiscard]] bool TracksMesh(const RE::NavMesh *mesh) const;
  // Ready-pause, read-only identity check for a cohort Skyrim kept across load.
  [[nodiscard]] bool StillPublished() const;
  [[nodiscard]] std::size_t MeshCount() const { return graph->MeshCount(); }
  [[nodiscard]] std::string ValidateInstalled() const;
  [[nodiscard]] const std::set<RE::FormID> &DeferredTargets() const {
    return graph->DeferredTargets();
  }
  [[nodiscard]] std::set<RE::TESObjectCELL *> AffectedCells() const;
  // Ready-pause copy; uses retained originals, never published runtime forms.
  [[nodiscard]] bool CopyOriginalView(NativeOriginalView &out,
                                      std::string &error) const;

private:
  using InfoReplacements = std::map<RE::BSNavmeshInfo *, RE::BSNavmeshInfo *>;
  using FormReplacements = std::map<RE::NavMesh *, RE::NavMesh *>;
  using CellDrafts =
      std::map<RE::TESObjectCELL *, std::vector<NativeFormDraft *>>;
  // Prepare owns the native publication/list/registry locks for these phases.
  void PrepareRoutes(const InfoReplacements &replacements);
  void PrepareCellLists(const CellDrafts &cells, const FormReplacements &forms,
                        const InfoReplacements &infos);
  [[nodiscard]] static bool ReadyForPublication();
  std::set<std::uint32_t> releasedCoreKeys;
  std::size_t releasedCoreBytes{};
  std::vector<std::uint32_t>
  RetirementCandidates(const RetirementAuditContext &context) const;
  using InfoMap = decltype(RE::NavMeshInfoMap::infoMap);
  using InfoList = RE::BSTArray<RE::BSNavmeshInfo *>;
  using Forms = RE::BSTArray<RE::BSTSmartPointer<RE::NavMesh>>;
  using Route = RE::BSTArray<const RE::BSNavmeshInfo *>;
  using Routes = decltype(RE::NavMeshInfoMap::allPaths);
  using RouteIndex = decltype(RE::NavMeshInfoMap::infoToIndexMap);
  struct RouteDeleter {
    void operator()(Route *route) const {
      if (route) {
        std::destroy_at(route);
        RE::free(route);
      }
    }
  };
  struct RouteCopy {
    Route *source{};
    Route expected;
    std::unique_ptr<Route, RouteDeleter> replacement;
  };
  struct Cell {
    RE::TESObjectCELL *owner{};
    RE::FormID id{};
    Forms expected, published, replacement;
    RE::NavMeshArray *array{};
  };
  struct List {
    InfoList *array{};
    InfoList expected, published, replacement;
  };
  struct CutList {
    RE::NiPointer<RE::NiRefObject> owner;
    InfoList *array{};
    InfoList expected, published, replacement;
  };
  void Exchange() noexcept;
  [[nodiscard]] bool ReconcileLiveForms(std::string &error);
  [[nodiscard]] bool ReconcileLiveCuts(std::string &error);
  void CaptureCutEvidence(std::string_view event) const noexcept;
  std::map<RE::NavMesh *, RE::NavMeshInfo *> originalBindingTransfers;
  // Membership authorization is independent of whether an info binding needs
  // a deferred write. Extras may be retained originals or verified unbound
  // forms belonging to an older retired replacement.
  std::set<RE::NavMesh *> verifiedDormantOriginals;
  [[nodiscard]] bool OriginalBindingMatches(RE::NavMesh *form,
                                            RE::NavMeshInfo *info) const;
  [[nodiscard]] std::string Preflight(bool restoring) const;
  std::unique_ptr<NativeGraphDraft> graph;
  RE::NavMeshInfoMap *registry{};
  InfoMap replacementMap;
  std::map<RE::FormID, RE::NavMeshInfo *> expectedMap;
  std::map<RE::FormID, RE::NavMeshInfo *> originalChanges, publishedMap;
  Routes expectedRoutes, publishedRoutes, replacementRoutes;
  RouteIndex expectedRouteIndex, publishedRouteIndex, replacementRouteIndex;
  std::vector<RouteCopy> routeCopies;
  std::vector<Cell> cells;
  std::vector<List> lists;
  std::vector<CutList> cutLists;
  Forms supersededForms;
  std::map<RE::NavMesh *, RE::FormID> originalIDs;
  std::shared_ptr<const NativeLocationCache::Obsolete> obsoleteOriginal,
      obsoleteReplacement;
  bool installed{}, exposed{}, republished{}, sealed{};
  RetainedMemory retainedMemory;
};
} // namespace SkyBuild::Navmesh
