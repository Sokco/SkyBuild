#pragma once
#include "navmesh/generation/AuthoredTopology.h"
#include <list>
#include <memory>
#include <mutex>
#include <set>

namespace SkyBuild::Navmesh {
struct AuthoredBasisRecord {
  AuthoredMesh mesh;
  std::string plugin;
  std::uint64_t loadSequence{};
};
struct AuthoredBasisSnapshot {
  std::map<std::uint32_t, std::shared_ptr<const AuthoredBasisRecord>> records;
  std::set<std::uint32_t> missing;
  bool Complete() const { return missing.empty(); }
};
// Process-local, bounded record-load cache. Immutable reader pins survive
// eviction/reloads. This never accepts live cut geometry or serialized runtime
// forms and never substitutes an old record when a newer load was rejected.
class AuthoredBasisStore {
public:
  explicit AuthoredBasisStore(std::size_t budget = 128U * 1024U * 1024U)
      : budget(budget) {}
  static std::size_t Bytes(const AuthoredMesh &mesh) {
    return sizeof(AuthoredBasisRecord) +
           mesh.vertices.capacity() * sizeof(Point) +
           mesh.triangles.capacity() * sizeof(AuthoredTriangle) +
           mesh.portals.capacity() * sizeof(AuthoredPortal) +
           (mesh.coverTriangles.capacity() + mesh.doorTriangles.capacity() +
            mesh.closedDoorTriangles.capacity()) *
               sizeof(std::uint16_t);
  }
  bool Put(AuthoredMesh mesh, std::string plugin) {
    const auto id = mesh.formID;
    const auto bytes = Bytes(mesh) + plugin.capacity();
    const std::scoped_lock lock(mutex);
    Erase(id); // A failed winning override must not expose the prior plugin.
    if (!id || id >= 0xFF000000 || mesh.nativeDependencies || plugin.empty() ||
        bytes > budget)
      return false;
    while (used > budget - bytes && !order.empty())
      Erase(order.front());
    auto record = std::make_shared<const AuthoredBasisRecord>(
        AuthoredBasisRecord{std::move(mesh), std::move(plugin), ++sequence});
    order.push_back(id);
    entries.emplace(id,
                    Entry{std::move(record), bytes, std::prev(order.end())});
    used += bytes;
    return true;
  }
  void Invalidate(std::uint32_t id) {
    const std::scoped_lock lock(mutex);
    Erase(id);
  }
  std::shared_ptr<const AuthoredBasisRecord> Get(std::uint32_t id) {
    const std::scoped_lock lock(mutex);
    const auto found = entries.find(id);
    if (found == entries.end())
      return {};
    order.splice(order.end(), order, found->second.order);
    return found->second.record;
  }
  std::size_t Size() const {
    const std::scoped_lock lock(mutex);
    return entries.size();
  }
  AuthoredBasisSnapshot Snapshot(const std::set<std::uint32_t> &required) {
    const std::scoped_lock lock(mutex);
    AuthoredBasisSnapshot result;
    for (auto id : required) {
      const auto found = entries.find(id);
      if (found == entries.end())
        result.missing.insert(id);
      else {
        result.records.emplace(id, found->second.record);
        order.splice(order.end(), order, found->second.order);
      }
    }
    return result;
  }
  // Whole-cohort version check. Unrelated loads do not invalidate the lease;
  // reload, deletion, rejected override or eviction of a required source does.
  bool Matches(const AuthoredBasisSnapshot &snapshot) const {
    const std::scoped_lock lock(mutex);
    if (!snapshot.Complete())
      return false;
    for (const auto &[id, record] : snapshot.records) {
      const auto found = entries.find(id);
      if (!record || found == entries.end() || found->second.record != record)
        return false;
    }
    return true;
  }
  std::size_t UsedBytes() const {
    const std::scoped_lock lock(mutex);
    return used;
  }

private:
  struct Entry {
    std::shared_ptr<const AuthoredBasisRecord> record;
    std::size_t bytes;
    std::list<std::uint32_t>::iterator order;
  };
  void Erase(std::uint32_t id) {
    const auto found = entries.find(id);
    if (found == entries.end())
      return;
    used -= found->second.bytes;
    order.erase(found->second.order);
    entries.erase(found);
  }
  mutable std::mutex mutex;
  std::map<std::uint32_t, Entry> entries;
  std::list<std::uint32_t> order;
  std::size_t used{}, budget;
  std::uint64_t sequence{};
};
} // namespace SkyBuild::Navmesh
