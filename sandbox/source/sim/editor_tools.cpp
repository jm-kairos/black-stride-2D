#include "sim/editor_tools.h"
#include "game.h"
#include "core/cursor_world.h" // mouse_true_world, mouse_true_hierpos
#include "core/geom2d.h"   // point_in_polygon, point_to_segment
#include <core/input.h>    // input_is_button_down / input_was_button_down / BUTTON_LEFT
#include <renderer/bs_imgui.h> // bs_imgui_wants_mouse
#include <renderer/bs_rml.h>   // bs_rml_wants_mouse
#include <math.h>          // atan2f, fabsf, sqrtf

using namespace bs_math;

// ---- Edit mode picking -----------------------------------------------------------------

// Return the world position of an edit selection, or {0,0} for EDIT_NONE.
HierPos2 edit_entity_position(const game_state* s, EditSelection sel) {
    switch (sel.kind) {
        case EDIT_LIGHT:
            if (sel.index >= 0 && sel.index < (i32)s->render.lights.size())
                return hierpos_from_vec2(s->render.lights[sel.index].position, BS_HIERPOS_CELL_SIZE);
            break;
        case EDIT_SHIP:
            return (sel.index == 0) ? s->player_ship().origin : s->fleet_state.enemy_ship.origin;
        default: break;
    }
    return HierPos2{};
}

// Return the world angle (radians) of an edit selection, or 0 for non-ships.
static f32 edit_entity_angle(const game_state* s, EditSelection sel) {
    if (sel.kind == EDIT_SHIP)
        return (sel.index == 0) ? s->player_ship().angle : s->fleet_state.enemy_ship.angle;
    return 0.0f;
}

// Write a new world position back to the selected entity.
static void edit_entity_set_position(game_state* s, EditSelection sel, HierPos2 pos) {
    switch (sel.kind) {
        case EDIT_LIGHT:
            if (sel.index >= 0 && sel.index < (i32)s->render.lights.size())
                s->render.lights[sel.index].position = hierpos_to_vec2(&pos, BS_HIERPOS_CELL_SIZE);
            break;
        case EDIT_SHIP:
            if (sel.index == 0) s->player_ship().origin = pos;
            else                s->fleet_state.enemy_ship.origin     = pos;
            break;
        default: break;
    }
}

// Write a new angle (radians) back to the selected ship.
static void edit_entity_set_angle(game_state* s, EditSelection sel, f32 a) {
    if (sel.kind == EDIT_SHIP) {
        if (sel.index == 0) s->player_ship().angle = a;
        else                s->fleet_state.enemy_ship.angle     = a;
    }
}

// Hit-test the cursor against editable entities (lights first, then ships). Returns the
// selection under the cursor, or {EDIT_NONE, -1} if nothing was hit.
// Lights are picked by their CENTER only (small screen-space tolerance), not by radius.
static EditSelection edit_pick(const game_state* s, HierPos2 cursor) {
    // Lights: nearest within a small screen-space tolerance around the center point.
    f32 tol = 20.0f / ((s->camera_state.camera.zoom > 0.0001f) ? s->camera_state.camera.zoom : 1.0f);
    f32 tol2 = tol * tol;
    i32 best_light = -1;
    f32 best_d2 = 0.0f;
    for (i32 i = 0; i < (i32)s->render.lights.size(); ++i) {
        HierPos2 lhp = hierpos_from_vec2(s->render.lights[i].position, BS_HIERPOS_CELL_SIZE);
        Vec2 d = hierpos_diff(&cursor, &lhp, BS_HIERPOS_CELL_SIZE);
        f32  d2 = d.x * d.x + d.y * d.y;
        if (d2 <= tol2 && (best_light < 0 || d2 < best_d2)) {
            best_light = i; best_d2 = d2;
        }
    }
    if (best_light >= 0) return EditSelection{ EDIT_LIGHT, best_light };
    // Ships: point-in-polygon against the collider (corners are ship-origin-relative).
    Vec2 corners[SHIP_MAX_COLLIDER_VERTS];
    Vec2 pc = hierpos_diff(&cursor, &s->player_ship().origin, BS_HIERPOS_CELL_SIZE);
    if (ship_collider_corners(&s->player_ship(), corners) &&
        point_in_polygon(pc, corners, s->player_ship().collider_count)) {
        return EditSelection{ EDIT_SHIP, 0 };
    }
    Vec2 ec = hierpos_diff(&cursor, &s->fleet_state.enemy_ship.origin, BS_HIERPOS_CELL_SIZE);
    if (ship_collider_corners(&s->fleet_state.enemy_ship, corners) &&
        point_in_polygon(ec, corners, s->fleet_state.enemy_ship.collider_count)) {
        return EditSelection{ EDIT_SHIP, 1 };
    }
    return EditSelection{ EDIT_NONE, -1 };
}

// Gizmo geometry helpers (all in world space).
// All sizes are expressed as target screen pixels, then converted to world units via zoom_inv.
// This keeps gizmos visible at every zoom level.
f32 gizmo_axis_len(f32 zoom_inv) { return 40.0f * zoom_inv; }
f32 gizmo_ring_radius_ship(const Ship* ship, f32 zoom_inv) {
    f32 visual_half = vec2_length(vec2_scale(ship->visual.size_local, 0.5f));
    return visual_half + 30.0f * zoom_inv;  // entity bounds + 30 px screen padding
}
f32 gizmo_ring_radius_light(f32 zoom_inv) { return 40.0f * zoom_inv; }
f32 gizmo_arrow_size(f32 zoom_inv) { return 8.0f * zoom_inv; }

// Test which gizmo part (if any) is under the cursor for the current selection.
// Returns the drag mode that should be used, or EDIT_DRAG_FREE for the entity body.
EditDragMode edit_pick_gizmo(const game_state* s, HierPos2 cursor) {
    if (s->editor.edit_selection.kind == EDIT_NONE) return EDIT_DRAG_NONE;
    HierPos2 origin_hp = edit_entity_position(s, s->editor.edit_selection);
    // Work relative to the entity origin so the gizmo math stays precise far from the origin.
    Vec2 c = hierpos_diff(&cursor, &origin_hp, BS_HIERPOS_CELL_SIZE);
    Vec2 origin = Vec2{ 0.0f, 0.0f };
    f32 zoom_inv = 1.0f / ((s->camera_state.camera.zoom > 0.0001f) ? s->camera_state.camera.zoom : 1.0f);
    f32 axis_len = gizmo_axis_len(zoom_inv);
    f32 tol = 12.0f * zoom_inv;  // world-space tolerance around gizmo lines
    // Rotation ring: narrow band around the ring.
    f32 ring_r = 0.0f;
    if (s->editor.edit_selection.kind == EDIT_SHIP) {
        const Ship* sh = (s->editor.edit_selection.index == 0) ? &s->player_ship() : &s->fleet_state.enemy_ship;
        ring_r = gizmo_ring_radius_ship(sh, zoom_inv);
    } else {
        ring_r = gizmo_ring_radius_light(zoom_inv);
    }
    f32 d_ring = fabsf(vec2_length(vec2_sub(c, origin)) - ring_r);
    if (d_ring <= tol * 1.5f) return EDIT_DRAG_ROTATE;
    // Axis arrows.
    Vec2 x_end = vec2_add(origin, Vec2{ axis_len, 0.0f });
    Vec2 y_end = vec2_add(origin, Vec2{ 0.0f, axis_len });
    if (point_to_segment(c, origin, x_end) <= tol) return EDIT_DRAG_AXIS_X;
    if (point_to_segment(c, origin, y_end) <= tol) return EDIT_DRAG_AXIS_Y;
    return EDIT_DRAG_FREE;
}

// Edit-mode input: left-click selects an entity under the cursor and begins a drag; holding
// the button repositions it; releasing ends the drag. Clicking empty space deselects. Gated
// on bs_imgui_wants_mouse so clicks on the EDITOR PANEL never pick world entities.
void update_edit_mode(game_state* s) {
    if (!s->editor.edit_mode_active) {
        s->editor.edit_drag.active = FALSE;
        s->editor.edit_drag.mode   = EDIT_DRAG_NONE;
        return;
    }
    if (bs_imgui_wants_mouse() || bs_rml_wants_mouse()) return; // cursor over a panel; ignore world picks
    Vec2 cursor = mouse_true_world(s);
    HierPos2 cursor_hp = mouse_true_hierpos(s);
    (void)cursor;
    b8 down     = input_is_button_down(BUTTON_LEFT);
    b8 was_down = input_was_button_down(BUTTON_LEFT);
    if (down && !was_down) {
        // Edge: mouse just pressed.
        // If we already have a selection, test gizmos first; otherwise pick a new entity.
        EditDragMode gizmo = EDIT_DRAG_NONE;
        if (s->editor.edit_selection.kind != EDIT_NONE)
            gizmo = edit_pick_gizmo(s, cursor_hp);
        if (gizmo != EDIT_DRAG_NONE) {
            // Gizmo drag started on the currently selected entity.
            s->editor.edit_drag.active        = TRUE;
            s->editor.edit_drag.mode          = gizmo;
            s->editor.edit_drag.drag_anchor   = cursor_hp;
            s->editor.edit_drag.entity_anchor = edit_entity_position(s, s->editor.edit_selection);
            s->editor.edit_drag.entity_angle  = edit_entity_angle(s, s->editor.edit_selection);
        } else {
            // No gizmo hit -> try picking a new entity (or deselect on empty space).
            EditSelection hit = edit_pick(s, cursor_hp);
            s->editor.edit_selection = hit;
            if (hit.kind != EDIT_NONE) {
                s->editor.edit_drag.active        = TRUE;
                s->editor.edit_drag.mode          = EDIT_DRAG_FREE;
                s->editor.edit_drag.drag_anchor   = cursor_hp;
                s->editor.edit_drag.entity_anchor = edit_entity_position(s, hit);
                s->editor.edit_drag.entity_angle  = edit_entity_angle(s, hit);
                if (hit.kind == EDIT_LIGHT) s->render.light_selected = hit.index;
            } else {
                s->editor.edit_drag.active = FALSE;
                s->editor.edit_drag.mode   = EDIT_DRAG_NONE;
            }
        }
    } else if (down && s->editor.edit_drag.active) {
        // Hold: update position or angle based on the drag mode.
        switch (s->editor.edit_drag.mode) {
            case EDIT_DRAG_FREE: {
                Vec2 delta = hierpos_diff(&cursor_hp, &s->editor.edit_drag.drag_anchor, BS_HIERPOS_CELL_SIZE);
                edit_entity_set_position(s, s->editor.edit_selection,
                                         hierpos_add_vec2(&s->editor.edit_drag.entity_anchor, delta));
                break;
            }
            case EDIT_DRAG_AXIS_X: {
                Vec2 delta = hierpos_diff(&cursor_hp, &s->editor.edit_drag.drag_anchor, BS_HIERPOS_CELL_SIZE);
                edit_entity_set_position(s, s->editor.edit_selection,
                                         hierpos_add_vec2(&s->editor.edit_drag.entity_anchor, Vec2{ delta.x, 0.0f }));
                break;
            }
            case EDIT_DRAG_AXIS_Y: {
                Vec2 delta = hierpos_diff(&cursor_hp, &s->editor.edit_drag.drag_anchor, BS_HIERPOS_CELL_SIZE);
                edit_entity_set_position(s, s->editor.edit_selection,
                                         hierpos_add_vec2(&s->editor.edit_drag.entity_anchor, Vec2{ 0.0f, delta.y }));
                break;
            }
            case EDIT_DRAG_ROTATE: {
                HierPos2 origin = edit_entity_position(s, s->editor.edit_selection);
                Vec2 a_rel = hierpos_diff(&s->editor.edit_drag.drag_anchor, &origin, BS_HIERPOS_CELL_SIZE);
                Vec2 c_rel = hierpos_diff(&cursor_hp, &origin, BS_HIERPOS_CELL_SIZE);
                f32 start_a = atan2f(a_rel.y, a_rel.x);
                f32 cur_a   = atan2f(c_rel.y, c_rel.x);
                f32 new_a   = s->editor.edit_drag.entity_angle + (cur_a - start_a);
                edit_entity_set_angle(s, s->editor.edit_selection, new_a);
                break;
            }
            default: break;
        }
    } else if (!down) {
        s->editor.edit_drag.active = FALSE;
        s->editor.edit_drag.mode   = EDIT_DRAG_NONE;
    }
}
