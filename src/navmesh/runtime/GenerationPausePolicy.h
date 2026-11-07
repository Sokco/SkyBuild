#pragma once
namespace SkyBuild::Navmesh {
enum class NavigationCommand { Generate, ApplyCache };
constexpr bool RequiresGameplayPause(NavigationCommand command) {
  return command == NavigationCommand::Generate;
}
struct GenerationWork {
  bool command{}, capture{}, baseline{}, geometryWorker{}, nativePreparation{},
      navigationWaiting{}, requested{};
  [[nodiscard]] constexpr bool Pending() const {
    return command || capture || baseline || geometryWorker ||
           nativePreparation || navigationWaiting || requested;
  }
};
constexpr bool ReleaseGenerationPause(bool owned, bool sceneLoaded,
                                      const GenerationWork &work) {
  return owned && (!sceneLoaded || !work.Pending());
}
} // namespace SkyBuild::Navmesh
