#include "rts_controls.h"
#include "game.h"
#include "core/view_transform.h" // game_screen_to_true_*, game_compression_factor, render_from_hierpos, game_camera_center_hierpos
#include "ship.h"
#include "weapon.h"
#include "fleet.h"
#include <core/input.h>
#include <math/math_utils.h>
#include <renderer/renderer.h>
#include <renderer/bs_imgui.h>
#include <renderer/bs_ui.h>
#include <renderer/camera2d.h>
using namespace bs_math;
// =====================================================================================
// Constants
// =====================================================================================
static constexpr f32 HOVER_DASH_ROTATION_SPEED = 1.0f;  // rad/s
static constexpr f32 HOVER_DASH_SEGMENTS       = 16.0f; // number of dashes around the ring
static constexpr f32 HOVER_DASH_FILL_RATIO     = 0.5f;  // fraction of each dash segment that is visible
static constexpr f32 HOVER_CIRCLE_THICKNESS    = 1.5f;
static constexpr bs_color HOVER_CIRCLE_COLOR   = { 0.35f, 0.85f, 0.95f, 0.85f };
static constexpr u32   HOVER_CIRCLE_LAYER        = 50; // same as LAYER_UI in game.cpp
static constexpr f32 BOX_SELECT_THICKNESS        = 1.0f;
static constexpr bs_color BOX_SELECT_COLOR       = { 0.35f, 0.85f, 0.95f, 0.90f };
static constexpr bs_color SELECTION_RECT_COLOR = { 0.35f, 0.85f, 0.95f, 0.85f };
static constexpr f32 RTS_CLICK_THRESHOLD = 4.0f; // pixels; below this a left release counts as a click
static constexpr f32 RTS_MOVE_MARKER_SIZE      = 12.0f;  // world units
static constexpr f32 RTS_MOVE_MARKER_THICKNESS = 1.5f;
static constexpr bs_color RTS_MOVE_MARKER_COLOR = { 0.35f, 0.85f, 0.95f, 0.90f };
// Free camera movement constants.
static constexpr f32 FREE_CAM_MOVE_SPEED     = 1600.0f;  // WASD constant world-space speed (mechanical)
static constexpr f32 FREE_CAM_SHIFT_MULT     = 3.0f;     // SHIFT speed multiplier for WASD/edge pan
static constexpr f32 FREE_CAM_DRAG_DAMPING   = 8.0f;     // 1/s, drag spring damping
static constexpr f32 FREE_CAM_DRAG_RESPONSIVE= 15.0f;    // 1/s, drag spring response
static constexpr f32 FREE_CAM_EDGE_MARGIN    = 24.0f;    // pixels
static constexpr f32 FREE_CAM_EDGE_SPEED     = 900.0f;   // world units/s
// =====================================================================================
RtsControls::RtsControls()
    : m_state(nullptr)
    , m_hovered_ship_idx(-1)
    , m_hovered_enemy_idx(-1)
    , m_dash_angle(0.0f)
    , m_box{ FALSE, 0.0f, 0.0f, 0.0f, 0.0f }
    , m_piloted_idx(0)
    , m_camera_transitioning(FALSE)
    , m_camera_transition_t(0.0f)
    , m_camera_transition_from(HierPos2{})
    , m_free_camera_vel(Vec2{0.0f, 0.0f})
    , m_free_camera_drag_target(HierPos2{})
    , m_free_camera_drag_start_mouse(Vec2{0.0f, 0.0f})
    , m_free_camera_drag_start_pos(HierPos2{})
    , m_free_camera_dragging(FALSE) {}
RtsControls::RtsControls(game_state* s)
    : m_state(s)
    , m_hovered_ship_idx(-1)
    , m_hovered_enemy_idx(-1)
    , m_dash_angle(0.0f)
    , m_box{ FALSE, 0.0f, 0.0f, 0.0f, 0.0f }
    , m_piloted_idx(0)
    , m_camera_transitioning(FALSE)
    , m_camera_transition_t(0.0f)
    , m_camera_transition_from(HierPos2{})
    , m_free_camera_vel(Vec2{0.0f, 0.0f})
    , m_free_camera_drag_target(HierPos2{})
    , m_free_camera_drag_start_mouse(Vec2{0.0f, 0.0f})
    , m_free_camera_drag_start_pos(HierPos2{})
    , m_free_camera_dragging(FALSE) {}
// =====================================================================================
static Vec2 read_wasd_dir() {
    Vec2 d{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) d.y += 1.0f;
    if (input_is_key_down(KEY_S)) d.y -= 1.0f;
    if (input_is_key_down(KEY_D)) d.x += 1.0f;
    if (input_is_key_down(KEY_A)) d.x -= 1.0f;
    f32 len = vec2_length(d);
    if (len > 0.0f) {
        d.x /= len;
        d.y /= len;
    }
    return d;
}
// =====================================================================================
// =====================================================================================
void RtsControls::draw_rect_from_screen_box(const RtsSelectionBox& box, f32 thickness, bs_color color, u32 layer) {
    if (!m_state) return;
    Vec2 p0 = camera2d_screen_to_world(&m_state->camera_state.camera, m_state->fb_width, m_state->fb_height, Vec2{ box.start_x, box.start_y });
    Vec2 p1 = camera2d_screen_to_world(&m_state->camera_state.camera, m_state->fb_width, m_state->fb_height, Vec2{ box.end_x, box.end_y });
    Vec2 center = Vec2{ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
    Vec2 size = Vec2{ fabsf(p1.x - p0.x), fabsf(p1.y - p0.y) };
    renderer_draw_rect_outline(center, size, thickness, color, layer);
}
// =====================================================================================
void RtsControls::draw_selection_rect(Vec2 center, f32 radius, f32 thickness, bs_color color, u32 layer) {
    if (radius <= 0.0f) return;
    renderer_draw_rect_outline(center, Vec2{ radius * 2.0f, radius * 2.0f }, thickness, color, layer);
}
// =====================================================================================
void RtsControls::clear_fleet_orders() {
    if (!m_state) return;
    for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
        m_state->fleet_state.fleet.at(i).clear_move_target();
        m_state->fleet_state.fleet.at(i).clear_attack_target();
    }
}
// =====================================================================================
static b8 is_point_over_ship(bs_math::HierPos2 world_pos, const Ship* ship) {
    if (!ship) return FALSE;
    f32 dist = vec2_length(hierpos_diff(&world_pos, &ship->origin, BS_HIERPOS_CELL_SIZE));
    return dist <= ship_bounding_radius(ship);
}
// =====================================================================================
void RtsControls::update(f32 dt) {
    if (!m_state) return;
    // RTS input is active in both view modes. In MODE_GLOBAL selection/orders run only while the
    // free camera is detached; in MODE_SYSTEM they run whenever the map view is active. Only these
    // two modes exist, so no early-out is needed here.
    // Ensure pilot index is always valid (defaults to flagship).
    if (m_piloted_idx < 0 || m_piloted_idx >= m_state->fleet_state.fleet.count())
        m_piloted_idx = 0;
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    // Hit-test and issue orders in true (simulation) world space. In system mode this inverts the
    // cosmetic compression so the cursor maps to the same space ship.origin lives in.
    Vec2 mouse_pos = game_screen_to_true_world(m_state, Vec2{ (f32)mx, (f32)my });

    bs_math::HierPos2 mouse_hp = game_screen_to_true_hierpos(m_state, Vec2{ (f32)mx, (f32)my });

    (void)mouse_pos;

    // ---- Hover detection over any fleet ship ------------------------------------------
    m_hovered_ship_idx = -1;
    for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
        if (is_point_over_ship(mouse_hp, &m_state->fleet_state.fleet.at(i).ship)) {
            m_hovered_ship_idx = i;
            break;
        }
    }
    // ---- Hover detection over enemy combat entities -------------------------------------
    m_hovered_enemy_idx = -1;
    for (i32 i = 0; i < m_state->combat_entity_count; ++i) {
        CombatEntity* ce = &m_state->combat_entities[i];
        if (!ce->active || !ce->ship) continue;
        if (ce->faction == m_state->player_ship().faction) continue; // only hostile targets
        if (is_point_over_ship(mouse_hp, ce->ship)) {
            m_hovered_enemy_idx = i;
            break;
        }
    }
    m_dash_angle += HOVER_DASH_ROTATION_SPEED * dt;
    if (m_dash_angle > 2.0f * BS_PI) m_dash_angle -= 2.0f * BS_PI;
    // Detached = the free camera is active (browse / pan / RTS), independent of zoom or render look.
    b8 detached = (m_state->camera_state.free_camera_active && !m_camera_transitioning);
    // ---- Free camera movement (only while detached, and not during a recenter animation) ---
    if (detached && !m_state->galaxy.map_recentering) {
        // ---- Free camera movement (WASD + middle-mouse drag + edge pan) -----------------
        // Scale movement so panning covers a roughly constant fraction of the screen per second at
        // any zoom: the on-screen scale is zoom*comp, so world-space speed uses its inverse. This
        // makes the camera faster the more you zoom out (unbounded above is fine: zoom is clamped to
        // ZOOM_GLOBAL_MIN, so zoom*comp has a floor). Floor at 1 keeps the zoomed-in feel unchanged.
        f32 comp     = game_compression_factor(m_state->camera_state.camera.zoom);
        f32 zoom_mul = clampf(1.0f / (m_state->camera_state.camera.zoom * comp), 1.0f, 1.0e7f);
        Vec2 input_dir = read_wasd_dir();
        b8 fast = input_is_key_down(KEY_LSHIFT) || input_is_key_down(KEY_RSHIFT);
        if (!bs_imgui_wants_mouse()) {
            Vec2 edge_dir{ 0.0f, 0.0f };
            if (mx < (i32)FREE_CAM_EDGE_MARGIN) edge_dir.x -= 1.0f;
            if (mx > (i32)m_state->fb_width - (i32)FREE_CAM_EDGE_MARGIN) edge_dir.x += 1.0f;
            if (my > (i32)m_state->fb_height - (i32)FREE_CAM_EDGE_MARGIN) edge_dir.y -= 1.0f;
            if (my < (i32)FREE_CAM_EDGE_MARGIN) edge_dir.y += 1.0f;
            input_dir = vec2_add(input_dir, edge_dir);
        }
        // Mechanical WASD/edge pan: instant constant velocity while held, instant stop on release
        // (no accel ramp, no friction coast) for a crisp, direct feel. SHIFT multiplies the speed.
        // The middle-mouse drag spring below owns the velocity while dragging, so skip it then.
        if (!m_free_camera_dragging) {
            f32 dir_len = vec2_length(input_dir);
            if (dir_len > 0.0f) {
                Vec2 dir   = vec2_scale(input_dir, 1.0f / dir_len);   // unit -> constant diagonal speed
                f32  speed = FREE_CAM_MOVE_SPEED * zoom_mul * (fast ? FREE_CAM_SHIFT_MULT : 1.0f);
                m_free_camera_vel = vec2_scale(dir, speed);
            } else {
                m_free_camera_vel = Vec2{ 0.0f, 0.0f };
            }
        }
        // Middle-mouse drag.
        b8 middle_down = input_is_button_down(BUTTON_MIDDLE);
        b8 middle_was_down = input_was_button_down(BUTTON_MIDDLE);
        if (!middle_down) {
            m_free_camera_dragging = FALSE;
        }
        if (middle_down && !bs_imgui_wants_mouse()) {
            if (!middle_was_down) {
                m_free_camera_dragging = TRUE;
                m_free_camera_drag_start_pos = m_state->camera_state.free_camera_pos;
                m_free_camera_drag_start_mouse = Vec2{ (f32)mx, (f32)my };
                m_free_camera_drag_target = m_state->camera_state.free_camera_pos;
                m_free_camera_vel = Vec2{ 0.0f, 0.0f };
            } else {
                Vec2 screen_delta = Vec2{ (f32)mx - m_free_camera_drag_start_mouse.x,
                                          (f32)my - m_free_camera_drag_start_mouse.y };
                Vec2 world_delta = Vec2{ screen_delta.x / m_state->camera_state.camera.zoom,
                                         -screen_delta.y / m_state->camera_state.camera.zoom };
                m_free_camera_drag_target = hierpos_add_vec2(&m_free_camera_drag_start_pos, vec2_scale(world_delta, -1.0f));
            }
        }
        // Spring toward drag target.
        if (m_free_camera_dragging) {
            Vec2 error = hierpos_diff(&m_free_camera_drag_target, &m_state->camera_state.free_camera_pos, BS_HIERPOS_CELL_SIZE);
            m_free_camera_vel = vec2_add(m_free_camera_vel,
                                         vec2_scale(error, FREE_CAM_DRAG_RESPONSIVE * zoom_mul * dt));
            m_free_camera_vel = vec2_sub(m_free_camera_vel,
                                         vec2_scale(m_free_camera_vel, FREE_CAM_DRAG_DAMPING * zoom_mul * dt));
        }
        // Apply velocity.
        m_state->camera_state.free_camera_pos = hierpos_add_vec2(&m_state->camera_state.free_camera_pos,
                                                    vec2_scale(m_free_camera_vel, dt));
        // Under the floating-origin path camera.position is a render-space residual paired with
        // camera_hierpos; game.cpp re-derives camera_hierpos + residual from free_camera_pos every
        // frame, so we leave camera.position untouched here to keep game_screen_to_true_world (used
        // by the box selection below, which runs before the end-of-frame re-anchor) consistent.
    }
    // ---- System-mode camera pan REMOVED: the galaxy-map look now uses the same detached free-
    // camera path above (free_camera_pos), so there is no separate camera.position pan branch. ----
    // ---- Selection and order input (whenever the camera is detached) -----------------------
    if (detached) {
        // ---- Box selection input -------------------------------------------------------
        b8 left_pressed = input_is_button_down(BUTTON_LEFT) && !input_was_button_down(BUTTON_LEFT);
        b8 left_down = input_is_button_down(BUTTON_LEFT);
        b8 left_released = !input_is_button_down(BUTTON_LEFT) && input_was_button_down(BUTTON_LEFT);
        if (left_pressed && !bs_imgui_wants_mouse()) {
            m_box.active = TRUE;
            m_box.start_x = (f32)mx;
            m_box.start_y = (f32)my;
            m_box.end_x = (f32)mx;
            m_box.end_y = (f32)my;
        }
        if (m_box.active && left_down) {
            m_box.end_x = (f32)mx;
            m_box.end_y = (f32)my;
        }
        if (m_box.active && left_released) {
            f32 dx = m_box.end_x - m_box.start_x;
            f32 dy = m_box.end_y - m_box.start_y;
            if (dx * dx + dy * dy < RTS_CLICK_THRESHOLD * RTS_CLICK_THRESHOLD) {
                // Click: select the hovered fleet ship (or none).
                m_state->fleet_state.fleet.clear_selection();
                if (m_hovered_ship_idx >= 0) {
                    m_state->fleet_state.fleet.set_selected(m_hovered_ship_idx, TRUE);
                }
            } else {
                // Drag: box-select all fleet ships whose origins fall inside the world box.
                bs_math::HierPos2 p0 = game_screen_to_true_hierpos(m_state, Vec2{ m_box.start_x, m_box.start_y });
                bs_math::HierPos2 p1 = game_screen_to_true_hierpos(m_state, Vec2{ m_box.end_x, m_box.end_y });
                m_state->fleet_state.fleet.clear_selection();
                for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
                    if (ship_inside_world_box(&m_state->fleet_state.fleet.at(i).ship, p0, p1)) {
                        m_state->fleet_state.fleet.set_selected(i, TRUE);
                    }
                }
            }
            m_box.active = FALSE;
        }
        // ---- Move / attack order input -------------------------------------------------
        b8 right_click = input_is_button_down(BUTTON_RIGHT) && !input_was_button_down(BUTTON_RIGHT);
        if (right_click && !bs_imgui_wants_mouse()) {
            if (m_state->fleet_state.fleet.any_selected()) {
                if (m_hovered_enemy_idx >= 0) {
                    CombatEntity* ce = &m_state->combat_entities[m_hovered_enemy_idx];
                    if (ce->active && ce->ship) {
                        m_state->fleet_state.fleet.order_attack(ce->ship);
                    }
                } else {
                    m_state->fleet_state.fleet.order_move(mouse_hp);
                }
            }
        }
        // ---- Cancel attack orders --------------------------------------------------------
        if (input_is_key_down(KEY_X) && !input_was_key_down(KEY_X)) {
            b8 ctrl_held = input_is_key_down(KEY_LCONTROL) || input_is_key_down(KEY_RCONTROL) ||
                           input_is_key_down(KEY_CONTROL);
            if (ctrl_held) {
                // Ctrl+X: clear attack orders for the entire fleet.
                for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
                    m_state->fleet_state.fleet.at(i).clear_attack_target();
                }
            } else {
                // X: clear attack orders for selected ships only.
                for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
                    if (m_state->fleet_state.fleet.is_selected(i)) {
                        m_state->fleet_state.fleet.at(i).clear_attack_target();
                    }
                }
            }
        }
        // ---- Number-row piloting selection (1-4) while detached ------------------------
        if (input_is_key_down(KEY_NUM1) && !input_was_key_down(KEY_NUM1)) {
            if (m_state->fleet_state.fleet.count() > 0) m_state->fleet_state.fleet.set_piloted(0);
        }
        if (input_is_key_down(KEY_NUM2) && !input_was_key_down(KEY_NUM2)) {
            if (m_state->fleet_state.fleet.count() > 1) m_state->fleet_state.fleet.set_piloted(1);
        }
        if (input_is_key_down(KEY_NUM3) && !input_was_key_down(KEY_NUM3)) {
            if (m_state->fleet_state.fleet.count() > 2) m_state->fleet_state.fleet.set_piloted(2);
        }
        if (input_is_key_down(KEY_NUM4) && !input_was_key_down(KEY_NUM4)) {
            if (m_state->fleet_state.fleet.count() > 3) m_state->fleet_state.fleet.set_piloted(3);
        }
        m_piloted_idx = m_state->fleet_state.fleet.piloted_index();
    }
    // ---- Camera transition: free camera -> piloted ship ---------------------------------
    if (m_camera_transitioning) {
        FleetShip* piloted = m_state->fleet_state.fleet.piloted();
        if (!piloted) piloted = &m_state->fleet_state.fleet.at(0);
        m_camera_transition_t += dt / 0.60f;
        if (m_camera_transition_t > 1.0f) m_camera_transition_t = 1.0f;
        f32 t = m_camera_transition_t;
        f32 eased = t * t * (3.0f - 2.0f * t); // smoothstep
        bs_math::HierPos2 target = piloted->ship.origin;
        bs_math::HierPos2 new_pos = hierpos_lerp(&m_camera_transition_from, &target, eased, BS_HIERPOS_CELL_SIZE);
        m_state->camera_state.free_camera_pos = new_pos; // keep free_camera_pos in sync
        if (t >= 1.0f) {
            m_camera_transitioning = FALSE;
            m_state->camera_state.free_camera_active = FALSE;
        }
    }
}
// =====================================================================================
void RtsControls::draw_dashed_circle(Vec2 center, f32 radius, u32 segments, f32 fill_ratio,
                                     f32 rotation, f32 thickness, bs_color color, u32 layer) {
    f32 step = (2.0f * BS_PI) / (f32)segments;
    f32 dash_span = step * fill_ratio;
    for (u32 i = 0; i < segments; ++i) {
        f32 a0 = rotation + (f32)i * step;
        f32 a1 = a0 + dash_span;
        Vec2 p0 = vec2_add(center, Vec2{ cosf(a0) * radius, sinf(a0) * radius });
        Vec2 p1 = vec2_add(center, Vec2{ cosf(a1) * radius, sinf(a1) * radius });
        renderer_draw_line(p0, p1, thickness, color, layer);
    }
}
// =====================================================================================
void RtsControls::draw() {
    if (!m_state) return;
    b8 in_free_camera = m_state->camera_state.free_camera_active;
    // RTS selection visuals are shown whenever the camera is detached (free camera), at any zoom.
    // Positions are pushed through the same compressed render-space transform the ship sprites use
    // so rings / markers line up with the ships in both the arena and galaxy-map looks.
    b8 draw_rts_overlay = in_free_camera;
    // Free-camera-only visuals: drag box, selection rects, move/attack markers, hover rings.
    if (draw_rts_overlay) {
        // Draw the active drag box.
        if (m_box.active) {
            draw_rect_from_screen_box(m_box, BOX_SELECT_THICKNESS, BOX_SELECT_COLOR, HOVER_CIRCLE_LAYER);
        }
        // Draw selection rectangles around every selected fleet ship.
        for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
            if (m_state->fleet_state.fleet.is_selected(i)) {
                const Ship* ship = &m_state->fleet_state.fleet.at(i).ship;
                draw_selection_rect(render_from_hierpos(m_state, &ship->origin),
                                    ship_bounding_radius(ship), BOX_SELECT_THICKNESS,
                                    SELECTION_RECT_COLOR, HOVER_CIRCLE_LAYER);
            }
        }
        // Draw move/attack markers for active orders.
        for (i32 i = 0; i < m_state->fleet_state.fleet.count(); ++i) {
            FleetShip& fs = m_state->fleet_state.fleet.at(i);
            if (fs.has_move_target) {
                draw_move_marker(render_from_hierpos(m_state, &fs.move_target),
                                 RTS_MOVE_MARKER_THICKNESS,
                                 RTS_MOVE_MARKER_COLOR, HOVER_CIRCLE_LAYER);
            }
            if (fs.has_attack_target && fs.attack_target) {
                draw_attack_marker(render_from_hierpos(m_state, &fs.attack_target->origin),
                                   ship_bounding_radius(fs.attack_target), BOX_SELECT_THICKNESS,
                                   bs_color{ 1.0f, 0.25f, 0.25f, 1.0f }, HOVER_CIRCLE_LAYER);
            }
        }
        // Hover ring around the hovered ship (fleet or not).
        if (m_hovered_ship_idx >= 0) {
            const Ship* ship = &m_state->fleet_state.fleet.at(m_hovered_ship_idx).ship;
            f32 radius = ship_bounding_radius(ship);
            if (radius > 0.0f) {
                bs_color col = HOVER_CIRCLE_COLOR;
                f32 pulse = 0.5f + 0.5f * sinf(m_state->elapsed_time * 3.0f);
                col.a *= 0.7f + 0.3f * pulse;
                draw_dashed_circle(render_from_hierpos(m_state, &ship->origin), radius,
                                   (u32)HOVER_DASH_SEGMENTS,
                                   HOVER_DASH_FILL_RATIO, m_dash_angle,
                                   HOVER_CIRCLE_THICKNESS, col, HOVER_CIRCLE_LAYER);
            }
        }
    }
    // Draw a dedicated HUD panel for the currently piloted ship.
    FleetShip* piloted = m_state->fleet_state.fleet.piloted();
    if (!piloted) piloted = &m_state->fleet_state.fleet.at(0);
    if (piloted) {
        Ship* ship = &piloted->ship;
        ShipFlight* fl = &piloted->flight;
        if (bs_ui_begin_hud_panel("FLEET SHIP", BS_UI_ANCHOR_TOP_RIGHT, 190.0f)) {
            bs_ui_text_colored(0.35f, 0.85f, 0.95f, 1.0f,
                               ship->vessel_name ? ship->vessel_name : "Ship");
            bs_ui_text_colored(0.55f, 0.65f, 0.75f, 0.85f, vessel_faction_name(ship->faction));
            bs_ui_separator();
            char buf[128];
            snprintf(buf, sizeof(buf), "Speed: %.0f", vec2_length(fl->velocity));
            bs_ui_text(buf);
            f32 heading_deg = ship->angle * 180.0f / BS_PI;
            snprintf(buf, sizeof(buf), "Heading: %.0f", heading_deg);
            bs_ui_text(buf);
            bs_ui_text("Health: --");
            bs_ui_separator();
            bs_ui_text("WEAPONS");
            for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) {
                if (i >= ship->weapon_count || !ship->weapons[i]) {
                    bs_ui_text_colored(0.35f, 0.45f, 0.50f, 0.60f, "-- empty --");
                    continue;
                }
                Weapon* w = ship->weapons[i];
                b8 selected = (i == ship->active_weapon_idx);
                const char* name = w->name ? w->name : "?";
                if (w->ready()) {
                    snprintf(buf, sizeof(buf), "%d  %s  READY", i + 1, name);
                } else {
                    f32 prog = w->cooldown_progress();
                    snprintf(buf, sizeof(buf), "%d  %s  %.0f%%", i + 1, name, prog * 100.0f);
                }
                if (bs_ui_selectable(buf, selected)) {
                    ship->active_weapon_idx = i;
                }
            }
            bs_ui_separator();
            if (in_free_camera) {
                if (bs_ui_button("Pilot unit", !m_camera_transitioning)) {
                    m_state->fleet_state.fleet.set_piloted(m_state->fleet_state.fleet.any_selected()
                                               ? m_state->fleet_state.fleet.first_selected()
                                               : 0);
                    m_piloted_idx = m_state->fleet_state.fleet.piloted_index();
                    m_camera_transitioning = TRUE;
                    m_camera_transition_t = 0.0f;
                    m_camera_transition_from = game_camera_center_hierpos(m_state);
                    m_free_camera_vel = Vec2{ 0.0f, 0.0f };
                    m_free_camera_dragging = FALSE;
                    // Note: only the piloted ship's order is cleared inside set_piloted().
                    // The rest of the fleet continues its current Move/Attack orders.
                }
            } else {
                if (bs_ui_button("Auto-pilot / RTS", !m_camera_transitioning)) {
                    m_state->camera_state.free_camera_active = TRUE;
                    m_state->camera_state.free_camera_pos = game_camera_center_hierpos(m_state);
                    m_camera_transitioning = FALSE;
                    // Existing fleet orders are preserved; the previously piloted ship
                    // coasts until a new order is issued.
                }
            }
            // Number-row weapon switching while piloted.
            if (input_is_key_down(KEY_NUM1) && !input_was_key_down(KEY_NUM1)) {
                if (ship->weapon_count > 0) ship->active_weapon_idx = 0;
            }
            if (input_is_key_down(KEY_NUM2) && !input_was_key_down(KEY_NUM2)) {
                if (ship->weapon_count > 1) ship->active_weapon_idx = 1;
            }
            if (input_is_key_down(KEY_NUM3) && !input_was_key_down(KEY_NUM3)) {
                if (ship->weapon_count > 2) ship->active_weapon_idx = 2;
            }
            if (input_is_key_down(KEY_NUM4) && !input_was_key_down(KEY_NUM4)) {
                if (ship->weapon_count > 3) ship->active_weapon_idx = 3;
            }
        }
        bs_ui_end_hud_panel();
    }
}
// =====================================================================================
void RtsControls::draw_move_marker(Vec2 target, f32 thickness, bs_color color, u32 layer) {
    f32 h = RTS_MOVE_MARKER_SIZE * 0.5f;
    // Draw a '+' cross centered at the target.
    Vec2 horizontal_p0 = Vec2{ target.x - h, target.y };
    Vec2 horizontal_p1 = Vec2{ target.x + h, target.y };
    Vec2 vertical_p0   = Vec2{ target.x, target.y - h };
    Vec2 vertical_p1   = Vec2{ target.x, target.y + h };
    renderer_draw_line(horizontal_p0, horizontal_p1, thickness, color, layer);
    renderer_draw_line(vertical_p0, vertical_p1, thickness, color, layer);
}
// =====================================================================================
void RtsControls::draw_attack_marker(Vec2 center, f32 radius, f32 thickness, bs_color color, u32 layer) {
    if (radius <= 0.0f) return;
    // Draw four corner brackets around the target.
    f32 corner = radius * 0.6f;
    f32 gap = radius * 0.2f;
    f32 r = radius + gap;
    Vec2 c = center;
    // Top-left.
    renderer_draw_line(Vec2{ c.x - r, c.y - r + corner }, Vec2{ c.x - r, c.y - r }, thickness, color, layer);
    renderer_draw_line(Vec2{ c.x - r, c.y - r }, Vec2{ c.x - r + corner, c.y - r }, thickness, color, layer);
    // Top-right.
    renderer_draw_line(Vec2{ c.x + r - corner, c.y - r }, Vec2{ c.x + r, c.y - r }, thickness, color, layer);
    renderer_draw_line(Vec2{ c.x + r, c.y - r }, Vec2{ c.x + r, c.y - r + corner }, thickness, color, layer);
    // Bottom-right.
    renderer_draw_line(Vec2{ c.x + r, c.y + r - corner }, Vec2{ c.x + r, c.y + r }, thickness, color, layer);
    renderer_draw_line(Vec2{ c.x + r, c.y + r }, Vec2{ c.x + r - corner, c.y + r }, thickness, color, layer);
    // Bottom-left.
    renderer_draw_line(Vec2{ c.x - r + corner, c.y + r }, Vec2{ c.x - r, c.y + r }, thickness, color, layer);
    renderer_draw_line(Vec2{ c.x - r, c.y + r }, Vec2{ c.x - r, c.y + r - corner }, thickness, color, layer);
}
