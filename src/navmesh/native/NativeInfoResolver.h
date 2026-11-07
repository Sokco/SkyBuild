#pragma once
#include <map>
namespace SkyBuild::Navmesh {
// Scoped lookup isolation, not registration/publication. Only this thread and
// these FF IDs can see the detached infos; unknown IDs fail closed in scope.
class NativeInfoResolver {
public:
  explicit NativeInfoResolver(
      const std::map<std::uint32_t, RE::BSNavmeshInfo *> &infos,
      bool allowClearedIdentity = false);
  ~NativeInfoResolver();
  NativeInfoResolver(const NativeInfoResolver &) = delete;
  NativeInfoResolver &operator=(const NativeInfoResolver &) = delete;
  [[nodiscard]] std::size_t Hits() const { return hits; }
  [[nodiscard]] std::size_t Misses() const { return misses; }
  [[nodiscard]] std::uint32_t LastMissingID() const { return lastMissingID; }
  [[nodiscard]] bool Contains(std::uint32_t id, RE::BSNavmeshInfo *info) const {
    const auto found = infos.find(id);
    return found != infos.end() && found->second == info;
  }
  // Authoritative native registration, never the thread's private lookup view.
  // Preserves the native lookup's locking and any pre-existing hook chain.
  [[nodiscard]] static RE::BSNavmeshInfo *
  LookupRegistered(RE::BSNavmeshInfoMap *registry, std::uint32_t id);

private:
  static RE::BSNavmeshInfo *Lookup(RE::BSNavmeshInfoMap *, std::uint32_t,
                                   unsigned);
  static RE::BSNavmeshInfo *FixLookup(RE::BSNavmeshInfoMap *, std::uint32_t);
  static RE::BSNavmeshInfo *ExactLookup(RE::BSNavmeshInfoMap *, std::uint32_t);
  RE::BSNavmeshInfoMap *registry{};
  const std::map<std::uint32_t, RE::BSNavmeshInfo *> &infos;
  std::size_t hits{}, misses{};
  std::uint32_t lastMissingID{};
  // Native30339 clears a private form's ID by explicitly looking up zero.
  // Only the isolated identity protocol opts in; cut lookups stay strict.
  bool allowClearedIdentity{};
};
} // namespace SkyBuild::Navmesh
