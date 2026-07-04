#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
#include "ship.h"
// Forward declaration: game_state is defined in game.h.
struct game_state;

inline b8 ship_inside_world_box(const Ship* ship, bs_math::HierPos2 min_world, bs_math::HierPos2 max_world) {
    if (!ship) return FALSE;
    // Work in a min-relative frame so the test stays precise far from the galaxy origin.
    bs_math::Vec2 p  = bs_math::hierpos_diff(&ship->origin, &min_world, bs_math::BS_HIERPOS_CELL_SIZE);
    bs_math::Vec2 mx = bs_math::hierpos_diff(&max_world,    &min_world, bs_math::BS_HIERPOS_CELL_SIZE);
    f32 lo_x = 0.0f, hi_x = mx.x; if (lo_x > hi_x) { f32 t = lo_x; lo_x = hi_x; hi_x = t; }
    f32 lo_y = 0.0f, hi_y = mx.y; if (lo_y > hi_y) { f32 t = lo_y; lo_y = hi_y; hi_y = t; }
    return (p.x >= lo_x && p.x <= hi_x && p.y >= lo_y && p.y <= hi_y) ? TRUE : FALSE;
}
// =====================================================================================
// RTS controls module.
//
// Handles fleet selection, orders (move/attack), control-group-like single-ship piloting,
// and the free-camera RTS UX. Orders are now executed by the Fleet class; this module
// only owns the input state + transition logic (hover, box selection, pilot/auto-pilot).
// =====================================================================================
struct RtsSelectionBox {
    b8 active;
    f32 start_x;  // screen-space origin
    f32 start_y;
    f32 end_x;    // current screen-space drag point
    f32 end_y;
};

class RtsControls {
public:
    RtsControls();
    explicit RtsControls(game_state* s);
    void update(f32 dt);
    void draw();
    // Index of the fleet member the player is manually piloting (default 0 = flagship).
    // -1 means "none", but the rest of the code clamps it to the flagship.
    i32 piloted_index() const { return m_piloted_idx; }
private:
    game_state* m_state;
    i32 m_hovered_ship_idx;    // fleet member under the cursor (or -1)
    i32 m_hovered_enemy_idx;   // combat entity index under the cursor (or -1)
    f32 m_dash_angle;
    RtsSelectionBox m_box;
    // Manual piloting: which fleet member is currently attached to the camera.
    i32 m_piloted_idx;
    // Camera transition from free-camera to piloted ship.
    b8 m_camera_transitioning;
    f32 m_camera_transition_t;
    bs_math::HierPos2 m_camera_transition_from;
    // Free camera movement state.
    bs_math::Vec2 m_free_camera_vel;
    bs_math::HierPos2 m_free_camera_drag_target;
    bs_math::Vec2 m_free_camera_drag_start_mouse;
    bs_math::HierPos2 m_free_camera_drag_start_pos;
    b8 m_free_camera_dragging;
    // Helpers.
    void clear_fleet_orders();
    void draw_dashed_circle(bs_math::Vec2 center, f32 radius, u32 segments,
                            f32 fill_ratio, f32 rotation, f32 thickness,
                            bs_color color, u32 layer);
    void draw_rect_from_screen_box(const RtsSelectionBox& box, f32 thickness,
                                   bs_color color, u32 layer);
    void draw_selection_rect(bs_math::Vec2 center, f32 radius, f32 thickness, bs_color color, u32 layer);
    void draw_move_marker(bs_math::Vec2 target, f32 thickness, bs_color color, u32 layer);
    void draw_attack_marker(bs_math::Vec2 center, f32 radius, f32 thickness, bs_color color, u32 layer);
};
