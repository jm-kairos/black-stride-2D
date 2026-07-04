#include "core/view_transform.h"
#include "game.h"
#include <renderer/camera2d.h>

using namespace bs_math;

// Single source of truth for the galaxy-view compression curve. Previously copy-pasted into
// game.cpp, mapped_system_layer.cpp, voronoi_galaxy.cpp and voronoi_cell_hover_effect.cpp --
// the voronoi copies had silently dropped the smoothstep line, so cells compressed on a linear
// ramp while stars/ships used smoothstep, drifting apart at extreme zoom-out. Unified here.
f32 compression_factor(f32 zoom) {
    const f32 threshold = 0.02f;
    const f32 min_zoom  = 0.000004f;
    if (zoom >= threshold) return 1.0f;
    f32 t = (zoom - min_zoom) / (threshold - min_zoom);
    t = t * t * (3.0f - 2.0f * t);         // smoothstep
    return 0.15f + 0.85f * t;
}

Vec2 cosmetic_compress(Vec2 pos, f32 zoom) {
    return vec2_scale(pos, compression_factor(zoom));
}

// Public wrapper so other translation units (RTS controls) can query the compression factor.
f32 game_compression_factor(f32 zoom) {
    return compression_factor(zoom);
}

// STEP 2: arena-look weight for the current zoom. 1.0 = full arena/gameplay look, 0.0 = full
// galaxy-map look, smoothly cross-fading across [VIEW_MAP_ZOOM, VIEW_ARENA_ZOOM]. Every render
// site reads this (via s->view_arena_w) instead of branching on the discrete GameMode. The band
// straddles the old hard mode-flip point (ZOOM_MIN = 0.08) so the two looks cross-fade.
static const f32 VIEW_MAP_ZOOM   = 0.05f;  // at/below this zoom: fully galaxy-map look
static const f32 VIEW_ARENA_ZOOM = 0.14f;  // at/above this zoom: fully arena look
f32 view_arena_weight(f32 zoom) {
    if (zoom <= VIEW_MAP_ZOOM)   return 0.0f;
    if (zoom >= VIEW_ARENA_ZOOM) return 1.0f;
    f32 t = (zoom - VIEW_MAP_ZOOM) / (VIEW_ARENA_ZOOM - VIEW_MAP_ZOOM);
    return t * t * (3.0f - 2.0f * t); // smoothstep
}

// True (galaxy-space, compression-corrected) world point currently displayed under a screen
// pixel. The renderer draws entities at comp*(P - camera_hierpos), so we invert that to recover
// the real point. Used to keep the point under the cursor fixed while zooming, and by the RTS
// controls to hit-test/order in the same true world space that ship.origin lives in.
bs_math::Vec2 game_screen_to_true_world(const game_state* s, bs_math::Vec2 screen_px) {
    bs_math::Vec2 R = camera2d_screen_to_world(&s->camera_state.camera, s->fb_width, s->fb_height, screen_px);
    f32 comp = compression_factor(s->camera_state.camera.zoom);
    if (comp < 1e-6f) comp = 1e-6f;
    bs_math::Vec2 chp = hierpos_to_vec2(&s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    return vec2_add(chp, vec2_scale(R, 1.0f / comp));
}

// Forward transform: true (simulation) world position -> compressed render-space position the
// renderer draws entities at. Used by the RTS overlay so selection rings / order markers line up
// with the compressed ship sprites.
bs_math::Vec2 game_true_world_to_render(const game_state* s, bs_math::Vec2 world) {
    f32 comp = compression_factor(s->camera_state.camera.zoom);
    bs_math::Vec2 chp = hierpos_to_vec2(&s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    return vec2_scale(vec2_sub(world, chp), comp);
}

// Absolute (true-world) point currently at the screen center. The renderer subtracts
// camera_hierpos and camera2d subtracts camera.position, so the true center is
// camera_hierpos + camera.position (camera.position being a small render-space residual). Use this
// whenever storing a true-world camera target (e.g. free_camera_pos) so it stays valid.
bs_math::Vec2 game_camera_center(const game_state* s) {
    return vec2_add(hierpos_to_vec2(&s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE), s->camera_state.camera.position);
}

// HierPos2 (precision-safe) form of game_screen_to_true_world.
bs_math::HierPos2 game_screen_to_true_hierpos(const game_state* s, bs_math::Vec2 screen_px) {
    bs_math::Vec2 R = camera2d_screen_to_world(&s->camera_state.camera, s->fb_width, s->fb_height, screen_px);
    f32 comp = compression_factor(s->camera_state.camera.zoom);
    if (comp < 1e-6f) comp = 1e-6f;
    return hierpos_add_vec2(&s->camera_state.camera_hierpos, vec2_scale(R, 1.0f / comp));
}

// HierPos2 (precision-safe) form of game_true_world_to_render: subtracts the camera cell in
// integer space before compressing, so it never loses precision far from the galaxy origin.
bs_math::Vec2 render_from_hierpos(const game_state* s, const bs_math::HierPos2* world) {
    f32 comp = compression_factor(s->camera_state.camera.zoom);
    return vec2_scale(hierpos_diff(world, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE), comp);
}

// HierPos2 form of game_camera_center.
bs_math::HierPos2 game_camera_center_hierpos(const game_state* s) {
    return hierpos_add_vec2(&s->camera_state.camera_hierpos, s->camera_state.camera.position);
}

