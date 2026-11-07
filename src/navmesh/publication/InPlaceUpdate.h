#pragma once

#include <cstdint>
#include <memory>

namespace SkyBuild::Navmesh {
// Engine-specific implementations own native geometry, grid, bounds, portals,
// and all undo data here. Their allocation must finish before requesting pause.
// Portable generation must never own raw pointers into live engine arrays.
struct PreparedUpdate {
  virtual ~PreparedUpdate() = default;
};

enum class PauseState { kWaiting, kReady, kFailed };

// One acquired pause request, not a global "navigation enabled" toggle.
// Destruction releases exactly this request, preserving other owners' pauses.
// A ready lease must protect both path readers AND navcut writers, and must
// pin every affected mesh and link until its destructor releases the barrier.
class UpdateLease {
public:
  virtual ~UpdateLease() = default;
  [[nodiscard]] virtual PauseState Poll() noexcept = 0;
  [[nodiscard]] virtual bool StillCurrent(const PreparedUpdate &) noexcept = 0;
  // Commit must make either the complete edit or no edit. Restore must not
  // allocate/fail; its immutable backup is part of PreparedUpdate.
  [[nodiscard]] virtual bool Commit(PreparedUpdate &) noexcept = 0;
  virtual void Restore(PreparedUpdate &) noexcept = 0;
  [[nodiscard]] virtual bool InvalidatePaths(PreparedUpdate &) noexcept = 0;
};

class UpdateBackend {
public:
  virtual ~UpdateBackend() = default;
  // Return null until this runtime's complete synchronization contract has
  // been verified. An Address Library entry alone is not that contract.
  [[nodiscard]] virtual std::unique_ptr<UpdateLease>
  RequestPause(const PreparedUpdate &) = 0;
};

enum class UpdateState {
  kPrepared,
  kWaiting,
  kApplied,
  kCancelled,
  kTimedOut,
  kStale,
  kUnavailable,
  kFailed
};

// Game-thread transaction. No blocking native flush, sleeps, or worker waits.
// After kApplied, callers retain the native original backup for later explicit
// restoration through a newly prepared transaction and a new lease.
class InPlaceUpdate {
public:
  using Tick = std::uint64_t; // monotonic milliseconds supplied by the caller
  explicit InPlaceUpdate(std::unique_ptr<PreparedUpdate> prepared,
                         Tick timeout = 2000);
  [[nodiscard]] UpdateState Step(UpdateBackend &backend, Tick now);
  void Cancel() noexcept;
  [[nodiscard]] UpdateState State() const noexcept { return state; }
  [[nodiscard]] std::unique_ptr<PreparedUpdate> TakeAppliedBackup() noexcept;

private:
  std::unique_ptr<PreparedUpdate> prepared;
  std::unique_ptr<UpdateLease> lease;
  UpdateState state{UpdateState::kPrepared};
  Tick started{}, timeout{};
  void Finish(UpdateState result) noexcept;
};
} // namespace SkyBuild::Navmesh
