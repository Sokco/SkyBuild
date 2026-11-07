#pragma once
namespace SkyBuild::Navmesh {
template <class Array, class Valid>
void CopyEligibleMeshCandidates(Array &view, const Array &live, Valid valid) {
  for (const auto &mesh : live)
    if (valid(mesh))
      view.push_back(mesh);
}
template <class Array, class Valid, class Rejected>
void FilterObstacleCandidates(Array &scratch, Valid valid, Rejected rejected) {
  for (auto it = scratch.begin(); it != scratch.end();) {
    if (valid(*it))
      ++it;
    else {
      rejected(*it);
      it = scratch.erase(it);
    }
  }
}
} // namespace SkyBuild::Navmesh
