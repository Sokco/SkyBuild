#pragma once
#include "building/ConstrainedPlacement.h"

namespace SkyBuild::ConstrainedPlacement {
// Triangle/compound contacts describe separate local intersections, not one
// convex solid. Opposing normals cannot all be mandatory translation planes.
// Find a clear exterior pose, sweep the whole assembly back toward the desired
// pose, then reduce the displacement with further collision-limited sweeps.
// Every accepted pose is checked with the original, complete overlap query.
template <class V, class Limit, class Query, class Cast>
std::optional<V>
ResolveSurfaceCast(V original, V support, const std::array<V, 3> &axes,
                   Limit maxCorrection, Query query, Cast cast) {
  const auto clear = [&](V at) {
    for (const auto &contact : query(at))
      if (!std::isfinite(contact.depth) || contact.depth > .05F)
        return false;
    return true;
  };
  const auto withinLimit = [&](V at) {
    const auto delta = at - original;
    const float limit = maxCorrection(delta);
    return std::isfinite(limit) && limit >= 0 &&
           Dot(delta, delta) <= limit * limit &&
           Dot(delta, support) >= -.00001F;
  };
  const auto sweep = [&](V from, V to) -> std::optional<V> {
    const auto travel = to - from;
    const float length = std::sqrt(Dot(travel, travel));
    if (length < .00001F)
      return from;
    const auto fraction = cast(from, to);
    if (!fraction || !std::isfinite(*fraction) || *fraction < 0 ||
        *fraction > 1)
      return {};
    // Leave a small physical margin before a swept hit; never extrapolate.
    const float safe =
        *fraction < 1 ? std::max(0.F, *fraction - .05F / length) : 1.F;
    const auto at = from + travel * safe;
    if (!clear(at))
      return {};
    return at;
  };
  std::vector<V> basis{support};
  const auto add = [&](V direction) {
    const float length = std::sqrt(Dot(direction, direction));
    if (!std::isfinite(length) || length < .00001F)
      return;
    direction = direction * (1.F / length);
    for (const auto &old : basis)
      if (std::abs(Dot(old, direction)) > .98F)
        return;
    if (basis.size() < 4)
      basis.push_back(direction);
  };
  // Build a small search basis from the actual obstacle normals. The support
  // axis is separate, so reducing wall clearance does not undo floor clearance.
  for (const auto &contact : query(original))
    if (contact.depth > .05F)
      add(contact.normal - support * Dot(contact.normal, support));
  for (const auto &axis : axes)
    add(axis - support * Dot(axis, support));
  std::optional<V> best;
  const auto consider = [&](V direction) {
    const float length = std::sqrt(Dot(direction, direction));
    if (length < .00001F)
      return;
    direction = direction * (1.F / length);
    const float limit = maxCorrection(direction);
    if (!std::isfinite(limit) || limit <= 0)
      return;
    const auto outside = original + direction * limit;
    if (!clear(outside))
      return;
    auto at = sweep(outside, original);
    if (!at || !withinLimit(*at))
      return;
    // Refine every viable escape before comparing them. The shortest raw ray
    // hit can lead to a worse final placement than another ray whose excess
    // offset is easily removed along an obstacle axis.
    for (unsigned pass = 0; pass < 2; ++pass)
      for (const auto &axis : basis) {
        const auto target = *at + axis * Dot(original - *at, axis);
        if (const auto reduced = sweep(*at, target);
            reduced && withinLimit(*reduced) &&
            Dot(*reduced - original, *reduced - original) <
                Dot(*at - original, *at - original))
          at = *reduced;
      }
    if (at && withinLimit(*at) &&
        (!best || Dot(*at - original, *at - original) <
                      Dot(*best - original, *best - original)))
      best = *at;
  };
  consider(support);
  for (std::size_t i = 1; i < basis.size(); ++i)
    for (float sign : {-1.F, 1.F}) {
      consider(basis[i] * sign);
      consider(basis[i] * sign + support);
    }
  return best;
}
} // namespace SkyBuild::ConstrainedPlacement
