#pragma once
#include "building/ConstrainedPlacement.h"

namespace SkyBuild::ConstrainedPlacement {
// All positions are reference origins with the same authored model transform.
// One sweep from the camera ray origin, stopping at the first blocking hit.
template <class V, class Query, class Cast>
std::optional<V> ResolveCameraSweep(V start, V target, Query query, Cast cast) {
  const auto clear = [&](V at) {
    for (const auto &contact : query(at))
      if (!std::isfinite(contact.depth) || contact.depth > .05F)
        return false;
    return true;
  };
  if (!clear(start))
    return {};
  const auto sweep = [&](V from, V to) -> std::optional<V> {
    const auto travel = to - from;
    const float length = std::sqrt(Dot(travel, travel));
    if (!std::isfinite(length))
      return {};
    if (length < .00001F)
      return from;
    const auto fraction = cast(from, to);
    if (!fraction || !std::isfinite(*fraction) || *fraction < 0 ||
        *fraction > 1)
      return {};
    const float safe =
        *fraction < 1 ? std::max(0.F, *fraction - .05F / length) : 1.F;
    const auto at = from + travel * safe;
    return clear(at) ? std::optional<V>{at} : std::nullopt;
  };
  return sweep(start, target);
}
} // namespace SkyBuild::ConstrainedPlacement
