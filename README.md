# SkyBuild

An SKSE plugin for building and terrain editing in Skyrim Special Edition.

SkyBuild includes grid snapping, free placement, terrain sculpting, runtime
navmesh generation, custom map markers, and connections between usable objects.
Building packs define the available pieces, placement rules, and material costs.

## Building

Requires Visual Studio 2022 with the C++ workload, CMake, Git, and vcpkg.
Set `VCPKG_ROOT` to your vcpkg installation.

```powershell
git clone --recurse-submodules https://github.com/Sokco/SkyBuild.git
cd SkyBuild
cmake --preset build-release-msvc -DDEPLOY_TO_MODS=OFF
cmake --build --preset release-msvc
```

The DLL is written to `build/msvc/Release/SkyBuild.dll`.
Install it to `Data/SKSE/Plugins` alongside the assets from the SkyBuild release.
The ESP, compiled Papyrus scripts, models, and building packs are distributed
with the mod; building this repository alone does not produce a complete install.

## Runtime dependencies

- SKSE64 and Address Library for the installed Skyrim runtime.
- Microsoft Visual C++ x64 Redistributable.
- The SkyBuild release assets.

## Source layout

- `src/building`: placement, snapping, previews, and material costs.
- `src/terrain`: density editing, meshing, collision, grass, and brush rendering.
- `src/navmesh`: Recast generation and native navigation updates.
- `src/ui`: menus, pack editing, HUD, and thumbnails.
- `src/activators`, `src/containers`, `src/map-markers`: object interactions.
- `scripts/Source`: Papyrus interfaces and the container weapon rack script.

The main dependencies are CommonLibSSE-NG, Dear ImGui, ImAnim, MC33++, Recast,
and Nifly. Third-party licenses are retained with their sources or copied into
the build's `SkyBuild-licenses` directory.
