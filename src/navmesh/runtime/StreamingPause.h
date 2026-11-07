#pragma once
#include "navmesh/runtime/PauseProbe.h"
namespace SkyBuild::Navmesh {
// Synchronous cell teardown cannot yield to the game task queue. Pump the
// native coordinator under our owned request, with no navigation locks held.
template <class Pump, class Clock, class Yield, class Action>
bool WithStreamingPause(SearchPauseAPI api, Pump pump, Clock clock, Yield yield,
                        Action action) {
  if (!api.Valid())
    return false;
  api.request(api.instance);
  struct Release {
    SearchPauseAPI api;
    ~Release() { api.release(api.instance); }
  } release{api};
  const auto started = clock();
  while (!api.ready(api.instance)) {
    if (clock() - started >= 2000)
      return false;
    pump();
    if (!api.ready(api.instance))
      yield();
  }
  return action();
}
} // namespace SkyBuild::Navmesh
