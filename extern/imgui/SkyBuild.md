# Private Dear ImGui build

Pinned source: https://github.com/ocornut/imgui/tree/v1.91.9b (MIT).

SkyBuild statically links its own context, Win32 backend, DX11 backend and ImAnim.
It does not load, call or share an ImGui context with SKSE Menu Framework.

Local adaptations:
- imconfig.h uses native DX11 SRV pointers as ImTextureID consistently in all TUs.
- SKYBUILD_RELATIVE_MOUSE skips the Win32 backend's OS mouse polling call. Skyrim
  supplies raw relative motion and may recenter the OS cursor; NativeUI queues
  that motion and maintains an independent cursor. Keyboard, text and buttons
  also come exclusively from Skyrim's event stream to avoid duplicate events.

The original backend still supplies frame timing, client size, key translation,
clipboard/IME integration, cursor policy and gamepad polling. Window focus loss
is chained through the original WndProc and clears SkyBuild's queued input.
