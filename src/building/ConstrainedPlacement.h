#pragma once
#include <array>
#include <cmath>
#include <optional>
#include <vector>
#include <limits>
#include <algorithm>
namespace SkyBuild::ConstrainedPlacement {
template <class V> float Dot(const V &a, const V &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
// Corrections may leave the support plane, but never move into it. An
// unresolved contact returns no correction, so the caller retains its original
// placement. Full projected box length along the total correction direction.
template <class V>
float DirectionalSize(V direction, V half, const std::array<V, 3> &axes) {
  const float length = std::sqrt(Dot(direction, direction));
  if (length <= .000001F)
    return 0;
  return 2.F *
         (std::abs(Dot(direction, axes[0])) * half.x +
          std::abs(Dot(direction, axes[1])) * half.y +
          std::abs(Dot(direction, axes[2])) * half.z) /
         length;
}
// Minimum-norm point in a two-dimensional intersection of contact half-planes.
// When a new plane excludes the previous optimum, the new optimum is on its
// boundary. Intersect that line with prior planes, then choose its nearest
// point.
template <class V> struct Plane {
  V normal;
  float distance;
};
template <class V>
std::optional<V> ClosestCorrection(const std::vector<Plane<V>> &planes,
                                   V support) {
  V result{};
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const auto &plane = planes[i];
    if (Dot(plane.normal, result) >= plane.distance - .00001F)
      continue;
    const auto base = plane.normal * plane.distance;
    const V along{support.y * plane.normal.z - support.z * plane.normal.y,
                  support.z * plane.normal.x - support.x * plane.normal.z,
                  support.x * plane.normal.y - support.y * plane.normal.x};
    float lower = -std::numeric_limits<float>::infinity();
    float upper = std::numeric_limits<float>::infinity();
    for (std::size_t j = 0; j < i; ++j) {
      const float slope = Dot(planes[j].normal, along);
      const float needed = planes[j].distance - Dot(planes[j].normal, base);
      if (std::abs(slope) < .000001F) {
        if (needed > .00001F)
          return {};
      } else if (slope > 0)
        lower = std::max(lower, needed / slope);
      else
        upper = std::min(upper, needed / slope);
    }
    if (lower > upper)
      return {};
    result = base + along * std::clamp(0.F, lower, upper);
  }
  return result;
}
// Three-dimensional minimum displacement. Each violated plane reduces the
// remaining problem to the two-dimensional solver on that plane's boundary.
template <class V>
std::optional<V> ClosestCorrection(const std::vector<Plane<V>> &planes) {
  V result{};
  for (std::size_t i = 0; i < planes.size(); ++i) {
    const auto &plane = planes[i];
    if (Dot(plane.normal, result) >= plane.distance - .00001F)
      continue;
    const auto base = plane.normal * plane.distance;
    std::vector<Plane<V>> projected;
    projected.reserve(i);
    for (std::size_t j = 0; j < i; ++j) {
      const auto tangent =
          planes[j].normal - plane.normal * Dot(planes[j].normal, plane.normal);
      const float length = std::sqrt(Dot(tangent, tangent));
      const float needed = planes[j].distance - Dot(planes[j].normal, base);
      if (length < .000001F) {
        if (needed > .00001F)
          return {};
      } else
        projected.push_back({tangent * (1.F / length), needed / length});
    }
    const auto correction = ClosestCorrection(projected, plane.normal);
    if (!correction)
      return {};
    result = base + *correction;
  }
  return result;
}
template <class V, class Limit, class Query>
std::optional<V> ResolveClosest(V original, V support, Limit maxCorrection,
                                Query query) {
  V candidate = original;
  // Keep the contact manifold for this solve. A missing contact at the next
  // pose may simply mean our last correction cleared it; forgetting it lets
  // the next minimum-displacement solve undo that correction.
  std::vector<Plane<V>> planes{{support, 0.F}};
  const auto withinLimit = [&](V correction) {
    const float limit = maxCorrection(correction);
    return std::isfinite(limit) && limit >= 0 &&
           Dot(correction, correction) <= limit * limit;
  };
  const auto clear = [&](V at) {
    for (const auto &contact : query(at))
      if (!std::isfinite(contact.depth) || contact.depth > .05F)
        return false;
    return true;
  };
  const auto addPlane = [](std::vector<Plane<V>> &target, V normal,
                           float distance) {
    for (auto &plane : target)
      if (Dot(plane.normal, normal) > .999999F) {
        plane.distance = std::max(plane.distance, distance);
        return;
      }
    target.push_back({normal, distance});
  };
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<Plane<V>> currentPlanes{{support, 0.F}};
    bool changed = false;
    for (const auto &contact : query(candidate)) {
      if (!std::isfinite(contact.depth))
        return {};
      if (contact.depth <= .05F)
        continue;
      const float length = std::sqrt(Dot(contact.normal, contact.normal));
      if (!std::isfinite(length) || length < .000001F)
        return {};
      const V normal = contact.normal * (1.F / length);
      const float distance =
          (contact.depth + .05F) / length + Dot(normal, candidate - original);
      addPlane(planes, normal, distance);
      addPlane(currentPlanes, normal, distance);
      changed = true;
    }
    if (!changed)
      return candidate;
    const auto correction = ClosestCorrection(planes);
    if (!correction || !withinLimit(*correction)) {
      // A finite obstacle's old tangent plane can cease to apply after its
      // edge is cleared. Release historical constraints only if a fresh full
      // shape query proves the relaxed endpoint is actually unobstructed.
      if (pass > 0) {
        const auto relaxed = ClosestCorrection(currentPlanes);
        if (relaxed && withinLimit(*relaxed) && clear(original + *relaxed))
          return original + *relaxed;
      }
      return {};
    }
    candidate = original + *correction;
  }
  return clear(candidate) ? std::optional<V>{candidate} : std::nullopt;
}
// Prefer an outward normal-only correction. Each accepted endpoint is tested
// against all shapes; contact planes only estimate the next distance to try.
template <class V, class Limit, class Query>
std::optional<V> ResolveAlongNormal(V original, V normal, Limit maxCorrection,
                                    Query query) {
  const float limit = maxCorrection(normal);
  if (!std::isfinite(limit) || limit < 0)
    return {};
  float distance = 0, blocked = 0;
  const auto clear = [&](float amount) {
    for (const auto &contact : query(original + normal * amount))
      if (!std::isfinite(contact.depth) || contact.depth > .05F)
        return false;
    return true;
  };
  for (unsigned pass = 0; pass < 4; ++pass) {
    float step = 0;
    bool overlapping = false, estimate = true;
    for (const auto &contact : query(original + normal * distance)) {
      if (!std::isfinite(contact.depth))
        return {};
      if (contact.depth <= .05F)
        continue;
      overlapping = true;
      const float alignment = Dot(contact.normal, normal);
      if (!std::isfinite(alignment) || alignment <= .000001F)
        estimate = false;
      else
        step = std::max(step, (contact.depth + .05F) / alignment);
    }
    if (!overlapping)
      return original + normal * distance;
    blocked = distance;
    if (!estimate || distance + step > limit || step <= 0)
      break;
    distance += step;
  }
  // A perpendicular local contact can still be escaped past an obstacle's
  // finite edge. If the full normal endpoint clears, refine without accepting
  // any untested/intersecting endpoint.
  float safe = limit;
  if (!clear(limit)) {
    bool found = false;
    // Multiple obstacles can bound a clear interval before the endpoint.
    // Check bounded intermediate candidates rather than treating endpoint
    // failure as proof the entire normal direction is obstructed.
    const float start = blocked;
    for (unsigned i = 1; i < 8; ++i) {
      const float amount = start + (limit - start) * (float(i) / 8.F);
      if (clear(amount)) {
        safe = amount;
        found = true;
        break;
      }
      blocked = amount;
    }
    if (!found)
      return {};
  }
  for (unsigned i = 0; i < 8; ++i) {
    const float mid = (blocked + safe) * .5F;
    if (clear(mid))
      safe = mid;
    else
      blocked = mid;
  }
  return original + normal * safe;
}
template <class V, class Limit, class Query>
std::optional<V> Resolve(V original, V support, Limit maxCorrection,
                         Query query) {
  if (const auto normal =
          ResolveAlongNormal(original, support, maxCorrection, query))
    return normal;
  return ResolveClosest(original, support, maxCorrection, query);
}
} // namespace SkyBuild::ConstrainedPlacement
