#pragma once
#include "navmesh/cuts/CutReferenceGraph.h"
namespace SkyBuild::Navmesh {
// Read-only adapter for a caller-retained cohort at a native transaction
// boundary. It includes every undo input, current owner and saved owner,
// regardless of shared undo status. Tokens never escape into save data.
// Complete describes enumeration of this cohort, not exclusion of all native
// writers or borrowers outside it. No native slot addresses or write API
// escape.
enum class CutMembershipCapture { CohortOnly, RetainExternalTokens };
// RetainExternalTokens records outside memberships as opaque identities. It
// never dereferences or authorizes changing those outside navigation infos.
// retainedBindings supplies caller-retained infos for a native-cleared
// backlink; the info's mesh and ID must still agree. Capture never repairs the
// backlink.
[[nodiscard]] CutReferenceGraph CaptureNativeCutReferences(
    const RE::BSTArray<RE::BSTSmartPointer<RE::BSNavmesh>> &cohort,
    const std::vector<RE::NiPointer<RE::NiRefObject>> &retainedInputs = {},
    CutMembershipCapture membershipScope = CutMembershipCapture::CohortOnly,
    const std::map<const RE::BSNavmesh *, const RE::BSNavmeshInfo *>
        *retainedBindings = nullptr);
} // namespace SkyBuild::Navmesh
