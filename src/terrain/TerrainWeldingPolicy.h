#pragma once
#include <utility>

namespace SkyBuild::Terrain::WeldingPolicy {
struct State {
  const void *mesh{};
  bool honorDisabled{};
};
inline thread_local State *current{};
class Scope {
  State *previous;

public:
  explicit Scope(State *state) : previous(std::exchange(current, state)) {}
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  ~Scope() { current = previous; }
};
inline State *For(const void *mesh) {
  return current && current->mesh == mesh ? current : nullptr;
}
// Native WeldingType::None is a supported initializer path, not a fabricated
// "already welded" marker. Restrict the policy to our explicit mesh scope.
inline int EffectiveType(const void *mesh, bool disabled, int requested) {
  const auto state = For(mesh);
  return state && state->honorDisabled && disabled && requested == 0
             ? 6
             : requested;
}
} // namespace SkyBuild::Terrain::WeldingPolicy
