#pragma once
#include "navmesh/generation/AuthoredGenerationBasis.h"
#include "navmesh/native/WriterObservation.h"
namespace SkyBuild::Navmesh::NativeAuthoredBasis {
// Install before plugin records load, not at DataLoaded. Captures owned data
// after successful NavMesh::Load; no navmesh/registry/cut mutations.
bool Initialize();
bool Available() noexcept;
std::shared_ptr<const AuthoredBasisRecord> Find(std::uint32_t id);
AuthoredBasisSnapshot Snapshot(const std::set<std::uint32_t> &required);
bool Matches(const AuthoredBasisSnapshot &snapshot);
// Generation reads winning records only. Live cut snapshots are deliberately
// absent from this interface. The caller retains/checks the whole-cohort lease.
AuthoredGenerationBasis
PrepareGeneration(const std::set<std::uint32_t> &required,
                  const std::set<std::uint32_t> &selected,
                  AuthoredBasisSnapshot &lease,
                  const std::map<std::uint32_t, AuthoredMesh> &observed);
WriterObservation LoadObservation() noexcept;
std::string Status();
} // namespace SkyBuild::Navmesh::NativeAuthoredBasis
