#pragma once
#include "navmesh/cuts/CutReferenceGraph.h"
#include <memory>
#include <type_traits>
namespace SkyBuild::Navmesh {
static_assert(std::is_nothrow_swappable_v<CutReferenceGraph>);
// Owned reference-graph transaction. Exact before-images are required because
// clone-to-canonical substitution is many-to-one and cannot be inverted.
// Native adapters must supply a verified capture/lifetime boundary separately;
// equal graph values do not certify epochs or exclude ABA. No engine writes.
class CutReferenceConversion {
public:
  enum class State { Prepared, Published, Restored };
  [[nodiscard]] static std::unique_ptr<CutReferenceConversion>
  Prepare(const CutReferenceGraph &source,
          const std::map<std::uint64_t, std::uint64_t> &mapping,
          std::string &error,
          const std::map<std::uint64_t, std::uint64_t> &infoMapping = {}) {
    auto transaction =
        std::unique_ptr<CutReferenceConversion>(new CutReferenceConversion);
    if (!RemapCutReferences(source, mapping, transaction->after, error,
                            infoMapping))
      return nullptr;
    transaction->before = source;
    return transaction;
  }
  bool Commit(CutReferenceGraph &actual, std::string &error) {
    if (state != State::Prepared || actual != before) {
      error = "Cut reference source changed or transaction already consumed";
      return false;
    }
    // Allocate before changing any destination. Eventual native adapters must
    // preallocate their own slot journal within a verified engine boundary.
    auto replacement = after;
    using std::swap;
    swap(actual, replacement);
    state = State::Published;
    error.clear();
    return true;
  }
  bool Restore(CutReferenceGraph &actual, std::string &error) {
    if (state != State::Published || actual != after) {
      error =
          "Published cut references changed or restoration already consumed";
      return false;
    }
    auto replacement = before;
    using std::swap;
    swap(actual, replacement);
    state = State::Restored;
    error.clear();
    return true;
  }
  [[nodiscard]] const CutReferenceGraph &Before() const { return before; }
  [[nodiscard]] const CutReferenceGraph &After() const { return after; }
  [[nodiscard]] State CurrentState() const { return state; }

private:
  CutReferenceConversion() = default;
  CutReferenceGraph before, after;
  State state{State::Prepared};
};
} // namespace SkyBuild::Navmesh
