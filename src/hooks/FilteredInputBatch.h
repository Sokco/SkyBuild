#pragma once
#include <utility>
#include <vector>

namespace SkyBuild::Hooks {
// Skyrim owns and may reuse these events. Filter only the dispatch list, never
// their device, key code or action. Restore links before returning to Skyrim.
template <class Event> class FilteredInputBatch {
public:
  template <class Consume> FilteredInputBatch(Event *first, Consume consume) {
    try {
      Event *last = nullptr;
      for (auto *event = first; event;) {
        auto *next = event->next;
        if (!consume(event)) {
          if (last)
            Link(last, event);
          else
            head = event;
          last = event;
        }
        event = next;
      }
      if (last)
        Link(last, nullptr);
    } catch (...) {
      Restore();
      throw;
    }
  }
  ~FilteredInputBatch() { Restore(); }
  FilteredInputBatch(const FilteredInputBatch &) = delete;
  FilteredInputBatch &operator=(const FilteredInputBatch &) = delete;
  Event *head{};

private:
  void Restore() {
    for (auto it = links.rbegin(); it != links.rend(); ++it)
      it->first->next = it->second;
  }
  void Link(Event *event, Event *next) {
    if (event->next != next) {
      links.emplace_back(event, event->next);
      event->next = next;
    }
  }
  std::vector<std::pair<Event *, Event *>> links;
};
} // namespace SkyBuild::Hooks
