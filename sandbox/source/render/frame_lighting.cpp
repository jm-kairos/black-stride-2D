#include "render/frame_lighting.h"

#include "game.h"
#include "sim/celestial_parallax.h" // celestial_center_render
#include "render/star_fx.h"
#include "core/render_layers.h"
#include "core/view_transform.h"    // render_from_hierpos (light positions, computed fresh)
#include "core/projectile_fx.h"     // ProjectileFxEvent ring (weapon-flash lights)
#include <renderer/renderer.h>
#include <math.h>

using namespace bs_math;

// ---- Dynamic ship lights: thruster burns and weapon flashes -----------------------------
// Every light source the scene draws also LIGHTS the scene: a burning main drive throws
// warm light onto its own stern plating and nearby hulls, and a muzzle flash / impact /
// flak burst strobes the hull that produced it. The mapped-hull shader shades these
// per-pixel against the ship's normal map; lit sprite runs pick them up too. Positions are
// computed fresh from hierpos here (Ship::render_pos is written later, by draw_ship_scene),
// so the lights track this frame's camera anchor exactly.
//
// Budget: shares the engine's 16-light array with the star + editor lights. One light per
// burning main-drive cluster (not per nozzle) keeps a 5-ship fleet at 5 slots; FX-ring
// flashes fill what remains, newest first by ring order. NPC drives are skipped for now —
// they fly distant trade lanes and would starve the fleet's slots.
static u32 collect_dynamic_ship_lights(game_state* s, bs_light2d* out, u32 max_out) {
    u32 count = 0;

    // ---- Main-drive burn light, one per fleet ship --------------------------------------
    // Same projection as draw_engine_exhaust: commanded thrust (thrust_vis/turn_vis
    // telemetry, never velocity) against each authored MAIN nozzle, so the light burns
    // exactly when the flames draw and goes dark on a coasting hull.
    for (i32 i = 0; i < s->fleet_state.fleet.count() && count < max_out; ++i) {
        const FleetShip& fs = s->fleet_state.fleet.at(i);
        const Ship* ship = &fs.ship;
        const ShipFlight* fl = &fs.flight;
        if (!ship->def || ship->def->thruster_count <= 0) continue;

        Vec2 T = Vec2{ clampf(fl->thrust_vis.x, -1.0f, 1.0f),
                       clampf(fl->thrust_vis.y, -1.0f, 1.0f) };
        f32  u = clampf(fl->turn_vis, -1.0f, 1.0f);

        Vec2 pos_sum  = Vec2{ 0.0f, 0.0f };
        Vec2 dir_sum  = Vec2{ 0.0f, 0.0f };
        f32  burn_sum = 0.0f;
        f32  burn_max = 0.0f;
        for (i32 t = 0; t < ship->def->thruster_count; ++t) {
            const ThrusterDef& td = ship->def->thrusters[t];
            if (td.kind != THRUSTER_MAIN) continue;
            Vec2 e = vec2_rotate(Vec2{ 0.0f, 1.0f }, td.facing);  // exhaust direction, local
            Vec2 f = Vec2{ -e.x, -e.y };                          // force it puts on the hull
            f32 burn = fmaxf(0.0f, T.x * f.x + T.y * f.y);
            f32 pl = vec2_length(td.pos_local);
            if (pl > 1.0e-3f) {
                f32 tau = (td.pos_local.x * f.y - td.pos_local.y * f.x) / pl;
                burn += fmaxf(0.0f, u * tau);
            }
            burn = clampf(burn, 0.0f, 1.0f);
            if (burn <= 0.02f) continue;
            pos_sum   = vec2_add(pos_sum, vec2_scale(td.pos_local, burn));
            dir_sum   = vec2_add(dir_sum, vec2_scale(e, burn));
            burn_sum += burn;
            burn_max  = fmaxf(burn_max, burn);
        }
        if (burn_sum <= 0.05f) continue;

        Vec2 nozzle_local = vec2_scale(pos_sum, 1.0f / burn_sum);   // burn-weighted mean, art texels
        f32 dl = vec2_length(dir_sum);
        Vec2 exhaust_dir = (dl > 1.0e-3f) ? vec2_scale(dir_sum, 1.0f / dl) : Vec2{ 0.0f, -1.0f };

        Vec2 render_pos = render_from_hierpos(s, &ship->origin);
        Vec2 world_off = vec2_rotate(vec2_scale(nozzle_local, ship->world_scale), ship->angle);
        // Push the light down the mean exhaust direction so it sits in the plume, not
        // inside the hull — the stern bevels then face it and catch the spill.
        f32 hull_w = ship->visual.size_local.x;
        Vec2 plume_off = vec2_rotate(vec2_scale(exhaust_dir, hull_w * 0.45f), ship->angle);

        f32 heat = clampf(fl->heat_vis, 0.0f, 1.0f);
        bs_light2d& L = out[count++];
        L = {};
        L.position  = vec2_add(render_pos, vec2_add(world_off, plume_off));
        L.radius    = hull_w * (2.0f + 1.5f * burn_max);
        L.intensity = 0.55f * clampf(burn_sum, 0.0f, 1.5f);
        // Ember-red cold drive warming toward white-hot, matching the bell-glow ramp.
        L.color     = bs_color{ 1.0f, 0.55f + 0.40f * heat, 0.25f + 0.70f * heat, 1.0f };
        L.enabled   = TRUE;
    }

    // ---- Weapon flashes: every live FX-ring event is a brief light ----------------------
    // Muzzle, impact, flak airburst and PD intercept all read as incandescent events; the
    // ring already carries position (hierpos), size, damage-derived power and the shot's
    // tint. Radius rides the same 12x-45x sizing band the drawn effects use, and the
    // squared fade matches their brightness decay.
    for (i32 i = 0; i < MAX_PROJECTILE_FX && count < max_out; ++i) {
        const ProjectileFxEvent& e = s->projectile_fx.events[i];
        if (!e.active || e.life <= 0.0f) continue;
        f32 fade = 1.0f - clampf(e.age / e.life, 0.0f, 1.0f);
        if (fade <= 0.01f) continue;
        bs_light2d& L = out[count++];
        L = {};
        L.position  = render_from_hierpos(s, &e.position);
        L.radius    = e.scale * (30.0f + 18.0f * e.power);
        L.intensity = fade * fade * (0.5f + 0.35f * e.power);
        // Hot-shift the shot's tint toward white so the flash reads as incandescence.
        L.color     = bs_color{ e.tint.r + (1.0f - e.tint.r) * 0.5f,
                                e.tint.g + (1.0f - e.tint.g) * 0.5f,
                                e.tint.b + (1.0f - e.tint.b) * 0.5f, 1.0f };
        L.enabled   = TRUE;
    }

    return count;
}

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
        f32 cap   = ship_speed_cap(&s->player_ship());   // the selected gear, not motion.max_speed
        speed_ratio = clampf(speed / fmaxf(cap, 1.0f), 0.0f, 1.0f) * s->view_arena_w;
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

    // Dynamic ship lights (thruster burns, weapon flashes) fill the remaining slots.
    frame_light_count += collect_dynamic_ship_lights(s, frame_lights + frame_light_count,
                                                     16u - frame_light_count);

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
