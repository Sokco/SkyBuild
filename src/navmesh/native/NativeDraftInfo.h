#pragma once
namespace SkyBuild::Navmesh {
class NativeGraphDraft;
// Navigation-detached info for a just-created island form. Never inserted into
// NavMeshInfoMap or a cell's navmesh list; destroyed before its owning form.
class NativeDraftInfo {
public:
  ~NativeDraftInfo();
  [[nodiscard]] static std::unique_ptr<NativeDraftInfo>
  Create(RE::NavMesh &form, std::string &error);
  [[nodiscard]] bool Matches(const RE::NavMesh &form) const;
  [[nodiscard]] std::string ValidationError(const RE::NavMesh &form,
                                            bool published = false) const;
  [[nodiscard]] static bool Registered(RE::FormID id);
  [[nodiscard]] bool Refresh(RE::NavMesh &form, std::string &error);

private:
  friend class NativeGraphDraft;
  friend class NativePublication;
  void PopulateConnected(RE::NavMesh &form);
  void PrepareBounds(RE::NavMesh &form);
  std::optional<std::array<float, 6>> bounds;
  bool connected{};
  RE::BSNavmeshInfo *info{};
};
} // namespace SkyBuild::Navmesh
