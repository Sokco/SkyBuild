#pragma once

#include <cstddef>

namespace SkyBuild::UI::ThumbnailScenePolicy {
enum class CloseAction { kWait, kEnd, kLeaveIntact };

inline CloseAction GetCloseAction(bool a_ready, std::size_t a_waitFrames,
                                  std::size_t a_limit) {
  if (a_ready) {
    return CloseAction::kEnd;
  }
  return a_waitFrames < a_limit ? CloseAction::kWait
                                : CloseAction::kLeaveIntact;
}

template <class Models>
bool IsReady(bool a_loadPending, const Models &a_models) {
  if (a_loadPending) {
    return false;
  }
  for (const auto &model : a_models) {
    if (!model.spModel || model.spModel->worldBound.radius <= 0.0F) {
      return false;
    }
  }
  return true;
}
} // namespace SkyBuild::UI::ThumbnailScenePolicy
