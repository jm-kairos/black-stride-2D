#include "mapped_system_layer.h"
#include "game.h"
// Explicit peer module includes (no longer via the game.h cascade).
#include "core/view_transform.h"    // compression_factor
#include "sim/celestial_parallax.h" // celestial_center_render
#include "star_fx.h"
#include <math/bs_hierpos.h>
#include <renderer/camera2d.h>
#include <renderer/renderer.h>
#include <math.h>
using namespace bs_math;
// =====================================================================================
// compression_factor now lives in core/view_transform.cpp (declared via core/view_transform.h,
// pulled in through game.h) so every galaxy-view layer shares one compression curve.
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
    if (!s->galaxy.map_draw_sensor_range) return 1.0f;
    bs_math::HierPos2 pos_hp = bs_math::hierpos_from_vec2(pos, bs_math::BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(hierpos_diff(&pos_hp, &s->player_ship().origin, bs_math::BS_HIERPOS_CELL_SIZE));
    return sensor_visibility_from_dist(dist, s->galaxy.map_sensor_range);
}
// =====================================================================================
MappedSystemLayer::MappedSystemLayer(game_state* _gs, StarFxSystem* _star_fx)
    // Parallax 1.0 (world-locked): the star is a real object at a real world position that the
    // fleet operates inside, so it must track the camera 1:1. This also matches how system mode
    // draws the star (no parallax), so there is no star jump when switching modes.
    : id(3), parallax(1.0f), zoom_scale(1.0f), is_custom_gpu(FALSE),
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
    // The star we render is the one under the CAMERA, not the ship. When the ship is inside a
    // system these coincide, but when the free camera pans to a remote system (or the map->arena
    // zoom blend happens while looking at another system) the camera's system is what must appear
    // in the arena look. Using the ship's current_system here drew the ship's star ~1e8 units
    // off-screen (culled) while the map renderer faded out the viewed system's star, so the star
    // appeared to "fade to nothing" when entering arena mode over a distant system.
    i32 cam_system = find_system_by_cell(&gs->camera_state.camera_hierpos, &gs->galaxy.galaxy_voronoi, gs->galaxy.systems);
    if (cam_system != current_system) {
        on_system_changed(cam_system);
    }
    if (current_system < 0) return;
    if (current_system >= gs->galaxy.system_count) return;
    StarSystem& ss = gs->galaxy.systems[current_system];
    // The frame's batched sprites are drawn in render space (world - camera_hierpos) with the main
    // camera positioned at the render-space residual. This layer's cam is the absolute-center camera
    // used for parallax scroll, so re-derive the residual camera (dcam) and a world offset that
    // shifts our true galaxy positions into that same render space.
    Camera2D dcam = cam;
    // Absolute galaxy position of this system's star (used only for coarse sensor-visibility).
    Vec2 sys_world = hierpos_to_vec2(&ss.galaxy_center, BS_HIERPOS_CELL_SIZE);
    // Render-space position of the system center (relative to camera_hierpos).
    // Use the precise render-space residual (raw camera.position == game_camera_center -
    // camera_hierpos) and integer HierPos differencing for the star, instead of subtracting
    // two ~1e9 absolute floats. That cancellation loses all precision in far systems (f32 ULP
    // is ~256 units at 2e9), so the star jitters and gets culled off-screen as you approach it
    // -- it "suddenly vanishes." hierpos_diff does the cell subtraction in integer space first.
    dcam.position = gs->camera_state.camera.position;
    // Depth-based parallax: the star and planets are offset via celestial_center_render, which
    // parallaxes them against the frame's SHARED anchor (s->celestial_anchor) exactly like the Map
    // renderer, so the backdrop does not shift or split across the map<->arena cross-fade. depth=0,
    // the master toggle off, or the zoom-fade at map scale all reduce these to the plain
    // hierpos_diff, i.e. the original world-locked behaviour.
    Vec2 sys_rel_star   = celestial_center_render(gs, &ss.galaxy_center, &gs->celestial_anchor, gs->render.depth_star);
    Vec2 sys_rel_planet = celestial_center_render(gs, &ss.galaxy_center, &gs->celestial_anchor, gs->render.depth_planet);
    // ---- Star world position (galaxy_center + local offset), in render space ----
    Vec2 star_world = vec2_add(sys_rel_star, ss.star.position);
    // ---- Cosmetic compression (match the galaxy-map view) ----
    // The galaxy map pulls distant systems toward the camera via cosmetic_compress when zoomed
    // out. Apply the same factor here (relative to the camera) so the star sits in the same place
    // in global mode as it does in system mode. compression_factor == 1.0 at normal zoom, so this
    // is a no-op except at extreme zoom-out.
    f32 comp = compression_factor(dcam.zoom);
    star_world = vec2_add(dcam.position, vec2_scale(vec2_sub(star_world, dcam.position), comp));
    // ---- Visibility based on distance from ship to system center ----
    f32 vis = get_sensor_visibility_global(gs, sys_world);
    if (vis <= 0.0f) return;
    // ---- Cull if off-screen ----
    if (!is_on_screen(&dcam, fb_w, fb_h, star_world, ss.star.radius * 2.0f))
        return;
    // ---- Compute screen-space position for the star ----
    // Since parallax = 1.0, virtual camera = {0,0}, so standard transform.
    Vec2 star_screen = camera2d_world_to_screen(&dcam, fb_w, fb_h, star_world);
    // ---- Same zoom-scale logic as MODE_SYSTEM ----
    f32 base_r = ss.star.radius * (0.3f + 0.7f * vis);
    // In 3D sphere mode the visible body is star_body_scale x larger, so base the min-screen-radius
    // floor on the actual sphere size. Otherwise the floor pins the sphere to a constant on-screen
    // size at every zoom level (it never appears to grow as you approach the star).
    f32 body_scale = star_fx->star_3d_mode ? star_fx->star_body_scale : 1.0f;
    f32 screen_r = base_r * body_scale * dcam.zoom;
    f32 zoom_scale = (screen_r < STAR_MIN_SCREEN_RADIUS)
                     ? (STAR_MIN_SCREEN_RADIUS / screen_r) : 1.0f;
    // Keep the current (hero) star prominent so its sunburst rays/streaks stay visible across
    // the arena<->map boundary. Faded in by map weight (map_w = 1 - view_arena_w) so the arena
    // look is unchanged; the map renderer applies the identical floor for continuity.
    // Skipped in 3D sphere mode: the big procedural sphere should scale with true zoom (shrinking
    // to a small dot on the galaxy map) rather than be pinned to the 42px hero minimum.
    if (!star_fx->star_3d_mode && screen_r > 0.0f) {
        f32 map_w = 1.0f - gs->view_arena_w;
        f32 hero_min = STAR_MIN_SCREEN_RADIUS
                       + (STAR_HERO_MAP_MIN_RADIUS - STAR_MIN_SCREEN_RADIUS) * map_w;
        if (screen_r < hero_min) {
            f32 hero_scale = hero_min / screen_r;
            if (hero_scale > zoom_scale) zoom_scale = hero_scale;
        }
    }
    // Match the galaxy-map star's edge-aberration scaling so the star transitions seamlessly
    // across the arena<->map boundary. dist_scale is a map-look feature: fade it toward 1.0 by
    // arena weight (map_w = 1 - view_arena_w). At full arena it is 1.0 (unchanged arena look);
    // in the blend band it ramps to the same value the map renderer uses at the boundary.
    Vec2 screen_center = Vec2{ (f32)fb_w * 0.5f, (f32)fb_h * 0.5f };
    f32 dist_from_center = vec2_length(vec2_sub(star_screen, screen_center));
    f32 dist_scale = 1.0f + dist_from_center * STAR_DIST_SCALE_FACTOR;
    if (dist_scale > STAR_MAX_DIST_SCALE) dist_scale = STAR_MAX_DIST_SCALE;
    dist_scale = 1.0f + (dist_scale - 1.0f) * (1.0f - gs->view_arena_w);
    f32 total_scale = zoom_scale * dist_scale;
    f32 scaled_r = base_r * total_scale;
    // ---- Draw star with StarFxSystem ----
    f32 screen_radius = scaled_r * dcam.zoom;
    // The aux-bloom pass is rendered with the main camera, so the proxy sprite needs a
    // world position that projects to the same screen position as the sunburst under the
    // parallax layer camera: aux_world_pos = star_world + main_cam_pos * (1 - parallax).
    Vec2 main_cam_pos = Vec2{ dcam.position.x / parallax, dcam.position.y / parallax };
    Vec2 aux_world_pos = vec2_add(star_world, vec2_scale(main_cam_pos, 1.0f - parallax));
    // Cross-fade this arena star (+ its planets) out toward the galaxy-map look by arena weight.
    // The galaxy-map renderer draws the same current star faded by map_w; additive blending sums
    // the two to a continuous full-intensity star across the blend band.
    f32 arena_a = gs->view_arena_w;
    renderer_set_draw_alpha(arena_a);
    renderer_set_aux_bloom_mode(star_fx->streak_enabled);
    renderer_set_streak_source(star_screen);
    star_fx->draw_star(ss, star_world, star_screen, scaled_r, screen_radius, vis * arena_a,
                       gs->galaxy.galaxy_map_time, id, fb_w, fb_h, total_scale, aux_world_pos);
    renderer_set_aux_bloom_mode(FALSE);
    // ---- Planets: true world positions ----
    for (i32 p = 0; p < ss.planet_count; ++p) {
        CelestialBody& planet = ss.planets[p];
        Vec2 planet_true  = vec2_add(sys_world, planet.position); // absolute (sensor vis)
        Vec2 planet_world = vec2_add(sys_rel_planet, planet.position);   // render space (precise)
        planet_world = vec2_add(dcam.position, vec2_scale(vec2_sub(planet_world, dcam.position), comp));
        if (!is_on_screen(&dcam, fb_w, fb_h, planet_world, planet.radius * 2.0f))
            continue;
        f32 pvis = get_sensor_visibility_global(gs, planet_true);
        if (pvis <= 0.0f) continue;
        bs_color pcol = planet.color;
        pcol.a *= pvis;
        f32 pr = planet.radius * dcam.zoom * zoom_scale;
        if (pr < 1.0f) pr = 1.0f;
        renderer_draw_circle(planet_world, pr, 16, 1.0f, pcol, id);
    }
    // Restore full opacity for subsequent passes (this is the last background layer).
    renderer_set_draw_alpha(1.0f);
}
