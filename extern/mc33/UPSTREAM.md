# MC33++ 5.4

Author: David E. Vega M.
Source: https://github.com/dvega68/MC33_cpp_library
Pinned commit: 252dabd7ea6dea3d0529e66b047adbe878caf2fa
License: MIT; see LICENSE.txt. Upstream source/header/table files are unmodified.

Algorithm: Vega, Abache, Coll, *A Fast and Memory-Saving Marching Cubes 33
Implementation with the Correct Interior Test*, JCGT 8(3), 2019.
Paper: https://jcgt.org/published/0008/03/01/

SkyBuild compiles source/libMC33++.cpp as a separate static library without
the plugin PCH. MC33_DOUBLE_PRECISION=1 is supplied consistently to the library
and its consumer. The upstream MC33_NORMAL_NEG=1 option matches SkyBuild's
solid-negative field and outward-facing triangle convention. The adapter supplies the raw sampled density field and
converts the resulting grid coordinates to world coordinates. Case selection,
triangulation, exact-zero handling and ambiguity decisions remain upstream.
