// Adapted from ImAnim 1.0.0 by Soufiane KHIAT (MIT; see LICENSE).
// im_anim.cpp: ease_in_cubic and transform_out(ease_cubic, t).
#pragma once
namespace ImAnim {
constexpr float OutCubic(float t) {
  const float remaining = 1.F - t;
  return 1.F - remaining * remaining * remaining;
}
}
