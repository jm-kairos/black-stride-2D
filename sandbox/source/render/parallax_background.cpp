#include "render/parallax_background.h"

#include "game.h"
#include "core/view_transform.h"    // game_camera_center
#include "sim/celestial_parallax.h" // celestial_center_render
#include <renderer/renderer.h>
#include <renderer/camera2d.h>

using namespace bs_math;

// Background layers are driven by the absolute camera center (not the render-space residual
// camera.position) so they track the ship under the floating-origin path. MappedSystemLayer
// (parallax 1.0) re-derives its residual from camera_hierpos, so this single center works for
// every layer.
static Camera2D bg_cam_for_parallax(const game_state* s) {
    Camera2D c = s->camera_state.camera;
    c.position = game_camera_center(s);
    return c;
}

void draw_parallax_background(game_state* s, f32 dt) {
    // Update current star world position for the dazzle effect. star_pos is the star's
    // render-space offset from the camera center (already relative to camera_hierpos).
    if (s->galaxy.current_system >= 0 && s->galaxy.current_system < s->galaxy.system_count) {
        StarSystem& ss = s->galaxy.systems[s->galaxy.current_system];
        Vec2 sys_pos = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star);
        s->star_pos = vec2_add(sys_pos, ss.star.position);
    } else {
        s->star_pos = bs_math::Vec2{0,0};
    }

    s->profiler.begin(PROF_BACKGROUND);
    s->render.global_background.draw(bg_cam_for_parallax(s), s->fb_width, s->fb_height, dt, s->elapsed_time);
    // global_background restores the camera to whatever it was passed; that is the effective-
    // center camera (for parallax scroll), so put the render-space camera back for the batched
    // sprite passes that follow.
    renderer_set_camera(s->camera_state.camera);
    s->profiler.end(PROF_BACKGROUND);
}
