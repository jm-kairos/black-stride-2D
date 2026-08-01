#include "render/frame_lighting.h"

#include "game.h"
#include "sim/celestial_parallax.h" // celestial_center_render
#include "render/star_fx.h"
#include "core/render_layers.h"
#include <renderer/renderer.h>
#include <math.h>

using namespace bs_math;

void submit_frame_lighting(game_state* s) {
    // Volumetric star light accumulator (filled in the galaxy-map look, consumed here).
    bs_light2d star_light{};
    b8 has_star_light = FALSE;

    // ---- Compute dynamic bloom / glow from ship speed -------------------
    // Speed-driven dynamic bloom is an arena-look effect; weight it by arena weight so it eases
    // out across the blend band instead of switching off at the discrete mode boundary.
    f32 speed_ratio = 0.0f;
    if (s->view_arena_w > 0.0f) {
        f32 speed = vec2_length(s->player_flight().velocity);
        speed_ratio = clampf(speed / s->player_ship().motion.max_speed, 0.0f, 1.0f) * s->view_arena_w;
    }
    bs_glow_params render_glow = s->render.glow_params;
    f32 render_bloom_intensity = s->render.bloom_intensity;
    if (s->render.dynamic_bloom) {
        render_glow.intensity += speed_ratio * 1.0f;
        render_bloom_intensity += speed_ratio * 0.5f;
    }

    // ---- Unified star light + ambient cross-fade (Step 2C) ----------------
    // The current system's volumetric star light and the map's bright ambient
    // are a MAP-look feature; the arena look is fullbright with directional ship
    // shading. Cross-fade both by map weight (1 - arena weight) so a slow zoom
    // through the blend band transitions seamlessly with no pop at the discrete
    // render-mode boundary. At map_w == 0 (deep arena) no star light is added,
    // matching the original fullbright arena; at map_w == 1 (galaxy map) it
    // reproduces the original volumetric-lit map exactly.
    f32 map_w = 1.0f - s->view_arena_w;
    if (s->render.star_light_enabled && map_w > 0.0f &&
        s->galaxy.current_system >= 0 && s->galaxy.current_system < s->galaxy.system_count) {
        const StarSystem& css = s->galaxy.systems[s->galaxy.current_system];
        Vec2 c_sys_pos_raw = hierpos_diff(&css.galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        f32 c_vis = get_sensor_visibility(s, c_sys_pos_raw);
        // Depth parallax so the volumetric star light aligns with the drawn (parallaxed) star.
        Vec2 c_sys_pos = celestial_center_render(s, &css.galaxy_center, &s->celestial_anchor, s->render.depth_star);
        Vec2 c_star_pos = vec2_add(c_sys_pos, css.star.position);
        f32 c_max_orbit = 0.0f;
        for (i32 i = 0; i < css.planet_count; ++i) {
            c_max_orbit = fmaxf(c_max_orbit, css.planets[i].semi_major_axis);
        }
        star_light = make_star_light(c_star_pos, css.star.color, c_max_orbit, c_vis,
                                     s->render.star_light_intensity_mul * map_w, s->render.star_light_radius_mul);
        has_star_light = TRUE;
    }

    // Build frame-local light array: star light first (when in system view), then editor lights
    bs_light2d frame_lights[16];
    u32 frame_light_count = 0;
    if (has_star_light) {
        frame_lights[0] = star_light;
        frame_light_count = 1;
    }
    for (u32 i = 0; i < s->render.lights.size() && frame_light_count < 16; ++i) {
        frame_lights[frame_light_count++] = s->render.lights[i];
    }

    // When a star light is active, boost ambient so the galaxy map stays visible
    // (the default ambient is ~0.2, which makes the map 80% darker than fullbright).
    // Cross-fade the ambient from fullbright white (arena look, where the scene
    // renders fullbright with no point lights) toward the map's bright ambient by
    // map weight, so brightening in from the arena is seamless.
    bs_color frame_ambient = s->render.light_ambient;
    if (has_star_light) {
        frame_ambient = bs_color{
            1.0f + (0.85f - 1.0f) * map_w,
            1.0f + (0.88f - 1.0f) * map_w,
            1.0f + (0.95f - 1.0f) * map_w,
            1.0f };
    }

    renderer_set_lights(frame_light_count > 0 ? frame_lights : nullptr,
                        frame_light_count, frame_ambient, LAYER_UI);

    renderer_set_glow_params(&render_glow);
    renderer_set_bloom_enabled(s->render.bloom_enabled);
    renderer_set_bloom_params(s->render.bloom_threshold, render_bloom_intensity);
}
