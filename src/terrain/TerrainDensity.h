#pragma once
#include "terrain/TerrainVolume.h"
#include <optional>
#include <tuple>

namespace SkyBuild::Terrain {
inline constexpr float MaxStoredDensity = 1048576.F;
struct DensityNode {
  std::int32_t z{};   // World-aligned Z sample; survives grid expansion.
  std::uint32_t xy{}; // Includes a one-sample XY halo.
  bool operator==(const DensityNode &) const = default;
  bool operator<(const DensityNode &b) const {
    return std::tie(z, xy) < std::tie(b.z, b.xy);
  }
};
struct DensityChange {
  DensityNode node;
  float before{}, after{};
};
struct DensityStroke {
  Brush marker;
  std::vector<DensityChange> changes; // Sorted, one entry per changed sample.
};
struct DensityGrid {
  int low{}, high{};
  std::vector<float> values;
  std::size_t Index(DensityNode node) const {
    if (node.z < low || node.z > high || node.xy >= DensitySide * DensitySide)
      throw std::runtime_error("Invalid terrain density sample");
    return std::size_t(node.z - low) * DensitySide * DensitySide + node.xy;
  }
  float At(DensityNode node) const { return values[Index(node)]; }
  float Sample(Vec p) const {
    if (!Finite(p))
      throw std::runtime_error("Invalid terrain density query");
    const float fx = std::clamp(p.x / Step + 1, 0.F, float(DensitySide - 1));
    const float fy = std::clamp(p.y / Step + 1, 0.F, float(DensitySide - 1));
    const float worldZ = std::clamp(p.z / Step, float(low), float(high));
    const int cellZ = std::min(int(std::floor(worldZ)), high - 1);
    const int x = std::min(int(fx), DensitySide - 2),
              y = std::min(int(fy), DensitySide - 2);
    const int z = cellZ - low;
    const auto layer = [&](int zz) {
      const auto base =
          std::size_t(zz) * DensitySide * DensitySide + y * DensitySide + x;
      return std::lerp(std::lerp(values[base], values[base + 1], fx - x),
                       std::lerp(values[base + DensitySide],
                                 values[base + DensitySide + 1], fx - x),
                       fy - y);
    };
    return std::lerp(layer(z), layer(z + 1), worldZ - cellZ);
  }
};
// A cube contributes to the zero isosurface only when its corners straddle
// zero. Preserve every such corner exactly. Uniform-sign regions can use a
// canonical finite-band value without changing any MC33 edge intersection.
template <class Sample>
inline float CanonicalDensityValue(float value, Sample sample) {
  const bool solid = value <= 0;
  const float canonical = solid ? -Step : Step;
  if (value == canonical)
    return value;
  for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        if (!x && !y && !z)
          continue;
        const auto neighbor = sample(x, y, z);
        // A missing neighbor is unknown, never an assumption of empty space.
        if (!neighbor || ((*neighbor <= 0) != solid))
          return value;
      }
  return canonical;
}
struct DensityCache {
  std::shared_ptr<const DensityGrid> original, current, completed, baseline;
  std::vector<std::shared_ptr<const DensityStroke>> strokes;
  std::shared_ptr<const DensityGrid> from;
  std::vector<DensityNode> changed;
  std::array<bool, DensitySide * DensitySide> modified{};
  float bound{Step};
};
inline float CachedDensity(const DensityCache &cache, Vec p) {
  return cache.current->Sample(p);
}
inline float DensityBottom(const DensityCache &cache) {
  return cache.current->low * Step;
}
inline float DensityTop(const DensityCache &cache) {
  return cache.current->high * Step;
}

inline std::shared_ptr<const DensityHalo>
OriginalDensityHalo(const Volume &volume) {
  if (volume.densityHeights)
    return volume.densityHeights;
  auto halo = std::make_shared<DensityHalo>();
  for (int y = 0; y < DensitySide; ++y)
    for (int x = 0; x < DensitySide; ++x)
      (*halo)[y * DensitySide + x] = volume.Height(
          volume.origin.x + (x - 1) * Step, volume.origin.y + (y - 1) * Step);
  return halo;
}
inline std::shared_ptr<DensityGrid> MakeOriginalGrid(const Volume &volume) {
  if (!volume.Valid() || std::fmod(volume.bottom, Step) != 0 ||
      std::fmod(volume.top, Step) != 0)
    throw std::runtime_error("Invalid terrain density grid bounds");
  auto grid = std::make_shared<DensityGrid>();
  grid->low = int(volume.bottom / Step);
  grid->high = int(volume.top / Step);
  grid->values.resize(std::size_t(grid->high - grid->low + 1) * DensitySide *
                      DensitySide);
  const auto halo = OriginalDensityHalo(volume);
  for (int z = grid->low; z <= grid->high; ++z)
    for (unsigned xy = 0; xy < halo->size(); ++xy)
      grid->values[grid->Index({z, xy})] =
          std::clamp(z * Step - (*halo)[xy], -Step, Step);
  return grid;
}
inline void UpdateDensityColumns(DensityCache &cache) {
  std::array<bool, DensitySide * DensitySide> inspect{};
  for (const auto &node : cache.changed)
    inspect[node.xy] = true;
  for (unsigned xy = 0; xy < inspect.size(); ++xy)
    if (inspect[xy]) {
      cache.modified[xy] = false;
      for (int z = cache.current->low; z <= cache.current->high; ++z)
        if (cache.current->At({z, xy}) != cache.original->At({z, xy})) {
          cache.modified[xy] = true;
          break;
        }
    }
}
inline bool DensityAffected(const Volume &volume, float x, float y) {
  if (!volume.density)
    return std::any_of(volume.edits.begin(), volume.edits.end(),
                       [&](const Brush &b) {
                         const auto dx = x - b.center.x, dy = y - b.center.y;
                         return dx * dx + dy * dy <= b.radius * b.radius;
                       });
  const auto gx =
      std::clamp((x - volume.origin.x) / Step + 1, 0.F, float(DensitySide - 1));
  const auto gy =
      std::clamp((y - volume.origin.y) / Step + 1, 0.F, float(DensitySide - 1));
  for (int yy = int(gy); yy <= int(std::ceil(gy)); ++yy)
    for (int xx = int(gx); xx <= int(std::ceil(gx)); ++xx)
      if (volume.density->modified[yy * DensitySide + xx])
        return true;
  return false;
}
inline void CheckpointDensity(Volume &volume) {
  if (!volume.density || volume.density->completed == volume.density->current)
    return;
  auto cache = std::make_shared<DensityCache>(*volume.density);
  cache->completed = cache->current;
  cache->from.reset();
  cache->changed.clear();
  volume.density = std::move(cache);
}
inline void GrowDensity(Volume &volume) {
  if (!volume.density || (volume.bottom == DensityBottom(*volume.density) &&
                          volume.top == DensityTop(*volume.density)))
    return;
  auto cache = std::make_shared<DensityCache>(*volume.density);
  auto original = MakeOriginalGrid(volume);
  const auto grow = [&](const std::shared_ptr<const DensityGrid> &old) {
    auto grid = std::make_shared<DensityGrid>(*original);
    for (int z = old->low; z <= old->high; ++z)
      std::copy_n(old->values.begin() +
                      std::size_t(z - old->low) * DensitySide * DensitySide,
                  DensitySide * DensitySide,
                  grid->values.begin() +
                      std::size_t(z - grid->low) * DensitySide * DensitySide);
    return grid;
  };
  cache->original = original;
  cache->current = grow(cache->current);
  cache->completed = grow(cache->completed);
  if (cache->baseline)
    cache->baseline = grow(cache->baseline);
  cache->from.reset();
  volume.density = std::move(cache);
}
inline bool ApplyDensityBrush(Volume &volume, const Brush &brush);
inline void InitializeDensity(Volume &volume) {
  if (volume.density)
    return;
  const auto legacy = volume.edits;
  auto prepared = volume;
  prepared.densityHeights = OriginalDensityHalo(volume);
  prepared.edits.clear();
  auto cache = std::make_shared<DensityCache>();
  cache->original = cache->current = cache->completed =
      MakeOriginalGrid(prepared);
  prepared.density = cache;
  // One-time upgrade only. Ordinary editing and generation never replay CSG.
  for (const auto &brush : legacy)
    ApplyDensityBrush(prepared, brush);
  CheckpointDensity(prepared);
  volume = std::move(prepared);
}
inline bool CommitDensityChanges(Volume &volume, const Brush &brush,
                                 const std::vector<DensityChange> &updates,
                                 std::shared_ptr<DensityGrid> current = {}) {
  const auto &old = *volume.density;
  const auto &grid = *old.current;
  if (updates.empty())
    return false;
  const bool extend = brush.stroke && !old.strokes.empty() &&
                      old.strokes.back()->marker.stroke == brush.stroke;
  if (!extend && old.strokes.size() >= MaxEdits)
    throw std::runtime_error("Terrain stroke history limit reached");
  auto cache = std::make_shared<DensityCache>(old);
  if (!current)
    current = std::make_shared<DensityGrid>(grid);
  cache->from = old.current;
  cache->changed.clear();
  for (const auto &update : updates) {
    current->values[current->Index(update.node)] = update.after;
    cache->changed.push_back(update.node);
  }
  cache->current = current;
  cache->bound = std::max(old.bound, brush.radius);
  auto stroke = std::make_shared<DensityStroke>();
  stroke->marker = extend ? old.strokes.back()->marker : brush;
  const std::vector<DensityChange> empty;
  const auto &prior = extend ? old.strokes.back()->changes : empty;
  stroke->changes.reserve(prior.size() + updates.size());
  std::size_t i{}, j{};
  while (i < prior.size() || j < updates.size()) {
    DensityChange change;
    if (j == updates.size() ||
        (i < prior.size() && prior[i].node < updates[j].node))
      change = prior[i++];
    else if (i == prior.size() || updates[j].node < prior[i].node)
      change = updates[j++];
    else {
      change = {prior[i].node, prior[i].before, updates[j].after};
      ++i;
      ++j;
    }
    if (change.before != change.after)
      stroke->changes.push_back(change);
  }
  if (extend)
    cache->strokes.back() = stroke;
  else {
    cache->strokes.push_back(stroke);
    volume.edits.push_back(brush);
  }
  UpdateDensityColumns(*cache);
  volume.density = std::move(cache);
  return true;
}
inline float SurfaceBrushFalloff(float distance, float radius) {
  const float edge = std::clamp(2 * (1 - distance / radius), 0.F, 1.F);
  return edge * edge * (3 - 2 * edge);
}
// Average the one-sided gradients of cells whose incident edges actually
// cross zero. A distant corner or saturated background must not select a
// different slope for the surface at this node.
template <class Sample>
inline Vec SurfaceDensityGradient(Vec node, Sample sample) {
  const auto center = sample(node);
  if (!center)
    return {};
  const std::array<std::optional<float>, 6> sides{
      sample(node + Vec{-Step, 0, 0}), sample(node + Vec{Step, 0, 0}),
      sample(node + Vec{0, -Step, 0}), sample(node + Vec{0, Step, 0}),
      sample(node + Vec{0, 0, -Step}), sample(node + Vec{0, 0, Step})};
  Vec sum{};
  int count{};
  for (int z = 0; z < 2; ++z)
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        const auto a = sides[x], b = sides[2 + y], c = sides[4 + z];
        if (!a || !b || !c)
          continue;
        const bool sign = *center <= 0;
        if ((*a <= 0) == sign && (*b <= 0) == sign && (*c <= 0) == sign)
          continue;
        sum = sum + Vec{(*a - *center) / (x ? Step : -Step),
                        (*b - *center) / (y ? Step : -Step),
                        (*c - *center) / (z ? Step : -Step)};
        ++count;
      }
  return count ? sum * (1.F / count) : Vec{};
}
template <class Neighbor>
inline bool ApplyDensityBrush(Volume &volume, const Brush &brush,
                              Neighbor neighbor) {
  if (!Finite(brush.center) || !std::isfinite(brush.radius) ||
      brush.radius < 16.F || brush.radius > 512 ||
      !std::isfinite(brush.strength) || brush.strength < 0 ||
      brush.stroke >= (std::uint64_t(1) << 62) - 1)
    throw std::runtime_error("Invalid terrain density brush");
  InitializeDensity(volume);
  volume.bottom = std::min(
      volume.bottom,
      std::floor((brush.center.z - brush.radius - 2 * Step) / Step) * Step);
  volume.top = std::max(
      volume.top,
      std::ceil((brush.center.z + brush.radius + 2 * Step) / Step) * Step);
  if (!volume.Valid())
    throw std::runtime_error("Terrain density height limit reached");
  GrowDensity(volume);
  const auto &old = *volume.density;
  const auto &grid = *old.current;
  const auto reach = brush.radius + old.bound + 1;
  const int x0 = std::clamp(
      int(std::floor((brush.center.x - reach - volume.origin.x) / Step)) + 1, 0,
      DensitySide - 1);
  const int x1 = std::clamp(
      int(std::ceil((brush.center.x + reach - volume.origin.x) / Step)) + 1, 0,
      DensitySide - 1);
  const int y0 = std::clamp(
      int(std::floor((brush.center.y - reach - volume.origin.y) / Step)) + 1, 0,
      DensitySide - 1);
  const int y1 = std::clamp(
      int(std::ceil((brush.center.y + reach - volume.origin.y) / Step)) + 1, 0,
      DensitySide - 1);
  const int z0 =
      std::max(grid.low, int(std::floor((brush.center.z - reach) / Step)));
  const int z1 =
      std::min(grid.high, int(std::ceil((brush.center.z + reach) / Step)));
  auto current = std::make_shared<DensityGrid>(grid);
  const int ax = std::max(0, x0 - 1), bx = std::min(DensitySide - 1, x1 + 1);
  const int ay = std::max(0, y0 - 1), by = std::min(DensitySide - 1, y1 + 1);
  const int az = std::max(grid.low, z0 - 1), bz = std::min(grid.high, z1 + 1);
  const auto sampleBefore = [&](Vec p) -> std::optional<float> {
    const int x = int(std::lround((p.x - volume.origin.x) / Step)) + 1;
    const int y = int(std::lround((p.y - volume.origin.y) / Step)) + 1;
    const int z = int(std::lround(p.z / Step));
    if (x >= 0 && x < DensitySide && y >= 0 && y < DensitySide &&
        z >= grid.low && z <= grid.high)
      return grid.At({z, unsigned(y * DensitySide + x)});
    return neighbor(p);
  };
  const auto sculpt = [&](float before, Vec p) {
    const auto sphere = Length(p - brush.center) - brush.radius;
    if (brush.strength ==
        0) // Full CSG operations retain their original semantics.
      return brush.add ? std::min(before, sphere) : std::max(before, -sphere);
    if (sphere > 2 * Step)
      return before;
    const auto gradient = SurfaceDensityGradient(p, sampleBefore);
    const float magnitude = Length(gradient);
    if (magnitude <= 1e-6F)
      return before;
    const Vec surface = p - gradient * (before / (magnitude * magnitude));
    const float amount =
        brush.strength *
        SurfaceBrushFalloff(Length(surface - brush.center), brush.radius) *
        magnitude;
    const float target = sphere * magnitude;
    const float next =
        brush.add ? std::min(before, target) : std::max(before, -target);
    float after = std::clamp(next, before - amount, before + amount);
    if ((brush.add && before >= 0 && after < 0) ||
        (!brush.add && before <= 0 && after > 0)) {
      // When the front crosses a grid node, the next cell may contain only
      // saturated background values. Carry the remaining WORLD displacement
      // into that cell using the same linear zero interpolation as MC33.
      // Reusing the old cell's density slope here reintroduces speed changes.
      const Vec normal = gradient * (1 / magnitude);
      const float components[]{normal.x, normal.y, normal.z};
      int axis = 0;
      for (int i = 1; i < 3; ++i)
        if (std::abs(components[i]) > std::abs(components[axis]))
          axis = i;
      Vec offset{};
      const float direction =
          (components[axis] > 0 ? 1.F : -1.F) * (brush.add ? 1.F : -1.F);
      if (axis == 0)
        offset.x = direction * Step;
      else if (axis == 1)
        offset.y = direction * Step;
      else
        offset.z = direction * Step;
      const Vec adjacent = p + offset;
      if (Length(SurfaceDensityGradient(adjacent, sampleBefore)) <= 1e-6F) {
        const auto raw = sampleBefore(adjacent);
        if (raw) {
          const float fixed = CanonicalDensityValue(*raw, [&](int x, int y,
                                                              int z) {
            return sampleBefore(adjacent + Vec{x * Step, y * Step, z * Step});
          });
          if (fixed * after < 0) {
            const float fraction =
                std::abs(after) /
                (magnitude * Step * std::abs(components[axis]));
            if (fraction < 1.F)
              after = -fixed * fraction / (1 - fraction);
          }
        }
      }
    }
    return after;
  };
  const auto normalize = [&](bool afterBrush) {
    for (int z = az; z <= bz; ++z)
      for (int y = ay; y <= by; ++y)
        for (int x = ax; x <= bx; ++x) {
          const DensityNode node{z, unsigned(y * DensitySide + x)};
          const Vec world{volume.origin.x + (x - 1) * Step,
                          volume.origin.y + (y - 1) * Step, z * Step};
          auto &value = current->values[current->Index(node)];
          value = CanonicalDensityValue(
              value, [&](int dx, int dy, int dz) -> std::optional<float> {
                const int xx = x + dx, yy = y + dy, zz = z + dz;
                if (xx >= 0 && xx < DensitySide && yy >= 0 &&
                    yy < DensitySide && zz >= grid.low && zz <= grid.high)
                  return current->At({zz, unsigned(yy * DensitySide + xx)});
                const auto p = world + Vec{dx * Step, dy * Step, dz * Step};
                const auto outside = neighbor(p);
                return outside && afterBrush
                           ? std::optional<float>{sculpt(*outside, p)}
                           : outside;
              });
        }
  };
  // First remove invisible residue from older fields. Signs never change, so
  // this pass is order-independent and preserves all current surface cubes.
  normalize(false);
  for (int z = z0; z <= z1; ++z)
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const DensityNode node{z, unsigned(y * DensitySide + x)};
        auto &value = current->values[current->Index(node)];
        value = sculpt(value, {volume.origin.x + (x - 1) * Step,
                               volume.origin.y + (y - 1) * Step, z * Step});
      }
  normalize(true);
  std::vector<DensityChange> updates;
  for (int z = az; z <= bz; ++z)
    for (int y = ay; y <= by; ++y)
      for (int x = ax; x <= bx; ++x) {
        const DensityNode node{z, unsigned(y * DensitySide + x)};
        const auto before = grid.At(node), after = current->At(node);
        if (before != after)
          updates.push_back({node, before, after});
      }
  return CommitDensityChanges(volume, brush, updates, std::move(current));
}
inline bool ApplyDensityBrush(Volume &volume, const Brush &brush) {
  return ApplyDensityBrush(volume, brush,
                           [](Vec) -> std::optional<float> { return {}; });
}

inline std::optional<float> HighestDensityColumn(const DensityGrid &grid,
                                                 unsigned xy) {
  for (int z = grid.high - 1; z >= grid.low; --z) {
    const float below = grid.At({z, xy}), above = grid.At({z + 1, xy});
    if (below <= 0 && above > 0)
      return (z + (-below / (above - below))) * Step;
  }
  return {};
}
// Recover this column's surface at the beginning of the held stroke using
// existing sparse undo values. Once a cut opens a roof, the exposed cave floor
// must not become a new surface to raise back up toward the same target.
inline std::optional<float> LevelColumnStart(const DensityGrid &grid,
                                             unsigned xy,
                                             const DensityStroke *stroke) {
  const auto sample = [&](int z) {
    const DensityNode node{z, xy};
    if (stroke) {
      const auto found =
          std::lower_bound(stroke->changes.begin(), stroke->changes.end(), node,
                           [](const DensityChange &change, DensityNode key) {
                             return change.node < key;
                           });
      if (found != stroke->changes.end() && found->node == node)
        return found->before;
    }
    return grid.At(node);
  };
  for (int z = grid.high - 1; z >= grid.low; --z) {
    const float below = sample(z), above = sample(z + 1);
    if (below <= 0 && above > 0)
      return (z - below / (above - below)) * Step;
  }
  return {};
}
// Flatten the upper surface toward a sampled world height. Advance actual
// surface height, not a blend of opposing saturated density signs: the latter
// creates a slab of zero samples instead of one moving surface.
template <class Target>
inline bool ApplyDensitySurface(Volume &volume, const Brush &brush,
                                Target target) {
  if (!Finite(brush.center) || !std::isfinite(brush.radius) ||
      brush.radius < 16.F || brush.radius > 512 ||
      !std::isfinite(brush.strength) || brush.strength <= 0 ||
      brush.stroke >= (std::uint64_t(1) << 62) - 1)
    throw std::runtime_error("Invalid terrain level brush");
  InitializeDensity(volume);
  volume.bottom = std::min(
      volume.bottom, std::floor((brush.center.z - 2 * Step) / Step) * Step);
  volume.top = std::max(volume.top,
                        std::ceil((brush.center.z + 2 * Step) / Step) * Step);
  if (!volume.Valid())
    throw std::runtime_error("Terrain level height limit reached");
  GrowDensity(volume);
  const auto &grid = *volume.density->current;
  const float bound = volume.density->bound;
  const auto &strokes = volume.density->strokes;
  const auto *stroke = brush.stroke && !strokes.empty() &&
                               strokes.back()->marker.stroke == brush.stroke
                           ? strokes.back().get()
                           : nullptr;
  std::vector<DensityChange> updates;
  // Work only on XY columns inside the brush, including the existing halo.
  for (int y = 0; y < DensitySide; ++y)
    for (int x = 0; x < DensitySide; ++x) {
      const float dx = volume.origin.x + (x - 1) * Step - brush.center.x;
      const float dy = volume.origin.y + (y - 1) * Step - brush.center.y;
      const float distance = std::sqrt(dx * dx + dy * dy);
      if (distance >= brush.radius)
        continue;
      const unsigned xy = unsigned(y * DensitySide + x);
      const auto height = HighestDensityColumn(grid, xy);
      if (!height)
        continue;
      const auto desired = target(volume.origin.x + (x - 1) * Step,
                                  volume.origin.y + (y - 1) * Step);
      if (!desired || !std::isfinite(*desired))
        continue;
      const auto initial = stroke ? LevelColumnStart(grid, xy, stroke) : height;
      if (!initial)
        continue;
      const bool raising = *desired > *initial;
      if ((raising && *height >= *desired) || (!raising && *height <= *desired))
        continue;
      // Full strength through the central half, smoothly fading at the edge.
      const float amount =
          brush.strength * SurfaceBrushFalloff(distance, brush.radius);
      const float next =
          std::clamp(*desired, *height - amount, *height + amount);
      if (next == *height)
        continue;
      // Replace only cells swept by this pulse's upper boundary, using world
      // distances so old density scaling cannot change leveling speed. During
      // a cut, existing cave air below the old surface is never filled.
      const int low =
          std::max(grid.low, int(std::floor(std::min(*height, next) / Step)));
      const int high =
          std::min(grid.high, int(std::ceil(std::max(*height, next) / Step)));
      for (int z = low; z <= high; ++z) {
        const DensityNode node{z, xy};
        const auto before = grid.At(node);
        const float plane = std::clamp(z * Step - next, -bound, bound);
        const float after =
            !raising && before > 0 && z * Step < *height ? before : plane;
        if (after != before)
          updates.push_back({node, before, after});
      }
    }
  // The shared stroke merger requires world-Z/XY ordering.
  std::sort(updates.begin(), updates.end(),
            [](const auto &a, const auto &b) { return a.node < b.node; });
  return CommitDensityChanges(volume, brush, updates);
}
inline bool ApplyDensityLevel(Volume &volume, const Brush &brush) {
  return ApplyDensitySurface(volume, brush, [&](float, float) {
    return std::optional<float>(brush.center.z);
  });
}
// A normalized 3D Gaussian kernel reads an immutable multi-cell snapshot.
// Only samples strictly inside the spherical brush are changed. Strength is a
// density-units-per-second limit, already integrated over time by the caller.
template <class Densities>
inline bool ApplyDensitySmooth(Volume &volume, const Brush &brush,
                               Densities densityAt) {
  if (!Finite(brush.center) || !std::isfinite(brush.radius) ||
      brush.radius < 16.F || brush.radius > 512 ||
      !std::isfinite(brush.strength) || brush.strength <= 0 ||
      brush.stroke >= (std::uint64_t(1) << 62) - 1)
    throw std::runtime_error("Invalid terrain smooth brush");
  InitializeDensity(volume);
  const auto &grid = *volume.density->current;
  const auto range = [&](float center, float origin) {
    return std::pair{
        std::clamp(int(std::floor((center - brush.radius - origin) / Step)) + 1,
                   0, DensitySide - 1),
        std::clamp(int(std::ceil((center + brush.radius - origin) / Step)) + 1,
                   0, DensitySide - 1)};
  };
  const auto [x0, x1] = range(brush.center.x, volume.origin.x);
  const auto [y0, y1] = range(brush.center.y, volume.origin.y);
  const int z0 = std::max(
      grid.low, int(std::floor((brush.center.z - brush.radius) / Step)));
  const int z1 = std::min(
      grid.high, int(std::ceil((brush.center.z + brush.radius) / Step)));
  std::vector<DensityChange> updates;
  for (int z = z0; z <= z1; ++z)
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const Vec world{volume.origin.x + (x - 1) * Step,
                        volume.origin.y + (y - 1) * Step, z * Step};
        const float distance = Length(world - brush.center);
        if (distance >= brush.radius)
          continue;
        float sum{};
        bool complete = true;
        for (int dz = -1; dz <= 1 && complete; ++dz)
          for (int dy = -1; dy <= 1 && complete; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const auto value =
                  densityAt(world + Vec{dx * Step, dy * Step, dz * Step});
              if (!value || !std::isfinite(*value)) {
                complete = false;
                break;
              }
              const float weight = float((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1) *
                                         (dz == 0 ? 2 : 1));
              sum += weight * *value;
            }
        if (!complete)
          continue; // Never invent values at missing snapshot boundaries.
        const DensityNode node{z, unsigned(y * DensitySide + x)};
        const float before = grid.At(node);
        const float edge =
            std::clamp(2 * (1 - distance / brush.radius), 0.F, 1.F);
        const float amount = brush.strength * edge * edge * (3 - 2 * edge);
        const float after =
            std::clamp(sum / 64.F, before - amount, before + amount);
        if (after != before)
          updates.push_back({node, before, after});
      }
  return CommitDensityChanges(volume, brush, updates);
}
// Rebase stored sample differences for a non-LIFO restore. No brush evaluation.
inline void RebuildDensityDeltas(DensityCache &cache) {
  auto current = std::make_shared<DensityGrid>(
      *(cache.baseline ? cache.baseline : cache.original));
  cache.bound = Step;
  for (auto &old : cache.strokes) {
    auto stroke = std::make_shared<DensityStroke>();
    stroke->marker = old->marker;
    for (const auto &change : old->changes) {
      const auto before = current->At(change.node);
      const auto after = before == change.before
                             ? change.after
                             : before + (change.after - change.before);
      if (!std::isfinite(after) || std::abs(after) > MaxStoredDensity)
        throw std::runtime_error("Invalid terrain density delta");
      current->values[current->Index(change.node)] = after;
      if (before != after)
        stroke->changes.push_back({change.node, before, after});
    }
    old = stroke;
  }
  cache.current = current;
  cache.modified.fill(false);
  for (int z = current->low; z <= current->high; ++z)
    for (unsigned xy = 0; xy < DensitySide * DensitySide; ++xy) {
      const auto value = current->At({z, xy});
      cache.bound = std::max(cache.bound, std::abs(value));
      if (value != cache.original->At({z, xy}))
        cache.modified[xy] = true;
    }
}
inline std::shared_ptr<const DensityStroke>
RemoveDensityStroke(Volume &volume, std::size_t index) {
  auto cache = std::make_shared<DensityCache>(*volume.density);
  const auto removed = cache->strokes.at(index);
  cache->from = cache->current;
  cache->changed.clear();
  for (const auto &change : removed->changes)
    cache->changed.push_back(change.node);
  if (index + 1 == cache->strokes.size()) {
    auto current = std::make_shared<DensityGrid>(*cache->current);
    for (const auto &change : removed->changes)
      current->values[current->Index(change.node)] = change.before;
    cache->current = current;
    cache->strokes.pop_back();
    UpdateDensityColumns(*cache);
  } else {
    for (std::size_t i = index + 1; i < cache->strokes.size(); ++i)
      for (const auto &change : cache->strokes[i]->changes)
        cache->changed.push_back(change.node);
    cache->strokes.erase(cache->strokes.begin() + index);
    RebuildDensityDeltas(*cache);
  }
  volume.edits.erase(volume.edits.begin() + index);
  volume.density = std::move(cache);
  return removed;
}
inline void InsertDensityStroke(Volume &volume, std::size_t index,
                                std::shared_ptr<const DensityStroke> stroke) {
  if (!volume.density || index > volume.density->strokes.size() ||
      volume.edits.size() >= MaxEdits)
    throw std::runtime_error(
        "Terrain redo no longer matches its density history");
  auto cache = std::make_shared<DensityCache>(*volume.density);
  cache->from = cache->current;
  cache->changed.clear();
  for (const auto &change : stroke->changes)
    cache->changed.push_back(change.node);
  if (index == cache->strokes.size()) {
    auto current = std::make_shared<DensityGrid>(*cache->current);
    for (const auto &change : stroke->changes) {
      if (current->At(change.node) != change.before)
        throw std::runtime_error("Terrain redo density mismatch");
      current->values[current->Index(change.node)] = change.after;
    }
    cache->current = current;
    cache->strokes.push_back(stroke);
    UpdateDensityColumns(*cache);
    for (const auto &change : stroke->changes)
      cache->bound = std::max(
          {cache->bound, std::abs(change.before), std::abs(change.after)});
  } else {
    for (std::size_t i = index; i < cache->strokes.size(); ++i)
      for (const auto &change : cache->strokes[i]->changes)
        cache->changed.push_back(change.node);
    cache->strokes.insert(cache->strokes.begin() + index, stroke);
    RebuildDensityDeltas(*cache);
  }
  volume.edits.insert(volume.edits.begin() + index, stroke->marker);
  volume.density = std::move(cache);
}
inline std::size_t DensityMemory(const Volume &volume) {
  if (!volume.density)
    return 0;
  const auto &cache = *volume.density;
  std::size_t result = cache.original->values.size() * sizeof(float);
  if (cache.current != cache.original)
    result += cache.current->values.size() * sizeof(float);
  if (cache.completed != cache.original && cache.completed != cache.current)
    result += cache.completed->values.size() * sizeof(float);
  if (cache.from && cache.from != cache.original &&
      cache.from != cache.current && cache.from != cache.completed)
    result += cache.from->values.size() * sizeof(float);
  if (cache.baseline && cache.baseline != cache.original &&
      cache.baseline != cache.current && cache.baseline != cache.completed &&
      cache.baseline != cache.from)
    result += cache.baseline->values.size() * sizeof(float);
  result += sizeof(Volume) + sizeof(DensityHalo) +
            volume.edits.size() * sizeof(Brush) +
            cache.changed.size() * sizeof(DensityNode);
  for (const auto &stroke : cache.strokes)
    result +=
        sizeof(DensityStroke) + stroke->changes.size() * sizeof(DensityChange);
  return result;
}
inline bool HasTerrainChanges(const Volume &volume) {
  return volume.density ? std::any_of(volume.density->modified.begin(),
                                      volume.density->modified.end(),
                                      [](bool x) { return x; })
                        : !volume.edits.empty();
}
} // namespace SkyBuild::Terrain
