# ImAnim integration

Source: https://github.com/soufianekhiat/ImAnim, version 1.0.0, MIT.

The complete upstream tween engine is compiled into SkyBuild's private ImGui
library. NativeUI calls iam_update_begin_frame once per frame and collects stale
channels every 60 frames. Sidebar selection uses an out-cubic crossfade; HUD
tracks retain their small independently owned easing helper.

Compatibility adjustment: ImGui 1.91.9b already exposes ImGuiStoragePair globally,
so the pre-1.92 storage-pair alias in im_anim.cpp uses that global type. The
pre-1.92 font compatibility path remains enabled. Easing.h is the previously
extracted out-cubic helper used by the HUD's two lightweight tracks.
