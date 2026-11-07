#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace SkyBuild::UI {
// Render-thread drafts; only the game thread applies the returned values.
template <class Draft> class AutosaveQueue {
public:
  void Stage(std::uint64_t id, Draft value, bool ready = false) {
    std::scoped_lock lock(mutex);
    pending.insert_or_assign(id, Entry{std::move(value), ready});
  }
  std::vector<Draft> Take(bool force = false) {
    std::scoped_lock lock(mutex);
    std::vector<Draft> result;
    for (auto it = pending.begin(); it != pending.end();)
      if (force || it->second.ready) {
        result.push_back(std::move(it->second.value));
        it = pending.erase(it);
      } else
        ++it;
    return result;
  }
  bool HasReady() {
    std::scoped_lock lock(mutex);
    for (const auto &[id, entry] : pending)
      if (entry.ready)
        return true;
    return false;
  }
  bool HasPending() {
    std::scoped_lock lock(mutex);
    return !pending.empty();
  }
  void Erase(std::uint64_t id) {
    std::scoped_lock lock(mutex);
    pending.erase(id);
  }
  void Clear() {
    std::scoped_lock lock(mutex);
    pending.clear();
  }

private:
  struct Entry {
    Draft value;
    bool ready;
  };
  std::mutex mutex;
  std::map<std::uint64_t, Entry> pending;
};
} // namespace SkyBuild::UI
