#pragma once
#include <defines.h>
#include <math/bs_hierpos.h> // bs_math::Vec2, bs_math::HierPos2

struct game_state;

// =====================================================================================
// View transform: the screen<->true-world<->render-space conversions every galaxy-view
// system shares.
//
// Positions render linearly at every zoom (render = world - camera_hierpos, then camera2d
// applies zoom). Stars, ships, voronoi cells and hover FX all go through the SAME transforms
// here so they never drift apart. This module owns the paired forward/inverse transforms
// between screen pixels, true (simulation) world space, and render space.
// =====================================================================================

// ---- Pure functions of zoom only (no game_state; trivially unit-testable) --------------

// Progressive zoom-out speed gain: each wheel notch covers MORE zoom the further out the player
// already is (0 = constant speed, higher = faster far-out zoom). This lets the wheel pull back far
// enough to frame the whole galaxy disc. Editor-tunable (see the "Zoom-out speed gain" slider).
extern f32 g_zoom_out_speed_gain;

// Arena-look weight for the current zoom: 1.0 = full arena/gameplay look, 0.0 = full
// galaxy-map look, smoothly cross-fading across the arena<->map zoom band. Render sites read
// this (via s->view_arena_w) instead of branching on the discrete GameMode.
f32 view_arena_weight(f32 zoom);

// ---- game_state-dependent transforms (screen <-> true world <-> render space) ----------
// The renderer draws entities at (world - camera_hierpos); these invert / apply that.

// Screen pixel -> true (simulation) world point currently under it.
bs_math::Vec2 game_screen_to_true_world(const game_state* s, bs_math::Vec2 screen_px);
// True world position -> render-space position the renderer draws entities at.
bs_math::Vec2 game_true_world_to_render(const game_state* s, bs_math::Vec2 world);
// Absolute (true-world) point currently at the screen center.
bs_math::Vec2 game_camera_center(const game_state* s);
// HierPos2 (precision-safe) form of game_screen_to_true_world.
bs_math::HierPos2 game_screen_to_true_hierpos(const game_state* s, bs_math::Vec2 screen_px);
// HierPos2 (precision-safe) form of game_true_world_to_render.
bs_math::Vec2 render_from_hierpos(const game_state* s, const bs_math::HierPos2* world);
// HierPos2 form of game_camera_center.
bs_math::HierPos2 game_camera_center_hierpos(const game_state* s);
