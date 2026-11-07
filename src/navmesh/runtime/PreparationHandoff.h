#pragma once
#include <optional>
#include <utility>
namespace SkyBuild::Navmesh {
// Tracks a request across a native pause that may complete on a later frame
// and the owned worker that follows it. No engine state or worker ownership.
template <class Action> class PreparationHandoff {
public:
  void Begin(Action action) { executing = action; }
  void CompletePause(bool waiting, bool preparationPending) {
    if (waiting)
      return;
    if (preparationPending && executing)
      preparing = executing;
    executing.reset();
  }
  std::optional<Action> TakeReady(bool pending, bool ready) {
    if (!pending)
      preparing.reset();
    if (!ready)
      return std::nullopt;
    return std::exchange(preparing, std::nullopt);
  }
  void Cancel() {
    executing.reset();
    preparing.reset();
  }

private:
  std::optional<Action> executing, preparing;
};
} // namespace SkyBuild::Navmesh
