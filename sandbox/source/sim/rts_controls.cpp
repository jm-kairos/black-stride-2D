#include "sim/rts_controls.h"
#include "game.h"
#include "core/view_transform.h" // game_screen_to_true_*, render_from_hierpos, game_camera_center_hierpos
#include "sim/ship.h"
#include "sim/weapon.h"
#include "sim/fleet.h"
#include <core/input.h>
#include <math/math_utils.h>
#include <renderer/renderer.h>
#include <renderer/bs_imgui.h>
#include <renderer/bs_rml.h>   // bs_rml_wants_mouse: in-game UI (RmlUi) owns the cursor
#include "render/galaxy_map_render.h" // galaxy_pick_planet: planet under the cursor (map look)
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
// FTL jump-mode visuals.
static constexpr f32 JUMP_CIRCLE_THICKNESS = 2.0f;
static constexpr bs_color JUMP_CIRCLE_COLOR = { 0.55f, 0.95f, 0.65f, 0.90f };
// Free camera movement constants.
static constexpr f32 FREE_CAM_MOVE_SPEED     = 1600.0f;  // WASD constant world-space speed (mechanical)
static constexpr f32 FREE_CAM_SHIFT_MULT     = 3.0f;     // SHIFT speed multiplier for WASD/edge pan
static constexpr f32 FREE_CAM_EDGE_MARGIN    = 24.0f;    // pixels
// =====================================================================================
RtsControls::RtsControls()
    : m_state(nullptr)
    , m_hovered_ship_idx(-1)
    , m_hovered_enemy_idx(-1)
    , m_dash_angle(0.0f)
    , m_jump_mode(FALSE)
    , m_box{ FALSE, 0.0f, 0.0f, 0.0f, 0.0f }
    , m_piloted_idx(0)
    , m_free_camera_vel(Vec2{0.0f, 0.0f}) {}
RtsControls::RtsControls(game_state* s)
    : m_state(s)
    , m_hovered_ship_idx(-1)
    , m_hovered_enemy_idx(-1)
    , m_dash_angle(0.0f)
    , m_jump_mode(FALSE)
    , m_box{ FALSE, 0.0f, 0.0f, 0.0f, 0.0f }
    , m_piloted_idx(0)
    , m_free_camera_vel(Vec2{0.0f, 0.0f}) {}
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
    // Hit-test and issue orders in true (simulation) world space, the same space ship.origin
    // lives in.
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
        // Discovery: ignore undiscovered NPC agents (they render as generic markers).
        if (ce->is_npc && ce->npc_index >= 0 && !m_state->npc_ships[ce->npc_index].discovered) continue;
        if (is_point_over_ship(mouse_hp, ce->ship)) {
            m_hovered_enemy_idx = i;
            break;
        }
    }
    // ---- Space-station hover + right-click context menu (works in both view modes) ----------
    // Hit-test every VISIBLE station of every materialised system in true world space — discovered
    // or not (undiscovered ones render as antenna markers, so they must react too), skipping only
    // stations culled by sensor visibility (matching draw_system_stations' cull, so hover targets
    // exactly what is drawn). A screen-constant pick floor keeps distant screen-marker stations
    // clickable. Right-click opens the RML context menu at the cursor and suppresses the RTS move
    // order this frame; a click elsewhere dismisses the menu.
    b8 station_right_consumed = FALSE;
    {
        i32 hovered_sid = -1;
        f32 zoom = m_state->camera_state.camera.zoom;
        f32 pick_floor = 16.0f / (zoom > 1e-6f ? zoom : 1e-6f); // ~16 px in world units
        for (i32 si = 0; si < m_state->galaxy.system_count && hovered_sid < 0; ++si) {
            StarSystem& ss = m_state->galaxy.systems[si];
            for (i32 k = 0; k < ss.station_count; ++k) {
                SystemStation& st = ss.stations[k];
                if (m_state->galaxy.map_draw_sensor_range) {        // sensor-culled stations don't react
                    f32 sd = vec2_length(hierpos_diff(&st.pos, &m_state->player_ship().origin, BS_HIERPOS_CELL_SIZE));
                    if (sensor_visibility_from_dist(sd, m_state->galaxy.map_sensor_range) <= 0.003f) continue;
                }
                f32 hit = st.radius > pick_floor ? st.radius : pick_floor;
                f32 d   = vec2_length(hierpos_diff(&mouse_hp, &st.pos, BS_HIERPOS_CELL_SIZE));
                if (d <= hit) { hovered_sid = st.station_id; break; }
            }
        }
        m_state->hovered_station_id = hovered_sid;

        b8 ui_free = !bs_imgui_wants_mouse() && !bs_rml_wants_mouse();
        b8 rclick  = input_is_button_down(BUTTON_RIGHT) && !input_was_button_down(BUTTON_RIGHT);
        b8 lclick  = input_is_button_down(BUTTON_LEFT)  && !input_was_button_down(BUTTON_LEFT);
        if (rclick && ui_free && hovered_sid >= 0) {
            m_state->station_menu_visible    = true;
            m_state->station_menu_station_id = hovered_sid;
            m_state->station_menu_x          = mx;
            m_state->station_menu_y          = my;
            station_right_consumed           = TRUE;      // don't also issue an RTS move order
        } else if ((lclick || rclick) && ui_free && m_state->station_menu_visible) {
            m_state->station_menu_visible = false;        // click outside the menu dismisses it
        }

        // ---- Planet left-click -> planet inspector window (galaxy-map look only) ------------
        // Uses the draw-accurate hit-test so only planets the player can actually see react.
        // Selection is stored as (system galaxy_center, planet index): the cache slot can be
        // recycled, the center cannot. The window persists until its Close box (like the
        // station inspector); clicking empty space keeps normal RTS behavior.
        if (lclick && ui_free && m_state->view_arena_w < 1.0f) {
            i32 pick_slot = -1, pick_planet = -1;
            if (galaxy_pick_planet(m_state, (f32)mx, (f32)my, &pick_slot, &pick_planet)) {
                m_state->show_planet_inspector = true;
                m_state->planet_insp_center    = m_state->galaxy.systems[pick_slot].galaxy_center;
                m_state->planet_insp_planet    = pick_planet;
            }
        }
    }
    m_dash_angle += HOVER_DASH_ROTATION_SPEED * dt;
    if (m_dash_angle > 2.0f * BS_PI) m_dash_angle -= 2.0f * BS_PI;
    // Detached = the free camera is active (browse / pan / RTS), independent of zoom or render look.
    b8 detached = (m_state->camera_state.free_camera_active && !m_state->camera_state.recentering);
    // Jump mode is an RTS-only interaction: drop it whenever the camera is attached/piloting.
    if (!detached) m_jump_mode = FALSE;
    // ---- Free camera movement (only while detached, not during a recenter glide) ------------
    if (detached) {
        // ---- Free camera movement (WASD + edge pan) -------------------------------------
        // Scale movement so panning covers a roughly constant fraction of the screen per second at
        // any zoom: the on-screen scale is zoom, so world-space speed uses its inverse. This
        // makes the camera faster the more you zoom out (unbounded above is fine: zoom is clamped to
        // ZOOM_GLOBAL_MIN, so zoom has a floor). Floor at 1 keeps the zoomed-in feel unchanged; the
        // cap covers 1/ZOOM_GLOBAL_MIN so panning stays screen-proportional at full galaxy zoom-out.
        f32 zoom_mul = clampf(1.0f / m_state->camera_state.camera.zoom, 1.0f, 2.0e9f);
        Vec2 input_dir = read_wasd_dir();
        b8 fast = input_is_key_down(KEY_LSHIFT) || input_is_key_down(KEY_RSHIFT);
        if (!bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {
            Vec2 edge_dir{ 0.0f, 0.0f };
            if (mx < (i32)FREE_CAM_EDGE_MARGIN) edge_dir.x -= 1.0f;
            if (mx > (i32)m_state->fb_width - (i32)FREE_CAM_EDGE_MARGIN) edge_dir.x += 1.0f;
            if (my > (i32)m_state->fb_height - (i32)FREE_CAM_EDGE_MARGIN) edge_dir.y -= 1.0f;
            if (my < (i32)FREE_CAM_EDGE_MARGIN) edge_dir.y += 1.0f;
            input_dir = vec2_add(input_dir, edge_dir);
        }
        // Mechanical WASD/edge pan: instant constant velocity while held, instant stop on release
        // (no accel ramp, no friction coast) for a crisp, direct feel. SHIFT multiplies the speed.
        {
            f32 dir_len = vec2_length(input_dir);
            if (dir_len > 0.0f) {
                Vec2 dir   = vec2_scale(input_dir, 1.0f / dir_len);   // unit -> constant diagonal speed
                f32  speed = FREE_CAM_MOVE_SPEED * zoom_mul * (fast ? FREE_CAM_SHIFT_MULT : 1.0f);
                m_free_camera_vel = vec2_scale(dir, speed);
            } else {
                m_free_camera_vel = Vec2{ 0.0f, 0.0f };
            }
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
        if (left_pressed && !bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {
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
        // ---- FTL jump mode toggle (J) --------------------------------------------------
        Fleet& fleet = m_state->fleet_state.fleet;
        if (input_is_key_down(KEY_J) && !input_was_key_down(KEY_J)) {
            if (fleet.any_selected()) m_jump_mode = !m_jump_mode;
            else                      m_jump_mode = FALSE;
        }
        // Jump mode requires a live selection; drop it if the selection was cleared.
        if (m_jump_mode && !fleet.any_selected()) m_jump_mode = FALSE;
        // ---- Move / attack / jump order input ------------------------------------------
        b8 right_click = input_is_button_down(BUTTON_RIGHT) && !input_was_button_down(BUTTON_RIGHT);
        if (right_click && !station_right_consumed && !bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {
            if (m_jump_mode) {
                // Right-click executes the jump when the destination is within range; an
                // out-of-range click is rejected and jump mode stays armed for a valid point.
                i32 center_idx = -1;
                f32 radius = 0.0f;
                if (fleet.selected_min_jump(&center_idx, &radius)) {
                    HierPos2 center_origin = fleet.at(center_idx).ship.origin;
                    // The player picks the precise destination: the fleet jumps to the exact clicked
                    // point (any arbitrary location), rejected only if it lies outside jump range.
                    HierPos2 dest = mouse_hp;
                    f32 dist = vec2_length(hierpos_diff(&dest, &center_origin));
                    if (dist <= radius) {
                        fleet.jump_selected(center_idx, dest);
                        m_jump_mode = FALSE;
                    }
                }
            } else if (fleet.any_selected()) {
                if (m_hovered_enemy_idx >= 0) {
                    CombatEntity* ce = &m_state->combat_entities[m_hovered_enemy_idx];
                    if (ce->active && ce->ship) {
                        fleet.order_attack(ce->ship);
                    }
                } else {
                    fleet.order_move(mouse_hp);
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
    // Positions are pushed through the same render-space transform the ship sprites use
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
        // ---- FTL jump mode: dashed range ring (the "Currently in Jump Mode" banner is now an
        // RmlUi HUD document driven from game_update via game_push_hud) --------------------
        if (m_jump_mode) {
            i32 center_idx = -1;
            f32 radius = 0.0f;
            if (m_state->fleet_state.fleet.selected_min_jump(&center_idx, &radius)) {
                const Ship* center = &m_state->fleet_state.fleet.at(center_idx).ship;
                // render_from_hierpos maps the center into render space; the renderer then applies
                // zoom on top, so the world radius maps directly to the ring.
                draw_dashed_circle(render_from_hierpos(m_state, &center->origin),
                                   radius, (u32)HOVER_DASH_SEGMENTS,
                                   HOVER_DASH_FILL_RATIO, m_dash_angle,
                                   JUMP_CIRCLE_THICKNESS, JUMP_CIRCLE_COLOR, HOVER_CIRCLE_LAYER);
            }
        }
    }
    // The FLEET SHIP readout is now an RmlUi HUD document (see hud.rml + game_push_hud). Only the
    // number-row weapon switching for the piloted ship remains here as input handling.
    FleetShip* piloted = m_state->fleet_state.fleet.piloted();
    if (!piloted) piloted = &m_state->fleet_state.fleet.at(0);
    if (piloted) {
        Ship* ship = &piloted->ship;
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
}
// =====================================================================================
// HUD "Pilot unit" / "Auto-pilot / RTS" button handler. Invoked by game_push_hud when the
// RmlUi fleet panel button is clicked. A no-op while a recenter glide is already in flight
// (the button renders dimmed in that state).
void RtsControls::hud_toggle_pilot_mode() {
    if (!m_state || m_state->camera_state.recentering) return;
    if (m_state->camera_state.free_camera_active) {
        // Free camera -> pilot the selected unit (or the flagship): smooth recenter glide.
        m_state->fleet_state.fleet.set_piloted(m_state->fleet_state.fleet.any_selected()
                                   ? m_state->fleet_state.fleet.first_selected()
                                   : 0);
        m_piloted_idx = m_state->fleet_state.fleet.piloted_index();
        m_state->camera_state.recentering       = TRUE;
        m_state->camera_state.recenter_t        = 0.0f;
        m_state->camera_state.recenter_from_pos = game_camera_center_hierpos(m_state);
        m_state->camera_state.global_free_camera_saved = FALSE; // deliberate piloting intent (Model A)
        m_free_camera_vel = Vec2{ 0.0f, 0.0f };
        // Note: only the piloted ship's order is cleared inside set_piloted().
        // The rest of the fleet continues its current Move/Attack orders.
    } else {
        // Piloting -> detach to the free-camera RTS view.
        m_state->camera_state.free_camera_active = TRUE;
        m_state->camera_state.free_camera_pos = game_camera_center_hierpos(m_state);
        m_state->camera_state.global_free_camera_saved = TRUE; // deliberate free-camera intent (Model A)
        // Existing fleet orders are preserved; the previously piloted ship
        // coasts until a new order is issued.
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
