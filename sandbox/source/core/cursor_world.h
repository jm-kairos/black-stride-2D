#pragma once
#include <math/bs_hierpos.h>

struct game_state;

// Mouse cursor -> world-space helpers. mouse_world returns the render-space point (legacy
// path where render space == true world); mouse_true_world and mouse_true_hierpos invert the
// compression / camera_hierpos transform so callers hit-testing against true-world entity
// positions (edit picking, piloting/aiming) stay correct under the unified coordinate path.
bs_math::Vec2 mouse_world(const game_state* s);
bs_math::Vec2 mouse_true_world(const game_state* s);
bs_math::HierPos2 mouse_true_hierpos(const game_state* s);
