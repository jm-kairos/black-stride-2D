#include "sim/camera_controller.h"
#include "game.h"
#include "core/view_transform.h" // compression_factor, game_screen_to_true_hierpos, game_camera_center_hierpos
#include <core/input.h>          // input_get_mouse_wheel / input_get_mouse_position
#include <math.h> // powf, logf, expf, fabsf

using namespace bs_math;

// ---- Camera / zoom ---- (constants used only by the zoom controller)
static const f32 ZOOM_MIN        = 0.08f;  // most zoomed-out (global)
static const f32 ZOOM_MAX        = 12.00f; // most zoomed-in
static const f32 ZOOM_STEP       = 1.12f;  // multiplicative per wheel notch
static const f32 ZOOM_GLOBAL_MIN = 0.000004f; // most zoomed-out allowed: matches compression_factor's min_zoom so the wheel can reach galaxy level (systems sit 1e8-2e9 units apart)

void update_zoom_and_mode(game_state* s, f32 dt) {
    // The wheel nudges a TARGET zoom; the actual camera zoom eases toward it each frame so zooming
    // feels smooth instead of snapping. Easing is done in LOG space because zoom is multiplicative
    // and spans many decades (ZOOM_GLOBAL_MIN..ZOOM_MAX) -> constant *perceived* zoom speed.
    i32 wheel = input_get_mouse_wheel();
    if (wheel != 0) {
        s->camera_state.target_zoom = clampf(s->camera_state.target_zoom * powf(ZOOM_STEP, (f32)wheel), ZOOM_GLOBAL_MIN, ZOOM_MAX);
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
    // Crossing the arena <-> galaxy-map threshold updates the render "look" label and detaches the
    // camera so the new look is immediately pannable. Coordinates already agree across the boundary
    // (Step 1), so this is a pure label change plus a free-camera hand-off -- no re-anchor, no jump.
    if (now_global != was_global) {
        s->view.mode               = now_global ? MODE_GLOBAL : MODE_SYSTEM;
        s->camera_state.free_camera_pos    = game_camera_center_hierpos(s);
        s->camera_state.free_camera_active = TRUE;
        if (now_global) {
            s->render.global_background.notify_system_changed(s->galaxy.current_system);
        }
    }
    // Detached (free camera or edit) -> zoom toward the cursor by pinning P. Following the piloted
    // ship -> zoom about the ship, since a pan would be undone when the camera re-centers next frame.
    if (s->camera_state.free_camera_active || s->editor.edit_mode_active) {
        // A true-world point W maps to screen (centered, y-up) as cursor_view = zoom*comp*(W - C),
        // where C is the true-world point at screen center. Pin P: C = P - cursor_view/(zoom*comp).
        f32  comp_new   = compression_factor(new_zoom);
        HierPos2 center_new = hierpos_add_vec2(&P, vec2_scale(cursor_view, -1.0f / (new_zoom * comp_new)));
        if (s->camera_state.free_camera_active) {
            s->camera_state.free_camera_pos = center_new;
        } else {
            // Edit mode: store as floating-origin anchor + render-space residual (kept tiny).
            s->camera_state.camera_hierpos  = center_new;
            s->camera_state.camera.position = Vec2{ 0.0f, 0.0f };
        }
    }
}
