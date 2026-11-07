#pragma once
#include "navmesh/generation/MeshBuilder.h"
#include "navmesh/publication/NativeFormOwnership.h"
namespace SkyBuild::Navmesh {
class NativeGeometry;
class NativeDraftInfo;
class NativeGraphDraft;
class NativePublication;
// Game-thread owned native form. The engine factory assigns a runtime ID and
// enters the general TESForm lookup, but this draft is never added to a cell or
// navigation registry. Its native destructor removes the general lookup entry.
class NativeFormDraft {
public:
  ~NativeFormDraft();
  static std::unique_ptr<NativeFormDraft>
  Create(const Mesh &mesh, const RE::BSTSmartPointer<RE::BSPathingCell> &parent,
         std::string &error);
  [[nodiscard]] RE::FormID ID() const { return runtimeID; }
  [[nodiscard]] bool Matches() const;
  [[nodiscard]] std::string ValidationError(bool published = false) const;

private:
  friend class NativeGraphDraft;
  friend class NativePublication;
  struct FormDeleter {
    void operator()(RE::NavMesh *value) const;
  };
  std::unique_ptr<RE::NavMesh, FormDeleter> form;
  RE::FormID runtimeID{};
  std::unique_ptr<NativeGeometry> expected;
  // Destroy info first: its native destructor clears the form's backlink.
  std::unique_ptr<NativeDraftInfo> navigationInfo;
};
namespace NativeFormPreparation {
[[nodiscard]] std::unique_ptr<NativeGraphDraft> TakeGraphForReplacement();
[[nodiscard]] bool Prepare();
[[nodiscard]] bool Pending();
[[nodiscard]] bool PendingReady();
void Clear();
void Finish();
[[nodiscard]] std::string Status();
} // namespace NativeFormPreparation
} // namespace SkyBuild::Navmesh
