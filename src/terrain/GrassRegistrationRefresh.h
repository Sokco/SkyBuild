#pragma once
#include <cstdint>
#include <optional>

namespace SkyBuild::Terrain::Grass {
// Installed rendering hooks observe AddGroup's returned ID and capture that
// group's data. A refresh replays this notification for an existing group;
// the native insertion is intercepted so its identity and owners stay intact.
class RegistrationRefresh {
public:
  RegistrationRefresh(const void *shape, std::uint32_t index)
      : shape(shape), index(index), previous(current) {
    current = this;
  }
  ~RegistrationRefresh() { current = previous; }
  RegistrationRefresh(const RegistrationRefresh &) = delete;
  RegistrationRefresh &operator=(const RegistrationRefresh &) = delete;
  static std::optional<std::uint32_t> Reuse(const void *shape) {
    if (!current || current->shape != shape)
      return {};
    current->observed = true;
    return current->index;
  }
  bool Observed() const { return observed; }

private:
  const void *shape;
  std::uint32_t index;
  RegistrationRefresh *previous;
  bool observed{};
  inline static thread_local RegistrationRefresh *current{};
};
} // namespace SkyBuild::Terrain::Grass
