#pragma once
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>
namespace SkyBuild::Navmesh {
struct XYBounds {
  float left, bottom, right, top;
};
// Immutable broad-phase index. Queries return conservative candidates only;
// callers retain the original geometric predicates and ordering rules.
class XYBoundsIndex {
public:
  explicit XYBoundsIndex(std::vector<XYBounds> entries)
      : boxes(std::move(entries)), order(boxes.size()) {
    std::iota(order.begin(), order.end(), 0U);
    if (!order.empty()) {
      nodes.reserve(order.size() * 2);
      Build(0, static_cast<std::uint32_t>(order.size()));
    }
  }
  template <class Stop>
  bool Query(XYBounds bounds, std::vector<std::uint32_t> &output,
             Stop stop) const {
    output.clear();
    if (!nodes.empty() && !Visit(0, bounds, output, stop))
      return false;
    // Stable source order preserves tie-breaking in the narrow phase.
    std::sort(output.begin(), output.end());
    return true;
  }

private:
  struct Node {
    XYBounds bounds;
    std::uint32_t begin{}, end{}, left{}, right{};
  };
  static bool Overlaps(XYBounds a, XYBounds b) {
    return a.left <= b.right && b.left <= a.right && a.bottom <= b.top &&
           b.bottom <= a.top;
  }
  std::uint32_t Build(std::uint32_t begin, std::uint32_t end) {
    auto bounds = boxes[order[begin]];
    for (auto i = begin + 1; i < end; ++i) {
      const auto box = boxes[order[i]];
      bounds.left = std::min(bounds.left, box.left);
      bounds.bottom = std::min(bounds.bottom, box.bottom);
      bounds.right = std::max(bounds.right, box.right);
      bounds.top = std::max(bounds.top, box.top);
    }
    const auto id = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back({bounds, begin, end});
    if (end - begin > 8) {
      const bool x = bounds.right - bounds.left >= bounds.top - bounds.bottom;
      const auto middle = begin + (end - begin) / 2;
      std::nth_element(order.begin() + begin, order.begin() + middle,
                       order.begin() + end, [&](auto a, auto b) {
                         const double ca =
                             x ? double(boxes[a].left) + boxes[a].right
                               : double(boxes[a].bottom) + boxes[a].top;
                         const double cb =
                             x ? double(boxes[b].left) + boxes[b].right
                               : double(boxes[b].bottom) + boxes[b].top;
                         return ca != cb ? ca < cb : a < b;
                       });
      const auto left = Build(begin, middle), right = Build(middle, end);
      nodes[id].left = left;
      nodes[id].right = right;
    }
    return id;
  }
  template <class Stop>
  bool Visit(std::uint32_t id, XYBounds bounds,
             std::vector<std::uint32_t> &output, Stop &stop) const {
    if (stop())
      return false;
    const auto &node = nodes[id];
    if (!Overlaps(node.bounds, bounds))
      return true;
    if (node.end - node.begin <= 8) {
      for (auto i = node.begin; i < node.end; ++i)
        if (Overlaps(boxes[order[i]], bounds))
          output.push_back(order[i]);
      return true;
    }
    return Visit(node.left, bounds, output, stop) &&
           Visit(node.right, bounds, output, stop);
  }
  std::vector<XYBounds> boxes;
  std::vector<std::uint32_t> order;
  std::vector<Node> nodes;
};
} // namespace SkyBuild::Navmesh
