#pragma once
#include <defines.h>
#include <math/math_utils.h>

// Pure 2D geometry helpers (no game_state). Shared by editor picking and combat hit-testing.

// Even-odd ray-cast point-in-polygon test. `verts` is a closed polygon in world space.
b8 point_in_polygon(bs_math::Vec2 p, const bs_math::Vec2* verts, i32 n);

// Distance from a point to a line segment [a, b].
f32 point_to_segment(bs_math::Vec2 p, bs_math::Vec2 a, bs_math::Vec2 b);
