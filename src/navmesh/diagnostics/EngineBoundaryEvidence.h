#pragma once
#include <cstdint>
namespace SkyBuild::Navmesh {
struct EngineBoundaryEvidence {
  bool expectedManager{}, pauseReady{}, publicationWriteOwned{},
      workerAuditAvailable{};
  std::uint32_t submitted{}, completed{}, activeWorkers{}, activeLoads{};
  std::uint32_t preparedWork{};
  bool preparedWorkObserved{};
  bool loadAuditAvailable{}, observationsStable{};
  // Evidence classification only, not a native lifetime/exclusion certificate.
  bool Candidate() const {
    return expectedManager && pauseReady && publicationWriteOwned &&
           workerAuditAvailable && loadAuditAvailable && observationsStable &&
           preparedWorkObserved && !preparedWork && !submitted && !completed &&
           !activeWorkers && !activeLoads;
  }
};
} // namespace SkyBuild::Navmesh
