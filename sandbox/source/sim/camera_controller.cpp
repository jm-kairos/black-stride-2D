#include "sim/camera_controller.h"
#include "game.h"
#include "core/view_transform.h" // game_screen_to_true_hierpos
// sim/ship_control.h dropped with the zoom-driven control hand-off: piloted_ship_origin was only
// needed for the on-screen test that decided whether to restore piloting on an inbound crossing.
#include <core/input.h>          // input_get_mouse_wheel / input_get_mouse_position
#include <math.h> // powf, logf, expf, fabsf

using namespace bs_math;

// ---- Camera / zoom ---- (constants used only by the zoom controller)
// Arena <-> galaxy-map flip point. MUST stay straddled by [VIEW_MAP_ZOOM, VIEW_ARENA_ZOOM] in
// core/view_transform.cpp -- that band is the visual cross-fade, this is the label flip and the
// free-camera hand-off, and nothing but the comments at both sites ties them together. Widened
// from 0.08 so the compressed ballistic engagement (19k-58k units of reach) frames INSIDE the
// arena look: half the screen spans 640/zoom world units, so 0.015 reaches ~42,700 units.
// Crossing this still force-detaches the camera, which is now meaningful rather than arbitrary --
// below it you are in the long-range missile game, which is played detached.
static const f32 ZOOM_MIN        = 0.015f; // most zoomed-out arena look (global)
static const f32 ZOOM_MAX        = 12.00f; // most zoomed-in
static const f32 ZOOM_STEP       = 1.12f;  // multiplicative per wheel notch
// Most zoomed-out allowed. Positions render linearly (no cosmetic compression), so the disc renders
// at radius ~= GALAXY_DISC_RMAX(~3.2e11) * zoom with no floor. To still frame the whole disc
// (~a few hundred px) the wheel must pull back far, so this is set low (7.5e-10). The extra decades
// are traversed quickly via g_zoom_out_speed_gain below.
static const f32 ZOOM_GLOBAL_MIN = 7.5e-10f;
// Below this zoom the per-notch step grows (progressive zoom-out speed) so pulling all the way back
// to the galaxy overview stays quick despite the extended range; at/above it, speed is unchanged.
// Kept BELOW VIEW_MAP_ZOOM (0.009): at its old 0.02 it now sat inside the widened arena band, so
// the wheel would have accelerated while the player was still framing a ballistic fight. The ramp
// is for traversing the empty decades out to the galaxy overview, not for combat zoom.
static const f32 ZOOM_SPEED_RAMP = 0.005f;

void update_zoom_and_mode(game_state* s, f32 dt) {
    // The wheel nudges a TARGET zoom; the actual camera zoom eases toward it each frame so zooming
    // feels smooth instead of snapping. Easing is done in LOG space because zoom is multiplicative
    // and spans many decades (ZOOM_GLOBAL_MIN..ZOOM_MAX) -> constant *perceived* zoom speed.
    i32 wheel = input_get_mouse_wheel();
    // The ship inspector is modal: still CONSUME the wheel accumulator (the engine clears it
    // per poll), but discard it so the world zoom cannot change under the window.
    if (s->show_flagship_inspector) wheel = 0;
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
    // Crossing ZOOM_MIN flips the render "look" (arena <-> galaxy map). Both looks share ONE
    // coordinate space, so the flip is a PURE LABEL CHANGE -- no re-anchor, no jump, and the zoom
    // itself stays continuous across the boundary.
    b8 was_global = (s->view.mode == MODE_GLOBAL);
    b8 now_global = (new_zoom >= ZOOM_MIN);
    s->camera_state.camera.zoom = new_zoom;
    // ZOOM NO LONGER DECIDES PILOT vs AUTO-PILOT. TAB (and the HUD button) are the only things
    // that attach or detach the camera; the wheel only ever changes the wheel's business.
    //
    // This used to force the free camera ON when leaving the arena and then restore the remembered
    // intent on the way back, which needed `global_free_camera_saved`, an on-screen test and two
    // deliberately asymmetric crossing paths -- about thirty lines whose entire job was to undo the
    // surprise the coupling created. Scrolling reads as "I want to see more", not "take the helm",
    // and since the ballistic engagement envelope was compressed the tactical picture sits one
    // scroll outside the arena band -- so the old behaviour handed control away mid-fight exactly
    // when the player zoomed out to look at the fight.
    //
    // What the player gives up is that the galaxy map is only PANNABLE while detached (cursor-pin
    // zoom, WASD pan and edge pan all require it, and an attached camera is pinned to the ship), so
    // browsing costs one TAB press. That is a keypress, not a mode you have to discover.
    if (now_global != was_global) {
        s->view.mode = now_global ? MODE_GLOBAL : MODE_SYSTEM;
        // Re-entering the arena still has to tell the backdrop which system it is drawing. This is
        // a RENDER side effect that merely happened to live in the control hand-off; it is not part
        // of it, and dropping it with the rest would silently leave a stale parallax backdrop.
        if (now_global)
            s->render.global_background.notify_system_changed(s->galaxy.current_system);
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
