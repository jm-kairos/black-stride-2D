#include "render/galaxy_map_render.h"

#include "game.h"
#include "core/view_transform.h"    // compression_factor, cosmetic_compress
#include "core/cursor_world.h"      // mouse_world
#include "sim/celestial_parallax.h" // celestial_center_render
#include "voronoi_galaxy.h"
#include "voronoi_cell_hover_effect.h"
#include "core/render_layers.h"
#include <core/input.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <renderer/bs_ui.h>
#include <renderer/bs_imgui.h>
#include <math.h>
#include <stdio.h>

using namespace bs_math;

// ---- Galaxy-map look tuning constants (moved from game.cpp; external definitions matching the
// extern decls in game.h — also referenced by mapped_system_layer.cpp) ----------------------
const f32 STAR_MIN_SCREEN_RADIUS   = 3.0f;   // px: minimum screen-space radius when zoomed out
const f32 STAR_DIST_SCALE_FACTOR   = 0.0003f;
const f32 STAR_MAX_DIST_SCALE      = 4.0f;
const f32 STAR_HERO_MAP_MIN_RADIUS = 42.0f;

// Draw a rotated rectangle outline by computing 4 corner points and connecting them.
static void draw_rotated_rect_outline(Vec2 center, Vec2 half_size, f32 angle,
                                      f32 thickness, bs_color color, u32 layer)
{
    Vec2 corners[4] = {
        Vec2{ -half_size.x, -half_size.y },
        Vec2{  half_size.x, -half_size.y },
        Vec2{  half_size.x,  half_size.y },
        Vec2{ -half_size.x,  half_size.y }
    };
    for (i32 i = 0; i < 4; ++i) {
        corners[i] = vec2_rotate(corners[i], angle);
        corners[i] = vec2_add(corners[i], center);
    }
    for (i32 i = 0; i < 4; ++i) {
        i32 j = (i + 1) % 4;
        renderer_draw_line(corners[i], corners[j], thickness, color, layer);
    }
}

// get_sensor_visibility is public (declared in game.h, defined in game.cpp).

void draw_galaxy_map_look(game_state* s, f32 dt) {
    // ---- Galaxy-map look render (was MODE_SYSTEM) -- cross-fades in by map weight ----------
    if (s->view_arena_w < 1.0f) {
        f32 map_w = 1.0f - s->view_arena_w;
        renderer_set_draw_alpha(map_w);

        // Update hovered cell (skip when cursor is over UI panels).
        if (!bs_imgui_wants_mouse()) {
            Vec2 mw = mouse_world(s);
            update_cell_hover_effect(&s->galaxy.galaxy_voronoi, dt, mw, &s->camera_state.camera_hierpos, s->camera_state.camera.zoom, s->galaxy.systems);
        }

        // Draw Delaunay dual lanes (natural connectivity from Voronoi diagram).
        if (s->galaxy.map_draw_lanes) {
            bs_color lane_col = bs_color{ 0.25f, 0.40f, 0.55f, 0.35f };
            draw_delaunay_lanes(&s->galaxy.galaxy_voronoi, s->galaxy.systems, s, lane_col, 1.0f);
        }

        // Draw Voronoi cell wireframe edges (territory boundaries).
        bs_color vedge_col = bs_color{ 0.45f, 0.55f, 0.70f, 0.12f };
        draw_voronoi_edges(&s->galaxy.galaxy_voronoi, s, vedge_col, 1.0f);

        // Overlay hovered cell with rotating neon-purple trail.
        bs_color hover_col = bs_color{ 0.55f, 0.20f, 1.00f, 1.00f };
        draw_cell_hover_effect(&s->galaxy.galaxy_voronoi, s, hover_col);

        // ---- Pass 1: Stars only (aux bloom eligible for streaks) ----
        renderer_set_aux_bloom_mode(s->render.star_fx.streak_enabled);

        if (s->galaxy.current_system >= 0 && s->galaxy.current_system < s->galaxy.system_count)
        {
            StarSystem& ss = s->galaxy.systems[s->galaxy.current_system];
            f32 length_mul = clampf(0.5f + ss.star.radius / 1500.0f, 0.5f, 2.0f);
            bs_color c = ss.star.color;
            f32 luminance = 0.3f * c.r + 0.6f * c.g + 0.1f * c.b;
            f32 intensity_mul = 0.4f + 0.6f * luminance;
            s->render.star_fx.streak_length_mul = length_mul;
            s->render.star_fx.streak_intensity_mul = intensity_mul;
        }
        else
        {
            s->render.star_fx.streak_length_mul = 1.0f;
            s->render.star_fx.streak_intensity_mul = 1.0f;
        }

        Vec2 hero_streak_screen = Vec2{ 0.0f, 0.0f };
        f32  hero_streak_scale  = 1.0f;
        b8   hero_streak_found  = FALSE;

        const i32 MAX_SUNBURST_STARS = 4; // keep in sync with BS_MAX_SUNBURST_STARS in the backend
        struct SunburstCandidate {
            f32  prominence;
            i32  sys;
            Vec2 star_pos;
            Vec2 star_screen;
            f32  scaled_base_r;
            f32  screen_radius;
            f32  vis;
            f32  total_scale;
        };
        SunburstCandidate sb_cand[MAX_SUNBURST_STARS];
        i32 sb_count = 0;
        for (i32 sys = 0; sys < s->galaxy.system_count; ++sys) {
            StarSystem& ss = s->galaxy.systems[sys];
            Vec2 sys_pos_raw = hierpos_diff(&ss.galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            f32 vis = get_sensor_visibility(s, sys_pos_raw);
            Vec2 sys_pos = vec2_scale(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star), compression_factor(s->camera_state.camera.zoom));
            Vec2 star_pos = vec2_add(sys_pos, ss.star.position);
            f32 base_r = ss.star.radius * (0.3f + 0.7f * vis);
            // 3D sphere mode: size the min-screen-radius floor by the actual sphere so it scales
            // with zoom instead of being pinned to a constant on-screen size.
            f32 body_scale = s->render.star_fx.star_3d_mode ? s->render.star_fx.star_body_scale : 1.0f;
            f32 screen_r = base_r * body_scale * s->camera_state.camera.zoom;
            f32 zoom_scale = (screen_r < STAR_MIN_SCREEN_RADIUS)
                ? (STAR_MIN_SCREEN_RADIUS / screen_r) : 1.0f;
            if (!s->render.star_fx.star_3d_mode && sys == s->galaxy.current_system && screen_r > 0.0f)
            {
                f32 hero_min = STAR_MIN_SCREEN_RADIUS
                    + (STAR_HERO_MAP_MIN_RADIUS - STAR_MIN_SCREEN_RADIUS) * map_w;
                if (screen_r < hero_min)
                    zoom_scale = fmaxf(zoom_scale, hero_min / screen_r);
            }
            Vec2 star_screen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, star_pos);
            Vec2 screen_center = Vec2{ (f32)s->fb_width * 0.5f, (f32)s->fb_height * 0.5f };
            f32 dist_from_center = vec2_length(vec2_sub(star_screen, screen_center));
            f32 dist_scale = 1.0f + dist_from_center * STAR_DIST_SCALE_FACTOR;
            dist_scale = fminf(dist_scale, STAR_MAX_DIST_SCALE);
            dist_scale = 1.0f + (dist_scale - 1.0f) * (1.0f - s->view_arena_w);
            f32 total_scale = zoom_scale * dist_scale;
            f32 scaled_base_r = base_r * total_scale;
            f32 screen_radius = scaled_base_r * s->camera_state.camera.zoom;
            if (sys == s->galaxy.current_system)
            {
                hero_streak_screen = star_screen;
                hero_streak_scale  = total_scale;
                hero_streak_found  = TRUE;
            }
            f32 cull_margin = screen_radius + 64.0f;
            b8 star_on_screen = star_screen.x > -cull_margin && star_screen.x < (f32)s->fb_width + cull_margin
                             && star_screen.y > -cull_margin && star_screen.y < (f32)s->fb_height + cull_margin;
            b8 is_hero = (sys == s->galaxy.current_system);
            if (!is_hero && (!star_on_screen || vis <= 0.0f))
                continue;
            // Prominence ranks nearer/brighter stars higher; the hero always wins a slot.
            f32 prominence = is_hero ? 3.4e38f : screen_radius * (vis + 0.01f);
            i32 slot = -1;
            if (sb_count < MAX_SUNBURST_STARS)
            {
                slot = sb_count++;
            }
            else
            {
                i32 weakest = 0;
                for (i32 c = 1; c < sb_count; ++c)
                    if (sb_cand[c].prominence < sb_cand[weakest].prominence) weakest = c;
                if (prominence > sb_cand[weakest].prominence) slot = weakest;
            }
            if (slot >= 0)
                sb_cand[slot] = SunburstCandidate{ prominence, sys, star_pos, star_screen,
                                                   scaled_base_r, screen_radius, vis, total_scale };
        }

        // Draw the selected stars. Each still owns the single-source streak while it is drawn; the
        // hero streak state is re-asserted immediately after this loop.
        for (i32 c = 0; c < sb_count; ++c)
        {
            const SunburstCandidate& cd = sb_cand[c];
            StarSystem& css = s->galaxy.systems[cd.sys];
            renderer_set_streak_source(cd.star_screen);
            s->render.star_fx.draw_star(css, cd.star_pos, cd.star_screen, cd.scaled_base_r, cd.screen_radius,
                                 cd.vis * map_w, s->galaxy.galaxy_map_time, LAYER_CELESTIAL,
                                 s->fb_width, s->fb_height, cd.total_scale);
        }

        // Re-assert the streak state for the current (hero) star so it owns the single-source
        // streak/flare, matching the arena renderer (where only the current star is drawn).
        if (hero_streak_found)
        {
            renderer_set_streak_source(hero_streak_screen);
            s->render.star_fx.apply_streak_state(hero_streak_scale, s->galaxy.galaxy_map_time);
        }

        renderer_set_aux_bloom_mode(FALSE);

        // ---- Pass 2: Labels, planets, orbit rings, and star light ----
        for (i32 sys = 0; sys < s->galaxy.system_count; ++sys) {
            StarSystem& ss = s->galaxy.systems[sys];
            Vec2 sys_pos_raw = hierpos_diff(&ss.galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            f32 vis = get_sensor_visibility(s, sys_pos_raw);
            // Depth-based parallax: per-element-type compressed system centers. depth_* scales the
            // camera-relative anchor offset (via celestial_center_render) so the backdrop drifts
            // slower than the camera; comp is the same cosmetic zoom-out compression applied
            // uniformly, so parallax ratios stay identical across Map and Arena modes.
            f32 comp = compression_factor(s->camera_state.camera.zoom);
            Vec2 center_star   = vec2_scale(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star),   comp);
            Vec2 center_orbit  = vec2_scale(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_orbit),  comp);
            Vec2 center_planet = vec2_scale(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_planet), comp);
            Vec2 star_pos = vec2_add(center_star, ss.star.position);

            // System name label above the star (only when clearly visible)
            if (ss.name && ss.name[0] && vis > 0.5f) {
                Vec2 screen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, star_pos);
                f32 zoom_factor = 0.006f / s->camera_state.camera.zoom;
                f32 font_scale = 1.0f + 0.25f * (zoom_factor - 1.0f);
                font_scale = clampf(font_scale, 0.8f, 1.6f);
                bs_color label_col = bs_color{ 0.90f, 0.92f, 0.96f, 0.85f * vis };
                bs_ui_label_at(ss.name, screen.x, screen.y - ss.star.radius * s->camera_state.camera.zoom - 4.0f,
                               font_scale, label_col, ss.name);
            }

            // Planets + orbit rings — draw true elliptical orbit paths.
            f32 inv_zoom = 1.0f / s->camera_state.camera.zoom;
            f32 max_orbit = 0.0f;
            for (i32 i = 0; i < ss.planet_count; ++i) {
                const CelestialBody& p = ss.planets[i];
                bs_color planet_col = p.color; planet_col.a *= vis;
                bs_color ring_col = p.color; ring_col.a = 0.25f * vis;
                f32 cw = cosf(p.arg_periapsis);
                f32 sw = sinf(p.arg_periapsis);
                f32 b = p.semi_major_axis * sqrtf(1.0f - p.eccentricity * p.eccentricity);
                const i32 SEGMENTS = 64;
                Vec2 prev = Vec2{0,0};
                b8 first = TRUE;
                for (i32 seg = 0; seg <= SEGMENTS; ++seg) {
                    f32 E = (f32)seg / (f32)SEGMENTS * 2.0f * BS_PI;
                    f32 x = p.semi_major_axis * (cosf(E) - p.eccentricity);
                    f32 y = b * sinf(E);
                    Vec2 rot = Vec2{ cw * x - sw * y, sw * x + cw * y };
                    Vec2 pt = vec2_add(center_orbit, vec2_scale(rot, comp));
                    if (!first) {
                        renderer_draw_line(prev, pt, 1.0f, ring_col, LAYER_CELESTIAL);
                    }
                    prev = pt;
                    first = FALSE;
                }
                Vec2 planet_off = vec2_scale(p.position, comp);
                Vec2 planet_vis = vec2_add(center_planet, planet_off);
                renderer_draw_circle(planet_vis, 2.0f * inv_zoom, 16, 1.0f, planet_col, LAYER_CELESTIAL);
                max_orbit = fmaxf(max_orbit, p.semi_major_axis);
            }

            // ---- Test sprites: colored dots orbiting the CURRENT star (volumetric light demo)
            if (sys == s->galaxy.current_system) {
                const i32 TEST_COUNT = 8;
                // Test sprites orbit the star; anchor them at the depth_testsprite center so they
                // stay co-located with the star (default depth_testsprite == depth_star).
                Vec2 ts_center = vec2_add(vec2_scale(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_testsprite), comp), ss.star.position);
                for (i32 ti = 0; ti < TEST_COUNT; ++ti) {
                    f32 t_angle = (f32)ti / (f32)TEST_COUNT * 2.0f * BS_PI + s->galaxy.galaxy_map_time * 0.3f;
                    f32 t_orbit = max_orbit * 0.3f + max_orbit * 0.7f * ((f32)ti / (f32)TEST_COUNT);
                    Vec2 tpos = Vec2{
                        ts_center.x + cosf(t_angle) * t_orbit * comp,
                        ts_center.y + sinf(t_angle) * t_orbit * comp
                    };
                    bs_color tcol = ss.star.color;
                    tcol.a = vis * 0.9f;
                    renderer_draw_circle(tpos, 3.0f * inv_zoom, 8, 2.0f, tcol, LAYER_CELESTIAL);
                }
            }

            // Star point light is now built once, unified, in the lighting
            // assembly below (Step 2C) so it cross-fades by map weight across the
            // blend band instead of switching on/off at the render-mode boundary.
        }

        // ---- Galaxy map entities ---------------------------------------------------------
        for (i32 i = 0; i < s->galaxy.map_entity_count; ++i) {
            Vec2 pos = hierpos_diff(&s->galaxy.map_entities[i].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            pos = cosmetic_compress(pos, s->camera_state.camera.zoom);
            // Player ship (index 0) is always fully visible; others fade with sensor range
            f32 vis = (i == 0) ? 1.0f : get_sensor_visibility(s, pos);
            bs_color ent_col = s->galaxy.map_entities[i].color;
            ent_col.a *= vis;
            renderer_draw_circle(pos, s->galaxy.map_entities[i].radius * (0.3f + 0.7f * vis), 8, 2.0f,
                                 ent_col, LAYER_UI);
        }

        // Animated quad around the player ship (index 0, has_outline == TRUE)
        if (s->galaxy.map_entity_count > 0 && s->galaxy.map_entities[0].has_outline) {
            Vec2 player_gal = hierpos_diff(&s->galaxy.map_entities[0].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            player_gal = cosmetic_compress(player_gal, s->camera_state.camera.zoom);
            // Animation parameters
            f32 t = s->galaxy.galaxy_map_time;
            f32 scale_mul  = s->galaxy.map_anim_scale     ? (1.0f + 0.30f * sinf(t * 2.0f)) : 1.0f;
            f32 angle      = s->galaxy.map_anim_rotate    ? (t * 1.5f) : 0.0f;
            f32 fill_alpha = s->galaxy.map_anim_alpha     ? (0.45f + 0.35f * sinf(t * 3.0f)) : 0.80f;
            f32 out_alpha  = s->galaxy.map_anim_alpha     ? (0.70f + 0.25f * sinf(t * 3.0f)) : 0.90f;
            f32 thick_mul  = s->galaxy.map_anim_thickness ? (1.0f + 0.50f * sinf(t * 4.0f)) : 1.0f;
            f32 base_size  = 120.0f;
            Vec2 size      = Vec2{ base_size * scale_mul, base_size * scale_mul };
            // Filled quad (sprite supports rotation natively)
            bs_sprite sq{};
            sq.position = player_gal;
            sq.size     = size;
            sq.origin   = Vec2{ 0.5f, 0.5f };
            sq.rotation = angle;
            sq.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            sq.tint     = bs_color{ 0.2f, 0.8f, 1.0f, fill_alpha };
            sq.texture  = bs_texture{ 0 }; // white texture
            sq.blend    = BLEND_ALPHA;
            sq.layer    = LAYER_UI;
            sq.glow_override = nullptr;
            renderer_draw_sprite(&sq);
            // Rotated outline
            f32 outline_thick = 3.0f * thick_mul;
            bs_color out_col  = bs_color{ 1.0f, 1.0f, 1.0f, out_alpha };
            draw_rotated_rect_outline(player_gal,
                                      Vec2{ size.x * 0.5f, size.y * 0.5f },
                                      angle, outline_thick, out_col, LAYER_UI);
        }

        // ---- Hyperjump range circle ---------------------------------------------------------
        if (s->galaxy.map_draw_jump_range) {
            Vec2 ship_rel = hierpos_diff(&s->galaxy.map_entities[0].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            ship_rel = cosmetic_compress(ship_rel, s->camera_state.camera.zoom);
            f32 r = s->galaxy.map_jump_range;
            bs_color range_col = bs_color{ 0.35f, 0.75f, 0.95f, 0.30f };
            u32 segments = 96;
            for (u32 i = 0; i < segments; i += 2) {
                f32 a0 = (f32)i       / segments * 2.0f * BS_PI;
                f32 a1 = (f32)(i + 1) / segments * 2.0f * BS_PI;
                Vec2 p0 = vec2_add(ship_rel, Vec2{ cosf(a0) * r, sinf(a0) * r });
                Vec2 p1 = vec2_add(ship_rel, Vec2{ cosf(a1) * r, sinf(a1) * r });
                renderer_draw_line(p0, p1, 1.5f, range_col, LAYER_UI);
            }
        }

        // ---- Sensor detection range rings ---------------------------------------------------
        if (s->galaxy.map_draw_sensor_range && s->galaxy.map_entity_count > 0) {
            Vec2 ship_rel = hierpos_diff(&s->galaxy.map_entities[0].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            ship_rel = cosmetic_compress(ship_rel, s->camera_state.camera.zoom);
            constexpr u32 SENSOR_RING_COUNT = 20;
            constexpr f32 SENSOR_BASE_ALPHA = 0.45f;
            bs_color base_col = bs_color{ 0.45f, 0.90f, 0.40f, 0.0f };
            for (u32 ring = 1; ring <= SENSOR_RING_COUNT; ++ring) {
                f32 t = (f32)ring / (f32)SENSOR_RING_COUNT;
                f32 r = s->galaxy.map_sensor_range * t;
                f32 alpha = SENSOR_BASE_ALPHA * (1.0f - t * t * t);
                if (alpha <= 0.0f) continue;
                bs_color ring_col = base_col;
                ring_col.a = alpha;
                renderer_draw_circle(ship_rel, r, 64, 1.0f, ring_col, LAYER_UI);
            }
        }

        // ---- Map entity hover tooltip -------------------------------------------------------
        i32 mx = 0, my = 0;
        input_get_mouse_position(&mx, &my);
        const MapEntity* hovered = nullptr;
        for (i32 i = 0; i < s->galaxy.map_entity_count; ++i) {
            Vec2 rel = hierpos_diff(&s->galaxy.map_entities[i].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
            rel = cosmetic_compress(rel, s->camera_state.camera.zoom);
            Vec2 screen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, rel);
            f32 dx = (f32)mx - screen.x;
            f32 dy = (f32)my - screen.y;
            f32 hit_r = s->galaxy.map_entities[i].radius * s->camera_state.camera.zoom + 8.0f;
            if (dx * dx + dy * dy <= hit_r * hit_r) {
                hovered = &s->galaxy.map_entities[i];
                break; // first match wins
            }
        }
        if (hovered) {
            f64 ax, ay, bx, by;
            hierpos_to_f64(&hovered->galaxy_pos, BS_HIERPOS_CELL_SIZE, &ax, &ay);
            hierpos_to_f64(&s->galaxy.map_entities[0].galaxy_pos, BS_HIERPOS_CELL_SIZE, &bx, &by);
            f64 dist = sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
            char buf[128];
            if (dist >= 1000000.0) {
                snprintf(buf, sizeof(buf), "%s\nDist: %.2f M u", hovered->name ? hovered->name : "?", dist / 1000000.0);
            } else if (dist >= 1000.0) {
                snprintf(buf, sizeof(buf), "%s\nDist: %.2f k u", hovered->name ? hovered->name : "?", dist / 1000.0);
            } else {
                snprintf(buf, sizeof(buf), "%s\nDist: %.0f u", hovered->name ? hovered->name : "?", dist);
            }
            bs_ui_tooltip_at((f32)mx, (f32)my, buf);
        }

        // Restore full opacity for the arena/gameplay passes that follow.
        renderer_set_draw_alpha(1.0f);
    }
}
