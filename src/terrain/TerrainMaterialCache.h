#pragma once
#include <map>
#include <memory>
#include <mutex>

namespace SkyBuild::Terrain {
// Cache keys never keep source textures or materials alive. Chunks own the
// returned record; expired entries are pruned on subsequent acquisitions.
template <class Key, class Value> class WeakMaterialCache {
  std::mutex mutex;
  std::map<Key, std::weak_ptr<Value>> entries;

public:
  template <class Create>
  std::shared_ptr<Value> Acquire(const Key &key, Create &&create) {
    {
      std::lock_guard guard(mutex);
      if (const auto it = entries.find(key); it != entries.end())
        if (auto value = it->second.lock())
          return value;
    }
    // Native material setup may invoke external hooks; keep it outside locks.
    auto value = create();
    if (!value)
      return {};
    std::lock_guard guard(mutex);
    if (const auto it = entries.find(key); it != entries.end())
      if (auto existing = it->second.lock())
        return existing;
    std::erase_if(entries,
                  [](const auto &entry) { return entry.second.expired(); });
    entries[key] = value;
    return value;
  }
};
} // namespace SkyBuild::Terrain
