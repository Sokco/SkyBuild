#pragma once
#include "navmesh/diagnostics/Diagnostics.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>
#include <vector>

namespace SkyBuild::Navmesh {
// One summary per operation, emitted after later-declared native guards have
// unwound. Nested stages are inclusive, not an additive breakdown.
class TimingTrace {
  using Clock = std::chrono::steady_clock;
  struct Entry {
    double total{}, maximum{};
    std::size_t calls{};
    std::uint32_t slowestID{};
  };
  std::string_view operation;
  bool enabled{Diagnostics::Enabled() &&
               spdlog::should_log(spdlog::level::debug)};
  Clock::time_point started{enabled ? Clock::now() : Clock::time_point{}},
      checkpoint{started};
  std::map<std::string_view, Entry> entries;
  void Add(std::string_view name, Clock::time_point from,
           std::uint32_t id = 0) noexcept {
    if (!enabled)
      return;
    try {
      const auto elapsed =
          std::chrono::duration<double, std::milli>(Clock::now() - from)
              .count();
      auto &entry = entries[name];
      entry.total += elapsed;
      ++entry.calls;
      if (elapsed >= entry.maximum) {
        entry.maximum = elapsed;
        entry.slowestID = id;
      }
    } catch (...) {
    }
  }

public:
  explicit TimingTrace(std::string_view name) : operation(name) {}
  TimingTrace(const TimingTrace &) = delete;
  TimingTrace &operator=(const TimingTrace &) = delete;
  class Scope {
    TimingTrace &trace;
    std::string_view name;
    std::uint32_t id;
    Clock::time_point started;

  public:
    Scope(TimingTrace &owner, std::string_view stage, std::uint32_t identity)
        : trace(owner), name(stage), id(identity),
          started(owner.enabled ? Clock::now() : Clock::time_point{}) {}
    Scope(const Scope &) = delete;
    ~Scope() { trace.Add(name, started, id); }
  };
  [[nodiscard]] Scope Measure(std::string_view name, std::uint32_t id = 0) {
    return Scope{*this, name, id};
  }
  void Mark(std::string_view completedStage) noexcept {
    if (!enabled)
      return;
    Add(completedStage, checkpoint);
    checkpoint = Clock::now();
  }
  ~TimingTrace() noexcept {
    if (!enabled)
      return;
    try {
      const auto total =
          std::chrono::duration<double, std::milli>(Clock::now() - started)
              .count();
      Mark("remaining / cleanup");
      std::vector<std::pair<std::string_view, Entry>> ordered(entries.begin(),
                                                              entries.end());
      std::sort(ordered.begin(), ordered.end(),
                [](const auto &a, const auto &b) {
                  return a.second.total > b.second.total;
                });
      std::ostringstream summary;
      summary << fmt::format(
          "Navmesh timing {}: total {:.3f} ms (nested stages inclusive)",
          operation, total);
      for (const auto &[name, value] : ordered)
        summary << fmt::format(
            "\n  {}: {:.3f} ms, {} calls, max {:.3f} ms, slowest ID {:08X}",
            name, value.total, value.calls, value.maximum, value.slowestID);
      spdlog::debug("{}", summary.str());
    } catch (...) {
    }
  }
};
} // namespace SkyBuild::Navmesh
