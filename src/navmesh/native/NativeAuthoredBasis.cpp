#include "navmesh/native/NativeAuthoredBasis.h"
#include "navmesh/native/NativeTopologyCapture.h"
#include <MinHook.h>
#include <atomic>
namespace SkyBuild::Navmesh::NativeAuthoredBasis {
namespace {
using Load = bool (*)(RE::NavMesh *, RE::TESFile *);
Load original{};
std::atomic_bool installed{};
std::atomic_uint64_t captured{}, rejected{};
WriterObserver loadObserver;
// No native references survive capture, and no native destructors run at exit.
AuthoredBasisStore &Store() {
  static auto *store = new AuthoredBasisStore;
  return *store;
}
bool LoadObserved(RE::NavMesh *mesh, RE::TESFile *file) {
  loadObserver.Enter();
  struct Leave {
    ~Leave() { loadObserver.Leave(); }
  } leave;
  const bool loaded = original(mesh, file);
  if (!mesh || mesh->GetFormID() >= 0xFF000000)
    return loaded;
  try {
    if (!loaded || !file || mesh->IsDeleted()) {
      Store().Invalidate(mesh->GetFormID());
      return loaded;
    }
    auto basis = CaptureNativeTopology(*mesh, mesh->GetFormID());
    if (Store().Put(std::move(basis), std::string(file->GetFilename())))
      ++captured;
    else
      ++rejected;
  } catch (const std::exception &error) {
    Store().Invalidate(mesh->GetFormID());
    ++rejected;
    spdlog::warn("Authored record capture {:08X} rejected: {}",
                 mesh->GetFormID(), error.what());
  }
  return loaded;
}
} // namespace
bool Initialize() {
  if (installed.load())
    return true;
  // 204173 primary NavMesh vtable slot6 resolves to30327 on inspected1170.
  // 30327 loads winning NVNM through91099 and binds info through91224. Neither
  // calls the obstacle apply/undo routines. MinHook relocates instructions;
  // Address Library resolves entry points, without byte-prefix assumptions.
  const REL::Relocation<std::uintptr_t> table{RE::VTABLE_NavMesh[0]};
  const REL::Relocation<std::uintptr_t> method{REL::ID(30327)};
  if (*reinterpret_cast<const std::uintptr_t *>(table.address() + 6 * 8) !=
      method.address()) {
    spdlog::warn(
        "Authored basis capture unavailable: NavMesh Load slot changed.");
    return false;
  }
  const auto initialized = MH_Initialize();
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
    return false;
  auto *address = reinterpret_cast<void *>(method.address());
  if (MH_CreateHook(address, reinterpret_cast<void *>(&LoadObserved),
                    reinterpret_cast<void **>(&original)) != MH_OK)
    return false;
  if (MH_EnableHook(address) != MH_OK) {
    MH_RemoveHook(address);
    return false;
  }
  installed.store(true);
  spdlog::info("Installed authored navmesh record capture (30327/91099); "
               "bounded immutable geometry, native publication unchanged.");
  return true;
}
std::shared_ptr<const AuthoredBasisRecord> Find(std::uint32_t id) {
  return Store().Get(id);
}
AuthoredBasisSnapshot Snapshot(const std::set<std::uint32_t> &required) {
  return Store().Snapshot(required);
}
bool Matches(const AuthoredBasisSnapshot &snapshot) {
  return Store().Matches(snapshot);
}
AuthoredGenerationBasis
PrepareGeneration(const std::set<std::uint32_t> &required,
                  const std::set<std::uint32_t> &selected,
                  AuthoredBasisSnapshot &lease,
                  const std::map<std::uint32_t, AuthoredMesh> &observed) {
  lease = Snapshot(required);
  if (!Available() || !lease.Complete()) {
    AuthoredGenerationBasis failed;
    failed.error = "Winning authored navigation records unavailable";
    if (!lease.missing.empty())
      failed.error += std::format(": first missing {:08X} ({} missing)",
                                  *lease.missing.begin(), lease.missing.size());
    return failed;
  }
  auto result = PrepareAuthoredGenerationBasis(lease.records, selected);
  if (result.Valid())
    result.error = ApplyAuthoredDoorBindings(result, observed);
  if (result.Valid())
    spdlog::info("Prepared winning-record generation basis: {} records, {} "
                 "selected baselines; runtime doors overlaid separately. "
                 "No live-cut undo used.",
                 result.topology.size(), result.baselines.size());
  return result;
}
WriterObservation LoadObservation() noexcept { return loadObserver.Snapshot(); }
bool Available() noexcept { return installed.load(); }
std::string Status() {
  return std::format("Authored record basis: available {}, {} captures, {} "
                     "rejected, {} retained records, {} bytes",
                     installed.load(), captured.load(), rejected.load(),
                     Store().Size(), Store().UsedBytes());
}
} // namespace SkyBuild::Navmesh::NativeAuthoredBasis
