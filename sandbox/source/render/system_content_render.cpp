#include "render/system_content_render.h"
#include "game.h"
#include "core/view_transform.h"   // render_from_hierpos
#include "core/render_layers.h"    // LAYER_CELESTIAL

#include <math/bs_hierpos.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>

#include <math.h>

using namespace bs_math;

// Sensor visibility for an absolute HierPos2 position (same falloff the celestial layers
// use; see mapped_system_layer.cpp / sensor_visibility_from_dist).
static f32 system_sensor_vis(const game_state* s, const HierPos2* pos) {
    if (!s->galaxy.map_draw_sensor_range) return 1.0f;
    f32 dist = vec2_length(hierpos_diff(pos, &s->player_ship().origin, BS_HIERPOS_CELL_SIZE));
    return sensor_visibility_from_dist(dist, s->galaxy.map_sensor_range);
}

static b8 on_screen(const Camera2D* cam, u16 fb_w, u16 fb_h, Vec2 world_pos, f32 radius) {
    Vec2 screen  = camera2d_world_to_screen(cam, fb_w, fb_h, world_pos);
    f32 screen_r = radius * cam->zoom;
    return (screen.x + screen_r > 0.0f && screen.x - screen_r < (f32)fb_w &&
            screen.y + screen_r > 0.0f && screen.y - screen_r < (f32)fb_h);
}

// Closed polygon silhouette: verts spaced evenly around the circle, radius jittered per vertex
// (deterministic, generated with the object), rotated by the tumble angle.
static void draw_asteroid_poly(Vec2 center, f32 rotation, i32 verts, const f32* vert_jitter,
                               bs_color col, f32 alpha, f32 draw_r) {
    col.a *= alpha;
    Vec2 prev{};
    for (i32 v = 0; v <= verts; ++v) {
        i32 vi = v % verts;
        f32 a  = rotation + (f32)vi / (f32)verts * 2.0f * BS_PI;
        f32 r  = draw_r * vert_jitter[vi];
        Vec2 p = Vec2{ center.x + cosf(a) * r, center.y + sinf(a) * r };
        if (v == 0) { prev = p; continue; }
        renderer_draw_line(prev, p, 1.5f, col, LAYER_CELESTIAL);
        prev = p;
    }
}

// On-screen radius (px) below which an entity collapses to a single quad instead of a
// multi-segment ring / polygon. Rings & polygons are the dominant sprite consumers, and at wide
// zoom hundreds of these entities are only a few px across -- emitting a full 10-24 segment ring
// for each would blow the renderer's single 16384-sprite batch. Collapsing them to one quad keeps
// the per-frame sprite count in budget without a visible difference at that size.
static constexpr f32 LOD_DOT_PX = 3.0f;

// Per-system detail (asteroids / resources / dust) numbers in the thousands per system, so drawing
// every materialized system's belt at galaxy zoom would blow the 16384-sprite batch. A belt is only
// worth drawing once it spans a meaningful size on screen; below this the whole system is skipped.
// This bounds the per-frame sprite count to the handful of systems whose belts are actually large
// on screen (system-view zoom), where the off-screen cull + LOD keep each within budget.
static constexpr f32 SYSTEM_DETAIL_MIN_SCREEN_PX = 48.0f;

// Outer-belt on-screen radius (px) for a system: outermost orbit * fringe factor, in render px.
// Callers skip a system's detail entirely when this is below SYSTEM_DETAIL_MIN_SCREEN_PX.
static f32 system_belt_screen_px(const StarSystem& ss, f32 zoom) {
    f32 outer = 0.0f;
    i32 pc = ss.planet_count < 5 ? ss.planet_count : 5;
    for (i32 i = 0; i < pc; ++i)
        if (ss.planets[i].semi_major_axis > outer) outer = ss.planets[i].semi_major_axis;
    return outer * 1.4f * zoom; // 1.4 = fringe zone extends just beyond the outermost orbit
}

// Adaptive circle tessellation: a ring only needs enough segments to look round at its on-screen
// size. Scales the segment count with screen radius, clamped to [6, base].
static u32 adaptive_segments(f32 screen_r, u32 base) {
    f32 seg = screen_r * 0.6f;
    if (seg < 6.0f)      seg = 6.0f;
    if (seg > (f32)base) seg = (f32)base;
    return (u32)seg;
}

// Undiscovered station: a small screen-constant antenna marker (mast + dish) with two side lobes
// whose alpha pulses, so unknown installations read as "signal source" until scanned. `rgb` sets the
// marker hue (its alpha is ignored; `alpha` drives fade) -- green for trade-active stations.
static void draw_station_unknown_marker(const game_state* s, Vec2 center, f32 alpha, f32 phase, bs_color rgb) {
    f32 zoom_inv = 1.0f / s->camera_state.camera.zoom;
    f32 r = 14.0f * zoom_inv;                         // marker base size (screen-constant)
    bs_color col = bs_color{ rgb.r, rgb.g, rgb.b, alpha };
    // Mast (vertical) + small dish circle at the top.
    renderer_draw_line(Vec2{ center.x, center.y - r * 0.4f }, Vec2{ center.x, center.y + r },
                       1.5f, col, LAYER_CELESTIAL);
    renderer_draw_circle(Vec2{ center.x, center.y + r }, r * 0.35f, 12, 1.5f, col, LAYER_CELESTIAL);
    // Pulsing side lobes (transmission).
    f32 pulse = 0.5f + 0.5f * sinf(s->elapsed_time * 4.0f + phase);
    bs_color lobe = bs_color{ col.r, col.g, col.b, alpha * pulse };
    renderer_draw_circle(Vec2{ center.x - r * 0.7f, center.y }, r * (0.4f + 0.3f * pulse), 12, 1.5f, lobe, LAYER_CELESTIAL);
    renderer_draw_circle(Vec2{ center.x + r * 0.7f, center.y }, r * (0.4f + 0.3f * pulse), 12, 1.5f, lobe, LAYER_CELESTIAL);
}

// Default undiscovered-marker hue (cyan) and the hue for trade-active stations (green).
static const bs_color STATION_MARKER_COL = bs_color{ 0.55f, 0.85f, 0.95f, 1.0f };
static const bs_color STATION_TRADE_COL  = bs_color{ 0.25f, 0.90f, 0.35f, 1.0f };

// TRUE if the station `station_id` is currently the origin (issuer) or destination (recipient) of
// any ACTIVE trade contract. Such trade-active stations render green (antenna + discovered sprite)
// so the player can spot which installations are delegating / receiving trade.
static b8 station_is_trade_active(const game_state* s, i32 station_id) {
    if (station_id < 0 || !s->galaxy.missions) return FALSE;
    for (i32 mi = 0; mi < s->galaxy.mission_count; ++mi) {
        const ShipMission& m = s->galaxy.missions[mi];
        if (!m.active) continue;
        if (m.station_id == station_id || m.dest_station_id == station_id) return TRUE;
    }
    return FALSE;
}

// Per-system civilian stations (habited systems + uninhabited station-markets). Discovered stations
// render as a filled disc (thick ring) with an internal quad in the owner civ's colour; undiscovered
// ones show the pulsing antenna marker. Stations actively delegating or receiving a trade contract
// render GREEN (antenna and discovered sprite). Faded by sensor visibility. Called from the scene renderer.
void draw_system_stations(game_state* s) {
    const Camera2D* cam = &s->camera_state.camera;
    f32 zoom = cam->zoom;
    f32 min_r = 2.5f / (zoom > 1e-6f ? zoom : 1e-6f);
    for (i32 si = 0; si < s->galaxy.system_count; ++si) {
        StarSystem& ss = s->galaxy.systems[si];
        for (i32 k = 0; k < ss.station_count; ++k) {
            SystemStation& st = ss.stations[k];
            Vec2 center = render_from_hierpos(s, &st.pos);
            f32 draw_r  = st.radius;
            if (draw_r < min_r) draw_r = min_r;
            f32 extent  = draw_r * 2.0f + 32.0f / (zoom > 1e-6f ? zoom : 1e-6f);
            if (!on_screen(cam, s->fb_width, s->fb_height, center, extent)) continue;
            f32 vis = system_sensor_vis(s, &st.pos);
            if (vis <= 0.003f) continue;

            b8 trade = station_is_trade_active(s, st.station_id);

            if (!st.discovered) {
                draw_station_unknown_marker(s, center, vis, st.pulse_phase,
                                            trade ? STATION_TRADE_COL : STATION_MARKER_COL);
                continue;
            }
            // Trade-active -> green; else owner civ colour (fallback to a neutral station blue).
            bs_color col = bs_color{ 0.55f, 0.75f, 0.95f, 1.0f };
            if (trade)
                col = STATION_TRADE_COL;
            else if (st.owner_civ >= 0 && st.owner_civ < s->galaxy.civ_count)
                col = s->galaxy.civs[st.owner_civ].color;
            col.a = vis;
            f32 screen_r = draw_r * zoom;
            // Outer circle outline + internal filled quad => "circular sprite with an internal quad".
            renderer_draw_circle(center, draw_r, adaptive_segments(screen_r, 32), 2.0f, col, LAYER_CELESTIAL);
            renderer_draw_quad(center, Vec2{ draw_r * 0.7f, draw_r * 0.7f }, col, LAYER_CELESTIAL);
        }
    }
}

// Per-system natural asteroids (every system). Asteroids REVEAL only within the flagship's Layer 1
// sensor radius (sensors.layer1_radius) -- fly a ship into a belt to see it. The whole system is
// skipped when its belt is a speck on screen (galaxy zoom), and each asteroid is off-screen culled
// + collapsed to a single quad at wide zoom. Called from the scene renderer.
void draw_system_asteroids(game_state* s) {
    const Camera2D* cam = &s->camera_state.camera;
    const Ship& flag = s->player_ship();
    f32 sensor_r = flag.sensors.layer1_radius; // asteroids reveal only within the ship's Layer 1 radius
    f32 zoom = cam->zoom;
    f32 min_r = 2.5f / (zoom > 1e-6f ? zoom : 1e-6f);
    for (i32 si = 0; si < s->galaxy.system_count; ++si) {
        StarSystem& ss = s->galaxy.systems[si];
        if (system_belt_screen_px(ss, zoom) < SYSTEM_DETAIL_MIN_SCREEN_PX) continue;
        for (i32 k = 0; k < ss.asteroid_count; ++k) {
            SystemAsteroid& a = ss.asteroids[k];
            Vec2 center = render_from_hierpos(s, &a.pos);
            f32 draw_r  = a.radius;
            if (draw_r < min_r) draw_r = min_r;
            if (!on_screen(cam, s->fb_width, s->fb_height, center, draw_r * 2.0f)) continue;
            f32 dist = vec2_length(hierpos_diff(&a.pos, &flag.origin, BS_HIERPOS_CELL_SIZE));
            f32 vis = sensor_visibility_from_dist(dist, sensor_r);
            if (vis <= 0.003f) continue;
            f32 screen_r = draw_r * zoom;
            bs_color col = a.color;
            // Far / tiny LOD: collapse to ONE quad (the belt reads as a dotted ring at system zoom).
            if (screen_r < LOD_DOT_PX) {
                col.a *= vis;
                renderer_draw_quad(center, Vec2{ draw_r * 1.6f, draw_r * 1.6f }, col, LAYER_CELESTIAL);
                continue;
            }
            f32 rot = a.rotation + a.spin * s->elapsed_time;
            draw_asteroid_poly(center, rot, a.verts, a.vert_jitter, col, vis, draw_r);
        }
    }
}

// Per-system resource nodes (every system; concentrated in belt/mid zones). Small rings that REVEAL
// only within the flagship's Layer 1 sensor radius (matching the asteroids), skipped whole-system at
// galaxy zoom, off-screen culled, and collapsed to a single quad at wide zoom. Called from the
// scene renderer alongside the per-system asteroids.
void draw_system_resources(game_state* s) {
    const Camera2D* cam = &s->camera_state.camera;
    const Ship& flag = s->player_ship();
    f32 sensor_r = flag.sensors.layer1_radius; // resources reveal only within the ship's Layer 1 radius
    f32 zoom = cam->zoom;
    f32 min_r = 2.5f / (zoom > 1e-6f ? zoom : 1e-6f);
    for (i32 si = 0; si < s->galaxy.system_count; ++si) {
        StarSystem& ss = s->galaxy.systems[si];
        if (system_belt_screen_px(ss, zoom) < SYSTEM_DETAIL_MIN_SCREEN_PX) continue;
        for (i32 k = 0; k < ss.resource_count; ++k) {
            SystemResource& r = ss.resources[k];
            Vec2 center = render_from_hierpos(s, &r.pos);
            f32 draw_r  = r.radius;
            if (draw_r < min_r) draw_r = min_r;
            if (!on_screen(cam, s->fb_width, s->fb_height, center, draw_r * 2.0f)) continue;
            f32 dist = vec2_length(hierpos_diff(&r.pos, &flag.origin, BS_HIERPOS_CELL_SIZE));
            f32 vis = sensor_visibility_from_dist(dist, sensor_r);
            if (vis <= 0.003f) continue;
            f32 screen_r = draw_r * zoom;
            bs_color col = r.color;
            col.a *= vis;
            // Far / tiny LOD: collapse to ONE quad (matches the asteroid budget control).
            if (screen_r < LOD_DOT_PX) {
                renderer_draw_quad(center, Vec2{ draw_r * 1.6f, draw_r * 1.6f }, col, LAYER_CELESTIAL);
                continue;
            }
            renderer_draw_circle(center, draw_r, adaptive_segments(screen_r, 10), 2.0f, col, LAYER_CELESTIAL);
        }
    }
}

// Per-system ambient dust decorations (every system; scattered across all zones). Tiny quads that
// REVEAL only within the flagship's Layer 1 sensor radius (matching the asteroids), skipped
// whole-system at galaxy zoom and off-screen culled. Purely cosmetic. Called from the scene
// renderer alongside the per-system asteroids.
void draw_system_decorations(game_state* s) {
    const Camera2D* cam = &s->camera_state.camera;
    const Ship& flag = s->player_ship();
    f32 sensor_r = flag.sensors.layer1_radius; // dust reveals only within the ship's Layer 1 radius
    f32 zoom = cam->zoom;
    f32 min_r = 2.0f / (zoom > 1e-6f ? zoom : 1e-6f);
    for (i32 si = 0; si < s->galaxy.system_count; ++si) {
        StarSystem& ss = s->galaxy.systems[si];
        if (system_belt_screen_px(ss, zoom) < SYSTEM_DETAIL_MIN_SCREEN_PX) continue;
        for (i32 k = 0; k < ss.decoration_count; ++k) {
            SystemDecoration& d = ss.decorations[k];
            Vec2 center = render_from_hierpos(s, &d.pos);
            f32 draw_r  = d.radius;
            if (draw_r < min_r) draw_r = min_r;
            if (!on_screen(cam, s->fb_width, s->fb_height, center, draw_r * 2.0f)) continue;
            f32 dist = vec2_length(hierpos_diff(&d.pos, &flag.origin, BS_HIERPOS_CELL_SIZE));
            f32 vis = sensor_visibility_from_dist(dist, sensor_r);
            if (vis <= 0.003f) continue;
            bs_color col = d.color;
            col.a *= vis;
            renderer_draw_quad(center, Vec2{ draw_r * 1.6f, draw_r * 1.6f }, col, LAYER_CELESTIAL);
        }
    }
}

