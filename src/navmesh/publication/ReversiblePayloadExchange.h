#pragma once
#include <span>
#include <type_traits>
namespace SkyBuild::Navmesh {
// All exchanges and rollback are allocation-free and noexcept. Inspection may
// fail at any point; exact reverse-order restoration runs before it propagates.
template <class Entry, class Exchange, class Inspect>
void WithExchangedPayloads(std::span<Entry> entries, Exchange exchange,
                           Inspect inspect) {
  static_assert(std::is_nothrow_invocable_v<Exchange &, Entry &>);
  struct Rollback {
    std::span<Entry> entries;
    Exchange &exchange;
    std::size_t changed{};
    ~Rollback() noexcept {
      while (changed)
        exchange(entries[--changed]);
    }
  } rollback{entries, exchange};
  for (auto &entry : entries) {
    exchange(entry);
    ++rollback.changed;
  }
  inspect();
}
} // namespace SkyBuild::Navmesh
