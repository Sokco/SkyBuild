#include "navmesh/native/NativeSearchAudit.h"
#include <array>
#include <atomic>
#include <utility>

namespace SkyBuild::Navmesh::NativeSearchAudit {
namespace {
struct Binding {
  std::uint64_t table, method;
};
// The initialized 1.6.1170 registry (90396) contains these 31 processors.
// IDs identify engine objects/functions, never executable byte prefixes.
constexpr std::array bindings{
    Binding{246004, 91699}, Binding{214778, 50507}, Binding{204675, 31487},
    Binding{214496, 50151}, Binding{212970, 48275}, Binding{204673, 31485},
    Binding{214494, 50149}, Binding{204689, 31503}, Binding{204669, 31478},
    Binding{214323, 49825}, Binding{204677, 31492}, Binding{214116, 49584},
    Binding{204683, 31497}, Binding{204700, 31547}, Binding{204446, 31181},
    Binding{213874, 49299}, Binding{204704, 31555}, Binding{212074, 47074},
    Binding{212858, 48156}, Binding{213832, 49284}, Binding{213221, 48605},
    Binding{213182, 48574}, Binding{204685, 31499}, Binding{204693, 31507},
    Binding{213005, 48290}, Binding{204706, 31559}, Binding{204691, 31505},
    Binding{204702, 31551}, Binding{204687, 31501}, Binding{204671, 31483},
    Binding{214069, 49562}};
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
    const REL::Relocation<std::uintptr_t> table{REL::ID(bindings[i].table)};
    const REL::Relocation<std::uintptr_t> method{REL::ID(bindings[i].method)};
    slots[i] = table.address() + sizeof(std::uintptr_t);
    if (*reinterpret_cast<const std::uintptr_t *>(slots[i]) !=
        method.address()) {
      spdlog::warn("Navmesh processor audit unavailable: dispatch slot {} was "
                   "replaced. No audit hooks installed.",
                   bindings[i].table);
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
