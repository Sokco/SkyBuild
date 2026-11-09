#include "navmesh/native/NativeSearchAudit.h"
#include <array>
#include <atomic>
#include <utility>

namespace SkyBuild::Navmesh::NativeSearchAudit {
namespace {
struct Binding {
  REL::RelocationID table, method;
};
// The initialized 1.6.1170 registry (90396) contains these 31 processors.
// IDs identify engine objects/functions, never executable byte prefixes.
constexpr std::array bindings{
    Binding{REL::RelocationID(294890, 246004), REL::RelocationID(89279, 91699)}, Binding{REL::RelocationID(267607, 214778), REL::RelocationID(49457, 50507)}, Binding{REL::RelocationID(255219, 204675), REL::RelocationID(30655, 31487)},
    Binding{REL::RelocationID(267392, 214496), REL::RelocationID(49265, 50151)}, Binding{REL::RelocationID(266318, 212970), REL::RelocationID(47264, 48275)}, Binding{REL::RelocationID(255218, 204673), REL::RelocationID(30656, 31485)},
    Binding{REL::RelocationID(267391, 214494), REL::RelocationID(49266, 50149)}, Binding{REL::RelocationID(255259, 204689), REL::RelocationID(30703, 31503)}, Binding{REL::RelocationID(255210, 204669), REL::RelocationID(30646, 31478)},
    Binding{REL::RelocationID(267185, 214323), REL::RelocationID(48908, 49825)}, Binding{REL::RelocationID(255225, 204677), REL::RelocationID(30663, 31492)}, Binding{REL::RelocationID(267072, 214116), REL::RelocationID(48664, 49584)},
    Binding{REL::RelocationID(255256, 204683), REL::RelocationID(30699, 31497)}, Binding{REL::RelocationID(255265, 204700), REL::RelocationID(30713, 31547)}, Binding{REL::RelocationID(255091, 204446), REL::RelocationID(30374, 31181)},
    Binding{REL::RelocationID(266858, 213874), REL::RelocationID(48328, 49299)}, Binding{REL::RelocationID(255273, 204704), REL::RelocationID(30721, 31555)}, Binding{REL::RelocationID(265574, 212074), REL::RelocationID(45837, 47074)},
    Binding{REL::RelocationID(266189, 212858), REL::RelocationID(47036, 48156)}, Binding{REL::RelocationID(266846, 213832), REL::RelocationID(48329, 49284)}, Binding{REL::RelocationID(266478, 213221), REL::RelocationID(47548, 48605)},
    Binding{REL::RelocationID(266469, 213182), REL::RelocationID(47547, 48574)}, Binding{REL::RelocationID(255257, 204685), REL::RelocationID(30700, 31499)}, Binding{REL::RelocationID(255261, 204693), REL::RelocationID(30704, 31507)},
    Binding{REL::RelocationID(266327, 213005), REL::RelocationID(47265, 48290)}, Binding{REL::RelocationID(255277, 204706), REL::RelocationID(30725, 31559)}, Binding{REL::RelocationID(255260, 204691), REL::RelocationID(30702, 31505)},
    Binding{REL::RelocationID(255269, 204702), REL::RelocationID(30717, 31551)}, Binding{REL::RelocationID(255258, 204687), REL::RelocationID(30701, 31501)}, Binding{REL::RelocationID(255217, 204671), REL::RelocationID(30654, 31483)},
    Binding{REL::RelocationID(267059, 214069), REL::RelocationID(48663, 49562)}};
// Native dispatcher 90396 passes processor, request smart-pointer address,
// solution smart-pointer address and receives a bool. Leave both opaque
// smart-pointer arguments untouched, including their ownership.
using Dispatch = bool (*)(void *, void *, void *);
std::array<Dispatch, bindings.size()> originals{};
std::atomic_uint32_t active{};
std::atomic_bool available{};
bool initialized{};
static_assert(std::atomic_uint32_t::is_always_lock_free);
thread_local unsigned searchDepth{};

class Call {
public:
  Call() noexcept {
    ++searchDepth;
    active.fetch_add(1);
  }
  ~Call() {
    active.fetch_sub(1);
    --searchDepth;
  }
};
template <std::size_t Index>
bool Intercept(void *processor, void *request, void *solution) {
  const Call call;
  return originals[Index](processor, request, solution);
}
template <std::size_t... Index>
constexpr auto Interceptors(std::index_sequence<Index...>) {
  return std::array<Dispatch, sizeof...(Index)>{&Intercept<Index>...};
}
constexpr auto interceptors =
    Interceptors(std::make_index_sequence<bindings.size()>{});
} // namespace

void Initialize() {
  if (initialized)
    return;
  initialized = true;
  // Preflight every slot before installing anything. A different processor
  // implementation is not silently treated as the inspected native ABI.
  std::array<std::uintptr_t, bindings.size()> slots{};
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const REL::Relocation<std::uintptr_t> table{bindings[i].table};
    const REL::Relocation<std::uintptr_t> method{bindings[i].method};
    slots[i] = table.address() + sizeof(std::uintptr_t);
    if (*reinterpret_cast<const std::uintptr_t *>(slots[i]) !=
        method.address()) {
      spdlog::warn("Navmesh processor audit unavailable: dispatch slot {} was "
                   "replaced. No audit hooks installed.",
                   bindings[i].table.id());
      return;
    }
    originals[i] = reinterpret_cast<Dispatch>(method.address());
  }
  for (std::size_t i = 0; i < bindings.size(); ++i)
    REL::safe_write(slots[i],
                    reinterpret_cast<std::uintptr_t>(interceptors[i]));
  available.store(true);
  spdlog::info("Installed native search guards on {} request processors; "
               "search admission remains controlled by Skyrim",
               bindings.size());
}
Report Snapshot() { return {active.load(), available.load()}; }
bool InSearchOnCurrentThread() { return searchDepth != 0; }
} // namespace SkyBuild::Navmesh::NativeSearchAudit
