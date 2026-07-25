#include "sim/heat_map.h"
#include "game.h"
#include "core/view_transform.h" // game_camera_center_hierpos
#include <renderer/renderer.h> // bs_heat_map_params, renderer_draw_heat_map
#include <math.h>  // powf, fmaxf
#include <chrono>

using namespace bs_math;

// Radiation detector holds full intensity down to this zoom, then fades to 0 at
// HEAT_FADE_ZERO_ZOOM. Set well below the arena zoom so the detector only fades when
// zoomed MUCH further out. HEAT_FADE_ZERO_ZOOM matches ZOOM_GLOBAL_MIN (the galaxy floor).
static const f32 HEAT_FADE_FULL_ZOOM = 0.005f;
static const f32 HEAT_FADE_ZERO_ZOOM = 0.000004f; // galaxy floor (== ZOOM_GLOBAL_MIN)

// Detector fade weight: holds full intensity down to HEAT_FADE_FULL_ZOOM, then smoothsteps
// to 0 at HEAT_FADE_ZERO_ZOOM.
static f32 heat_map_fade_weight(f32 zoom) {
    if (zoom >= HEAT_FADE_FULL_ZOOM) return 1.0f;
    if (zoom <= HEAT_FADE_ZERO_ZOOM) return 0.0f;
    f32 t = (zoom - HEAT_FADE_ZERO_ZOOM) / (HEAT_FADE_FULL_ZOOM - HEAT_FADE_ZERO_ZOOM);
    return t * t * (3.0f - 2.0f * t); // smoothstep
}

// Colored heat map is drawn via a single-pass GPU shader; a simple circle around the
// flagship shows the detector range. CPU work is O(sources) and scales with ship count.
void draw_ship_metaballs(game_state* s) {
    // The radiation heat map keeps its explicit UI toggle, but its visibility is driven by the
    // detector fade weight so it holds full intensity across the normal zoom range and only fades
    // (rather than popping) once zoomed MUCH further out, all the way down toward the galaxy floor.
    if (heat_map_fade_weight(s->camera_state.camera.zoom) <= 0.0f || !s->show_metaball_ui) {
        s->heat_map_cpu_ms = 0.0f;
        return;
    }
    s->perf_heat_map_start_ns = (f64)std::chrono::steady_clock::now().time_since_epoch().count();
    f32 threshold = s->metaball_threshold;
    if (threshold < 1.0e-4f) threshold = 1.0e-4f;

    f32 base_radius = s->base_detection_radius;
    f32 heat_signature_radius = s->heat_signature_radius;
    if (base_radius <= 0.0f) return;

    // Viewport for shader submission and source culling.
    f32 vis_w = (f32)s->fb_width  / s->camera_state.camera.zoom;
    f32 vis_h = (f32)s->fb_height / s->camera_state.camera.zoom;
    const f32 MARGIN = 0.15f;
    f32 pad_x = vis_w * MARGIN;
    f32 pad_y = vis_h * MARGIN;
    // STEP 1 (heat map deferred to step 2): the heat-map shader maps world sources via its own
    // camera_pos uniform, independent of the render-space sprite camera. Feed it the effective
    // true-world center so sources (kept in true world) still resolve correctly under the unified
    // path, where s->camera_state.camera.position is a small render-space residual rather than the true center.
    // Work in a camera-relative true-world frame (positions minus camera_hierpos) so the heat
    // map stays precision-safe far from the galaxy origin. Radii stay in true-world units and
    // the shader only cares about relative offsets, so translating everything by -camera_hierpos
    // is exact and leaves the on-screen result identical.
    bs_math::HierPos2 cam_center_hp = game_camera_center_hierpos(s);
    Vec2 heat_center = hierpos_diff(&cam_center_hp, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    f32 min_x = heat_center.x - vis_w * 0.5f - pad_x;
    f32 max_x = heat_center.x + vis_w * 0.5f + pad_x;
    f32 min_y = heat_center.y - vis_h * 0.5f - pad_y;
    f32 max_y = heat_center.y + vis_h * 0.5f + pad_y;
    const f32 cull_margin = fmaxf(base_radius, heat_signature_radius) * 3.0f;
    auto in_viewport = [&](Vec2 p) -> b8 {
        return p.x >= min_x - cull_margin && p.x <= max_x + cull_margin &&
               p.y >= min_y - cull_margin && p.y <= max_y + cull_margin;
    };
    auto to_rel = [&](const bs_math::HierPos2& hp) -> Vec2 {
        return hierpos_diff(&hp, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    };

    // ---- Gather all game objects with radiation_emission > 0. -----------------------
    // Sources are combat entities (ships + non-ship targets) and projectiles so the system
    // is generic: any future object with a radiation_emission property will show up on the
    // heat map. Player fleet ships are also submitted as detector sources; they define the
    // sensor circles that reveal enemy/projectile heat signatures.
    struct MBSource { Vec2 origin; f32 r2; f32 emission; b8 is_detector; };
    MBSource srcs[BS_MAX_HEAT_SOURCES];
    i32 src_count = 0;

    // ---- Submit player fleet ships as detector sources -------------------------------
    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
        if (src_count >= BS_MAX_HEAT_SOURCES) break;
        srcs[src_count++] = MBSource{ to_rel(s->fleet_state.fleet.at(i).ship.origin), base_radius * base_radius, 0.0f, TRUE };
    }

    // ---- Submit heat emitters (enemy ships, non-ship targets, projectiles) ---------
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active || ce->radiation_emission <= 0.0f) continue;
        Vec2 ce_rel = to_rel(ce->position);
        if (!in_viewport(ce_rel)) continue;
        if (src_count >= BS_MAX_HEAT_SOURCES) break;
        f32 emission = ce->radiation_emission;
        // Emitter blob radius is independent of the detector range.
        srcs[src_count++] = MBSource{ ce_rel, heat_signature_radius * heat_signature_radius, emission, FALSE };

        // Trail sources: velocity-extrapolated positions with speed-scaled, age-faded emission.
        const f32 TAIL_TIME_STEP = 0.05f; // seconds of spacing between each trail point
        f32 speed = vec2_length(ce->velocity);
        f32 speed_ratio = clampf(speed / SHIP_MAX_SPEED, 0.0f, 1.0f);
        if (speed > 0.001f) {
            Vec2 vel_dir = vec2_scale(ce->velocity, 1.0f / speed);
            i32 trail_len = (i32)clampf(s->heat_tail_length, 0.0f, 8.0f);
            for (i32 h = 1; h <= trail_len; ++h) {
                f32 t = (f32)h / (f32)(trail_len + 1); // 0 = newest, 1 = oldest
                f32 dist_back = speed * TAIL_TIME_STEP * (f32)h;
                Vec2 trail_pos = vec2_sub(ce_rel, vec2_scale(vel_dir, dist_back));
                f32 trail_emission = emission * speed_ratio * powf(1.0f - t, s->heat_tail_fade);
                if (trail_emission < 1.0e-4f) continue;
                srcs[src_count++] = MBSource{ trail_pos, heat_signature_radius * heat_signature_radius, trail_emission, FALSE };
                if (src_count >= BS_MAX_HEAT_SOURCES) break;
            }
            if (src_count >= BS_MAX_HEAT_SOURCES) break;
        }
    }

    // ---- Gather active projectiles as heat sources ------------------------------------
    // Projectiles radiate along their entire trajectory, are visible to all factions, and
    // are revealed only where they intersect the player fleet's detector circles.
    const f32 PROJ_TAIL_TIME_STEP = 0.02f; // seconds of spacing between each trail point
    const i32 PROJ_MAX_TRAIL_POINTS = 16; // longer tail than ships because projectiles move fast
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        const Projectile& p = s->projectiles.pool[i];
        if (!p.active || p.radiation_emission <= 0.0f) continue;
        Vec2 p_rel = to_rel(p.position);
        if (!in_viewport(p_rel)) continue;
        if (src_count >= BS_MAX_HEAT_SOURCES) break;
        srcs[src_count++] = MBSource{ p_rel, heat_signature_radius * heat_signature_radius, p.radiation_emission, FALSE };

        f32 speed = vec2_length(p.velocity);
        if (speed > 0.001f) {
            Vec2 vel_dir = vec2_scale(p.velocity, 1.0f / speed);
            for (i32 h = 1; h <= PROJ_MAX_TRAIL_POINTS; ++h) {
                if (src_count >= BS_MAX_HEAT_SOURCES) break;
                f32 t = (f32)h / (f32)(PROJ_MAX_TRAIL_POINTS + 1); // 0 = newest, 1 = oldest
                f32 dist_back = speed * PROJ_TAIL_TIME_STEP * (f32)h;
                Vec2 trail_pos = vec2_sub(p_rel, vec2_scale(vel_dir, dist_back));
                f32 trail_emission = p.radiation_emission * powf(1.0f - t, s->heat_tail_fade);
                if (trail_emission < 1.0e-4f) continue;
                srcs[src_count++] = MBSource{ trail_pos, heat_signature_radius * heat_signature_radius, trail_emission, FALSE };
            }
        }
    }

    if (src_count == 0) return;

    // Draw the colored heat map via a single-pass GPU shader.
    bs_heat_map_params hm{};
    hm.camera_pos = heat_center;
    hm.viewport_w = vis_w;
    hm.viewport_h = vis_h;
    hm.base_radius = base_radius;
    hm.heat_signature_radius = heat_signature_radius;
    hm.palette = (u32)s->heat_palette;
    hm.color_low = s->heat_color_low;
    hm.color_high = s->heat_color_high;
    hm.color_falloff_power = s->heat_color_falloff_power;
    hm.heat_warp_strength = s->heat_warp_strength;
    hm.venn_sharpness = s->heat_map_venn_sharpness;
    hm.threshold = threshold;
    hm.intensity = s->heat_map_intensity * heat_map_fade_weight(s->camera_state.camera.zoom);
    hm.source_count = (u32)src_count;
    hm.heat_tail_length = s->heat_tail_length;
    hm.heat_tail_fade = s->heat_tail_fade;
    for (i32 i = 0; i < src_count; ++i) {
        hm.sources[i] = srcs[i].origin;
        hm.emissions[i] = srcs[i].emission;
        hm.is_detector[i] = srcs[i].is_detector;
    }
    renderer_draw_heat_map(&hm);

    f64 heat_end_ns = (f64)std::chrono::steady_clock::now().time_since_epoch().count();
    f32 heat_dt_ms = (f32)(heat_end_ns - s->perf_heat_map_start_ns) / 1e6f;
    s->heat_map_cpu_ms = s->heat_map_cpu_ms * 0.9f + heat_dt_ms * 0.1f;
}
