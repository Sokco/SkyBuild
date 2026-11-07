#include "navmesh/diagnostics/Diagnostics.h"
namespace SkyBuild::Navmesh::Diagnostics {
void SetEnabled(bool value) {
  if (enabled.exchange(value) == value)
    return;
  spdlog::set_level(value ? spdlog::level::debug : spdlog::level::info);
}
} // namespace SkyBuild::Navmesh::Diagnostics
