#include "sim/camera_controller.h"
#include "game.h"
#include "core/view_transform.h" // game_screen_to_true_hierpos, game_camera_center_hierpos
#include "sim/ship_control.h"    // piloted_ship_origin (on-screen test when restoring piloting)
#include <core/input.h>          // input_get_mouse_wheel / input_get_mouse_position
#include <math.h> // powf, logf, expf, fabsf

using namespace bs_math;

// ---- Camera / zoom ---- (constants used only by the zoom controller)
static const f32 ZOOM_MIN        = 0.08f;  // most zoomed-out (global)
static const f32 ZOOM_MAX        = 12.00f; // most zoomed-in
static const f32 ZOOM_STEP       = 1.12f;  // multiplicative per wheel notch
// Most zoomed-out allowed. Positions render linearly (no cosmetic compression), so the disc renders
// at radius ~= GALAXY_DISC_RMAX(~3.2e11) * zoom with no floor. To still frame the whole disc
// (~a few hundred px) the wheel must pull back far, so this is set low (7.5e-10). The extra decades
// are traversed quickly via g_zoom_out_speed_gain below.
static const f32 ZOOM_GLOBAL_MIN = 7.5e-10f;
// Below this zoom the per-notch step grows (progressive zoom-out speed) so pulling all the way back
// to the galaxy overview stays quick despite the extended range; at/above it, speed is unchanged.
static const f32 ZOOM_SPEED_RAMP = 0.02f;

void update_zoom_and_mode(game_state* s, f32 dt) {
    // The wheel nudges a TARGET zoom; the actual camera zoom eases toward it each frame so zooming
    // feels smooth instead of snapping. Easing is done in LOG space because zoom is multiplicative
    // and spans many decades (ZOOM_GLOBAL_MIN..ZOOM_MAX) -> constant *perceived* zoom speed.
    i32 wheel = input_get_mouse_wheel();
    if (wheel != 0) {
        f32 step = ZOOM_STEP;
        // Zooming IN toward a captured planet: ease the zoom-in speed down as it nears its framed
        // size (planet_approach.zoom_damp), so the camera decelerates into the planet.
        if (wheel > 0 && s->planet_approach.engaged) {
            f32 slow = 1.0f - 0.85f * s->planet_approach.zoom_damp;
            step = 1.0f + (ZOOM_STEP - 1.0f) * slow;
        }
        // Progressive zoom-out speed: the further out we already are (below ZOOM_SPEED_RAMP), the more
        // zoom each wheel notch covers, so the extended zoom-out range stays quick
        // to traverse in both directions. far_t ramps 0..1 in LOG space from ZOOM_SPEED_RAMP down to
        // ZOOM_GLOBAL_MIN; at/above the ramp zoom the speed is unchanged (far_boost == 1).
        f32 far_boost = 1.0f;
        if (s->camera_state.target_zoom < ZOOM_SPEED_RAMP) {
            f32 span = logf(ZOOM_SPEED_RAMP) - logf(ZOOM_GLOBAL_MIN);
            f32 far_t = span > 1.0e-6f
                        ? (logf(ZOOM_SPEED_RAMP) - logf(s->camera_state.target_zoom)) / span : 1.0f;
            far_boost = 1.0f + g_zoom_out_speed_gain * clampf(far_t, 0.0f, 1.0f);
        }
        s->camera_state.target_zoom = clampf(s->camera_state.target_zoom * powf(step, (f32)wheel * far_boost), ZOOM_GLOBAL_MIN, ZOOM_MAX);
    }
    f32 old_zoom = s->camera_state.camera.zoom;
    if (old_zoom <= 0.0f) old_zoom = ZOOM_GLOBAL_MIN;
    // Advance toward the target in log space (frame-rate independent). Snap when essentially there.
    f32 new_zoom;
    if (fabsf(logf(s->camera_state.target_zoom) - logf(old_zoom)) < 1e-4f) {
        new_zoom = s->camera_state.target_zoom;
    } else {
        f32 alpha   = 1.0f - expf(-s->camera_state.zoom_smooth_rate * dt);
        f32 log_new = logf(old_zoom) + (logf(s->camera_state.target_zoom) - logf(old_zoom)) * alpha;
        new_zoom    = expf(log_new);
    }
    // Idle: nothing is animating this frame, so skip the cursor-pin work (it would fight pan/follow).
    if (new_zoom == old_zoom) return;
    // True-world point under the cursor, captured BEFORE the zoom change so it can be pinned.
    i32 mx, my; input_get_mouse_position(&mx, &my);
    Vec2 cursor      = Vec2{ (f32)mx, (f32)my };
    Vec2 cursor_view = Vec2{ cursor.x - (f32)s->fb_width * 0.5f,
                             (f32)s->fb_height * 0.5f - cursor.y };
    HierPos2 P = game_screen_to_true_hierpos(s, cursor);
    // Crossing ZOOM_MIN flips the render "look" (arena <-> galaxy map). Both looks now share ONE
    // coordinate space, so the flip is a pure label change plus a free-camera hand-off -- there is
    // no re-anchor, and the zoom itself stays continuous across the boundary.
    b8 was_global = (s->view.mode == MODE_GLOBAL);
    b8 now_global = (new_zoom >= ZOOM_MIN);
    s->camera_state.camera.zoom = new_zoom;
    // Crossing the arena <-> galaxy-map threshold updates the render "look" label. Coordinates
    // already agree across the boundary (Step 1), so this is a pure label change plus a control
    // hand-off -- no re-anchor, no jump. The hand-off remembers the player's control INTENT so the
    // round trip is reversible: leaving MODE_GLOBAL snapshots piloting-vs-free-camera, and re-
    // entering restores it instead of always forcing free camera on (the old one-way behaviour).
    if (now_global != was_global) {
        s->view.mode = now_global ? MODE_GLOBAL : MODE_SYSTEM;
        if (!now_global) {
            // Leaving the arena for the galaxy map: force free camera on (the map is browsed with a
            // detached, pannable camera). The control INTENT (global_free_camera_saved) is NOT
            // snapshotted here -- it is maintained live at the deliberate toggle sites, so the
            // temporary off-screen free-camera fallback can't overwrite a piloting intent.
            s->camera_state.free_camera_pos    = game_camera_center_hierpos(s);
            s->camera_state.free_camera_active = TRUE;
        } else {
            // Re-entering the arena from the galaxy map: restore the remembered control intent.
            s->render.global_background.notify_system_changed(s->galaxy.current_system);
            if (s->camera_state.global_free_camera_saved) {
                // Was in free camera / RTS -> keep it detached where the view currently sits.
                s->camera_state.free_camera_pos    = game_camera_center_hierpos(s);
                s->camera_state.free_camera_active = TRUE;
            } else {
                // Was piloting. If the piloted ship is on-screen at the crossing, glide the camera
                // onto it (TAB-style recenter that ends in ship-follow). Otherwise drop into free
                // camera + autopilot instead of yanking the view across space to a distant ship.
                HierPos2 center = game_camera_center_hierpos(s);
                HierPos2 ship   = piloted_ship_origin(s);
                Vec2 d    = hierpos_diff(&ship, &center, BS_HIERPOS_CELL_SIZE);
                Vec2 scr  = vec2_scale(d, new_zoom); // centered screen px (matches cursor_view)
                b8 on_screen = (fabsf(scr.x) <= (f32)s->fb_width  * 0.5f) &&
                               (fabsf(scr.y) <= (f32)s->fb_height * 0.5f);
                if (on_screen) {
                    // Reuse the recenter glide: detached during the ease, then it hands control
                    // back to ship-follow (free_camera_active = FALSE) on completion.
                    s->camera_state.recentering       = TRUE;
                    s->camera_state.recenter_t        = 0.0f;
                    s->camera_state.recenter_from_pos = center;
                    s->camera_state.free_camera_pos    = center;
                    s->camera_state.free_camera_active = TRUE;
                } else {
                    // Ship off-screen: free camera + autopilot, anchored at the current view.
                    s->camera_state.free_camera_pos    = center;
                    s->camera_state.free_camera_active = TRUE;
                }
            }
        }
    }
    // Detached (free camera or edit) -> zoom TOWARD THE CURSOR by pinning the point under it.
    // Following the piloted ship -> zoom about the ship. While a planet is captured the approach
    // block re-centres on the planet, so skip the cursor-pin then (it would fight the follow).
    if (s->camera_state.free_camera_active || s->editor.edit_mode_active) {
        // A true-world point W maps to screen (centered, y-up) as cursor_view = zoom*(W - C),
        // where C is the true-world point at screen center. Pin P: C = P - cursor_view/zoom.
        HierPos2 center_new = hierpos_add_vec2(&P, vec2_scale(cursor_view, -1.0f / new_zoom));
        if (s->camera_state.free_camera_active) {
            // Captured planet, OR actively zooming toward a candidate planet -> the approach block
            // homes the zoom on the planet; skip the cursor-pin so the parallax doesn't drift it off
            // (the shoot-off at low parallax-fade). Normal cursor-pin zoom otherwise.
            if (!(s->planet_approach.engaged || s->planet_approach.candidate))
                s->camera_state.free_camera_pos = center_new;
        } else {
            // Edit mode: store as floating-origin anchor + render-space residual (kept tiny).
            s->camera_state.camera_hierpos  = center_new;
            s->camera_state.camera.position = Vec2{ 0.0f, 0.0f };
        }
    }
}
