#include "mapped_system_layer.h"
#include "game.h"
#include "star_fx.h"
#include <math/bs_hierpos.h>
#include <renderer/camera2d.h>
#include <renderer/renderer.h>
#include <math.h>
using namespace bs_math;
// =====================================================================================
static b8 is_on_screen(const Camera2D* cam, u16 fb_w, u16 fb_h,
                       Vec2 world_pos, f32 radius)
{
    Vec2 screen = camera2d_world_to_screen(cam, fb_w, fb_h, world_pos);
    f32  screen_r = radius * cam->zoom;
    return (screen.x + screen_r > 0.0f && screen.x - screen_r < (f32)fb_w &&
            screen.y + screen_r > 0.0f && screen.y - screen_r < (f32)fb_h);
}
// =====================================================================================
static f32 get_sensor_visibility_global(const game_state* s, Vec2 pos)
{
    if (!s->map_draw_sensor_range) return 1.0f;
    f32 dist = vec2_length(vec2_sub(pos, s->player_ship().origin));
    return sensor_visibility_from_dist(dist, s->map_sensor_range);
}
// =====================================================================================
MappedSystemLayer::MappedSystemLayer(game_state* _gs, StarFxSystem* _star_fx)
    : ParallaxLayer(3, 0.20f, 1.0f, FALSE),
      gs(_gs), star_fx(_star_fx), current_system(-1)
{
}
// =====================================================================================
void MappedSystemLayer::on_system_changed(i32 system_idx)
{
    current_system = system_idx;
}
// =====================================================================================
void MappedSystemLayer::draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
                             f32 dt, f32 elapsed_time,
                             const bs_math::Vec2& blur)
{
    (void)blur;
    (void)dt;
    if (!gs) return;
    // Auto-detect if ship has jumped to a different system while in global mode.
    if (gs->current_system != current_system) {
        on_system_changed(gs->current_system);
    }
    if (current_system < 0) return;
    if (current_system >= gs->system_count) return;
    StarSystem& ss = gs->systems[current_system];
    // ---- Star world position (galaxy_center + local offset) ----
    Vec2 sys_world = hierpos_to_vec2(&ss.galaxy_center, BS_HIERPOS_CELL_SIZE);
    Vec2 star_world = vec2_add(sys_world, ss.star.position);
    // ---- Visibility based on distance from ship to system center ----
    f32 vis = get_sensor_visibility_global(gs, sys_world);
    if (vis <= 0.0f) return;
    // ---- Cull if off-screen ----
    if (!is_on_screen(&cam, fb_w, fb_h, star_world, ss.star.radius * 2.0f))
        return;
    // ---- Compute screen-space position for the star ----
    // Since parallax = 1.0, virtual camera = {0,0}, so standard transform.
    Vec2 star_screen = camera2d_world_to_screen(&cam, fb_w, fb_h, star_world);
    // ---- Same zoom-scale logic as MODE_SYSTEM ----
    f32 base_r = ss.star.radius * (0.3f + 0.7f * vis);
    f32 screen_r = base_r * cam.zoom;
    f32 zoom_scale = (screen_r < STAR_MIN_SCREEN_RADIUS)
                     ? (STAR_MIN_SCREEN_RADIUS / screen_r) : 1.0f;
    f32 total_scale = zoom_scale;
    f32 scaled_r = base_r * total_scale;
    // ---- Draw star with StarFxSystem ----
    f32 screen_radius = scaled_r * cam.zoom;
    // The aux-bloom pass is rendered with the main camera, so the proxy sprite needs a
    // world position that projects to the same screen position as the sunburst under the
    // parallax layer camera: aux_world_pos = star_world + main_cam_pos * (1 - parallax).
    Vec2 main_cam_pos = Vec2{ cam.position.x / parallax, cam.position.y / parallax };
    Vec2 aux_world_pos = vec2_add(star_world, vec2_scale(main_cam_pos, 1.0f - parallax));
    renderer_set_aux_bloom_mode(star_fx->streak_enabled);
    renderer_set_streak_source(star_screen);
    star_fx->draw_star(ss, star_world, star_screen, scaled_r, screen_radius, vis,
                       elapsed_time, id, fb_w, fb_h, total_scale, aux_world_pos);
    renderer_set_aux_bloom_mode(FALSE);
    // ---- Planets: true world positions ----
    for (i32 p = 0; p < ss.planet_count; ++p) {
        CelestialBody& planet = ss.planets[p];
        Vec2 planet_world = vec2_add(sys_world, planet.position);
        if (!is_on_screen(&cam, fb_w, fb_h, planet_world, planet.radius * 2.0f))
            continue;
        f32 pvis = get_sensor_visibility_global(gs, planet_world);
        if (pvis <= 0.0f) continue;
        bs_color pcol = planet.color;
        pcol.a *= pvis;
        f32 pr = planet.radius * cam.zoom * zoom_scale;
        if (pr < 1.0f) pr = 1.0f;
        renderer_draw_circle(planet_world, pr, 16, 1.0f, pcol, id);
    }
}
