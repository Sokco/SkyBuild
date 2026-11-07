#pragma once
#include "navmesh/runtime/PauseProbe.h"
#include <string>

namespace SkyBuild::Navmesh::NativeSynchronization {
void Initialize();
void SetSceneLoaded(bool loaded);
void Update();
void CancelPending();
[[nodiscard]] bool PendingAction();
[[nodiscard]] bool RetireBeforeStreaming();
void DeferStreamingPathRefresh();
void RequestReplacement();
void RequestOriginalRestore();
void RequestCommittedRestore();
[[nodiscard]] bool RequestBaselineCapture();
[[nodiscard]] bool Available();
[[nodiscard]] PauseProbeState ProbeState();
[[nodiscard]] const char *StatusText();
} // namespace SkyBuild::Navmesh::NativeSynchronization
