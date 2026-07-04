#pragma once
#include <defines.h>
#include <math/bs_hierpos.h> // bs_math::Vec2, bs_math::HierPos2

struct game_state;

// =====================================================================================
// View transform: cosmetic zoom-space compression + the screen<->true-world<->render-space
// conversions every galaxy-view system shares.
//
// At extreme zoom-out the renderer shrinks camera-relative positions toward the origin so
// distant systems still fit on screen (and f32 offsets stay small). Stars, ships, voronoi
// cells and hover FX ALL must use the SAME curve, or they drift apart sub-pixel as the view
// approaches the galaxy floor. This module owns the single definition every layer calls, plus
// the paired forward/inverse transforms between screen pixels, true (simulation) world space,
// and compressed render space.
// =====================================================================================

// ---- Pure functions of zoom only (no game_state; trivially unit-testable) --------------

// 1.0 at normal zoom, smoothstepping down toward 0.15 at the galaxy-floor zoom.
f32 compression_factor(f32 zoom);

// Compress a camera-relative position toward the origin. Purely cosmetic: true galaxy
// positions are unchanged; only the on-screen offset is scaled by compression_factor(zoom).
bs_math::Vec2 cosmetic_compress(bs_math::Vec2 pos, f32 zoom);

// Public wrapper so other translation units (RTS controls) can query the compression factor
// to scale free-camera pan speed by the on-screen scale (zoom*comp).
f32 game_compression_factor(f32 zoom);

// Arena-look weight for the current zoom: 1.0 = full arena/gameplay look, 0.0 = full
// galaxy-map look, smoothly cross-fading across the arena<->map zoom band. Render sites read
// this (via s->view_arena_w) instead of branching on the discrete GameMode.
f32 view_arena_weight(f32 zoom);

// ---- game_state-dependent transforms (screen <-> true world <-> render space) ----------
// In the arena look these are near-identity; under the unified floating-origin path the
// renderer draws entities at comp*(world - camera_hierpos), and these invert / apply that.

// Screen pixel -> true (simulation) world point currently under it.
bs_math::Vec2 game_screen_to_true_world(const game_state* s, bs_math::Vec2 screen_px);
// True world position -> compressed render-space position the renderer draws entities at.
bs_math::Vec2 game_true_world_to_render(const game_state* s, bs_math::Vec2 world);
// Absolute (true-world) point currently at the screen center.
bs_math::Vec2 game_camera_center(const game_state* s);
// HierPos2 (precision-safe) form of game_screen_to_true_world.
bs_math::HierPos2 game_screen_to_true_hierpos(const game_state* s, bs_math::Vec2 screen_px);
// HierPos2 (precision-safe) form of game_true_world_to_render.
bs_math::Vec2 render_from_hierpos(const game_state* s, const bs_math::HierPos2* world);
// HierPos2 form of game_camera_center.
bs_math::HierPos2 game_camera_center_hierpos(const game_state* s);
