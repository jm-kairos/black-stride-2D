#include "render/galaxy_map_render.h"



#include "game.h"

#include "core/view_transform.h"    // game_true_world_to_render, view transforms

#include "core/cursor_world.h"      // mouse_world

#include "sim/celestial_parallax.h" // celestial_center_render

#include "sim/galaxy_map.h"         // GALAXY_MATERIALIZE_RADIUS, galaxy_nearest_node

#include "sim/ship_mission.h"        // ship_mission_position (cross-system traveler pips)

#include "sim/galaxy_gen.h"         // galaxy_env_at (structural population environment)

#include "sim/ss_generation.h"          // generate_star_system, spectral_class_name, planet_type_name

#include "sim/galaxy_history.h"     // galaxy_history_civ_at_node, civ_* labels (Phase 1)

#include "sim/voronoi_galaxy.h"

#include "render/voronoi_cell_hover_effect.h"

#include "core/render_layers.h"

#include <core/input.h>

#include <core/memory/bs_memory.h>

#include <renderer/renderer.h>

#include <renderer/camera2d.h>

#include <renderer/bs_ui.h>

#include <renderer/bs_imgui.h>

#include <renderer/bs_rml.h>   // bs_rml_wants_mouse

#include <math.h>

#include <stdio.h>



using namespace bs_math;



// ---- Galaxy-map look tuning constants (moved from game.cpp; external definitions matching the

// extern decls in game.h — also referenced by mapped_system_layer.cpp) ----------------------

const f32 STAR_MIN_SCREEN_RADIUS   = 3.0f;   // px: minimum screen-space radius when zoomed out

const f32 STAR_DIST_SCALE_FACTOR   = 0.0003f;

const f32 STAR_MAX_DIST_SCALE      = 4.0f;

const f32 STAR_HERO_MAP_MIN_RADIUS = 42.0f;



// Hit-test the map for a planet under `screen_x/y`. Mirrors the Pass-2 planet draw math exactly

// (per-element parallax anchor, per-type render size, and the >=6px orbit visibility

// gate) so a hit corresponds to what the player actually sees. Returns the nearest planet's galaxy

// gate) so a hit corresponds to what the player actually sees. Returns the nearest planet's cache

// slot + planet index. Only considers currently-cached systems.

b8 galaxy_pick_planet(const game_state* s, f32 screen_x, f32 screen_y, i32* out_slot, i32* out_planet_index)

{

    const f32 zoom = s->camera_state.camera.zoom;

    f32 best_d2 = 3.4e38f;

    i32 best_sys = -1, best_pi = -1;

    for (i32 sys = 0; sys < s->galaxy.system_count; ++sys) {

        const StarSystem& ss = s->galaxy.systems[sys];

        Vec2 center_planet = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_planet);

        for (i32 i = 0; i < ss.planet_count; ++i) {

            const CelestialBody& p = ss.planets[i];

            if (p.semi_major_axis * zoom < 6.0f) continue; // same LOD gate as Pass 2

            const PlanetTypeParams& pe = s->render.star_fx.planet_params[(i32)ss.planet_props[i].type];

            Vec2 planet_vis = vec2_add(center_planet, p.position);

            Vec2 pscreen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, planet_vis);

            f32 body_px = fmaxf(p.radius * pe.size_mul * zoom, pe.min_px);

            f32 hit_r = body_px + 8.0f; // small click margin

            f32 dx = screen_x - pscreen.x, dy = screen_y - pscreen.y;

            f32 d2 = dx * dx + dy * dy;

            if (d2 <= hit_r * hit_r && d2 < best_d2) { best_d2 = d2; best_sys = sys; best_pi = i; }

        }

    }

    if (best_sys < 0) return FALSE;

    if (out_slot)         *out_slot = best_sys;

    if (out_planet_index) *out_planet_index = best_pi;

    return TRUE;

}



// Build the galaxy-map hover tooltip text for the cursor. Runs the same two hit-tests that used to

// live at the end of draw_galaxy_map_look (map entity first, else nearest star system) but only

// *computes* the string — the RmlUi HUD draws it. Called from game_push_hud in the update path so

// the pushed snapshot is consumed by the data model the same frame (no one-frame cursor lag).

b8 galaxy_map_hover_tooltip(game_state* s, i32 mx, i32 my, char* out, i32 cap, i32* out_x, i32* out_y)

{

    if (!out || cap <= 0) return FALSE;

    out[0] = '\0';

    if (s->view_arena_w >= 1.0f) return FALSE; // galaxy-map look not on screen



    // ---- Map entity hover (ships/fleets): short name + distance -------------------------

    const MapEntity* hovered = nullptr;

    for (i32 i = 0; i < s->galaxy.map_entity_count; ++i) {

        Vec2 rel = hierpos_diff(&s->galaxy.map_entities[i].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

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

        if (dist >= 1000000.0)

            snprintf(out, (size_t)cap, "%s\nDist: %.2f M u", hovered->name ? hovered->name : "?", dist / 1000000.0);

        else if (dist >= 1000.0)

            snprintf(out, (size_t)cap, "%s\nDist: %.2f k u", hovered->name ? hovered->name : "?", dist / 1000.0);

        else

            snprintf(out, (size_t)cap, "%s\nDist: %.0f u", hovered->name ? hovered->name : "?", dist);

        if (out_x) *out_x = mx + 16;

        if (out_y) *out_y = my + 16;

        return TRUE;

    }



    // ---- Star-system hover (galaxy mode): physical properties on demand -----------------

    // Find the system nearest the cursor (true-world space), and if the

    // cursor is close to its on-screen dot, materialise it from its seed and show its star class

    // + per-planet type / temperature / habitability. Skipped when the cursor is over a UI panel.

    if (s->view_arena_w < 0.9f && !bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {

        bs_math::HierPos2 mp = mouse_true_hierpos(s);

        i32 node = galaxy_nearest_node(s, &mp);

        if (node >= 0) {

            const GalaxyNode& nd = s->galaxy.nodes[node];

            Vec2 nrel = hierpos_diff(&nd.galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

            Vec2 nscreen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, nrel);

            f32 ddx = (f32)mx - nscreen.x, ddy = (f32)my - nscreen.y;

            if (ddx * ddx + ddy * ddy <= 22.0f * 22.0f) {

                // Materialising a system now runs the full evolution pipeline, so cache the

                // last hovered node instead of regenerating every frame.

                static StarSystem s_hover_sys;

                static i32 s_hover_node = -1;

                if (node != s_hover_node) {

                    SSGenEnv env = galaxy_env_at(&s->galaxy.gen_params, &nd.galaxy_center);

                    generate_star_system(&s_hover_sys, nd.seed, Vec2{ 0.0f, 0.0f }, env);

                    s_hover_node = node;

                }

                const StarSystem& info = s_hover_sys;

                i32 off = snprintf(out, (size_t)cap,

                    "%s\n%s-class star  %.0f K\nL %.2f Lsun   %.1f Gyr\n%d planets, %d moons, %d belts:",

                    nd.name[0] ? nd.name : "?",

                    spectral_class_name(info.star_props.spectral_class),

                    info.star_props.temperature_k, info.star_props.luminosity_solar,

                    info.star_props.age_gyr, info.planet_count, info.moon_count,

                    info.evo.belt_count);

                for (i32 pi = 0; pi < info.planet_count; ++pi) {

                    if (off < 0 || off >= cap - 1) break;

                    const PlanetProperties& pp = info.planet_props[pi];

                    // Genome subtype + type + orbit/temperature.

                    off += snprintf(out + off, (size_t)cap - (size_t)off,

                        "\n  %s %s  %.2f AU  %.0f K",

                        planet_subtype_name(pp.type, pp.genome.subtype), planet_type_name(pp.type),

                        pp.orbit_au, pp.temperature_k);

                    if (off >= 0 && off < cap - 1 && pp.habitability > 0.4f)

                        off += snprintf(out + off, (size_t)cap - (size_t)off,

                            "  * habitable %.0f%%", pp.habitability * 100.0f);

                    // Up to 3 descriptor traits.

                    const char* tags[3];

                    i32 nt = planet_trait_names(pp.genome.trait_bits, tags, 3);

                    if (nt > 0 && off >= 0 && off < cap - 1) {

                        off += snprintf(out + off, (size_t)cap - (size_t)off, "\n     ");

                        for (i32 t = 0; t < nt && off >= 0 && off < cap - 1; ++t)

                            off += snprintf(out + off, (size_t)cap - (size_t)off, "%s%s", t ? ", " : "", tags[t]);

                    }

                    // Rare anomaly badge.

                    if (pp.genome.anomaly != 0 && off >= 0 && off < cap - 1)

                        off += snprintf(out + off, (size_t)cap - (size_t)off,

                            "  [! %s]", planet_anomaly_name(pp.genome.anomaly));

                }

                // Civilization (Phase 1/2): homeworld name, else present-day controller.

                i32 civ_idx = galaxy_history_civ_at_node(s, node);

                i32 owner   = galaxy_history_owner_at_node(s, node);

                if (civ_idx >= 0 && off >= 0 && off < cap - 1) {

                    const Civilization& cv = s->galaxy.civs[civ_idx];

                    if (cv.status != 0)

                        off += snprintf(out + off, (size_t)cap - (size_t)off,

                            "\n-- Homeworld of the %s --\n%s - %s - founded %d yrs ago [FALLEN]",

                            cv.name, civ_government_name(cv.government), civ_ethos_name(cv.ethos),

                            s->galaxy.clock.present_year - cv.founding_year);

                    else

                        off += snprintf(out + off, (size_t)cap - (size_t)off,

                            "\n-- Homeworld of the %s --\n%s - %s - founded %d yrs ago (%d systems)",

                            cv.name, civ_government_name(cv.government), civ_ethos_name(cv.ethos),

                            s->galaxy.clock.present_year - cv.founding_year, cv.territory_count);

                    if (cv.parent_civ >= 0 && cv.parent_civ < s->galaxy.civ_count &&

                        off >= 0 && off < cap - 1)

                        off += snprintf(out + off, (size_t)cap - (size_t)off,

                            "\nsuccessor of the %s", s->galaxy.civs[cv.parent_civ].name);

                } else if (owner >= 0 && off >= 0 && off < cap - 1) {

                    const Civilization& cv = s->galaxy.civs[owner];

                    i32 cy = s->galaxy.node_colonized_year ? s->galaxy.node_colonized_year[node] : 0;

                    off += snprintf(out + off, (size_t)cap - (size_t)off,

                        "\n-- Held by the %s --\ncolonized %d yrs ago",

                        cv.name, s->galaxy.clock.present_year - cy);

                }

                if (out_x) *out_x = mx + 16;

                if (out_y) *out_y = my + 16;

                return TRUE;

            }

        }

    }



    return FALSE;

}



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



// ---- Galaxy overview: far-system dots + travel lanes -----------------------------------------

// At 10k systems only the handful near the camera are materialised and drawn in full detail

// (Pass 1/2 below). Everything else renders as a coloured dot straight from its lightweight

// GalaxyNode, and the MST/add-back lane graph is drawn for the local neighbourhood.

//

// PERF: this pass runs the instant the galaxy look fades in (view_arena_w < 1). A naive

// implementation drew each dot as a 6-line circle (6 sprites) and every lane, emitting ~60k+

// sprites for 10k systems -> overflowed the backend's BS_MAX_SPRITES (16384) batch, which logs a

// warning per dropped sprite (tens of thousands of OutputDebugString calls) and stalled to ~1 FPS.

// The fix keeps sprite count bounded: 1 quad per dot, one dot per ~2px screen bucket (dedup so the

// dense core collapses), a hard budget under the batch cap, and lanes gated by on-screen length.

static const f32 NODE_DOT_PX             = 2.5f;

static const i32 GALAXY_OVERVIEW_BUDGET  = 12000; // stay well under BS_MAX_SPRITES (16384)

static const i32 OCC_BUCKET_PX           = 2;     // screen-space dedup bucket size

static const f32 GALAXY_LANE_MIN_SCREEN  = 6.0f;  // px: shorter lanes are invisible clutter -> skip



// Frame-stamped screen-occupancy grid: a bucket is "taken this frame" when its stamp == the

// current frame stamp, so we never memset it except on resize / stamp wrap.

static u32* g_occ = nullptr;

static i32  g_occ_w = 0, g_occ_h = 0;

static u32  g_occ_stamp = 0;



static void draw_galaxy_overview(game_state* s) {

    const game_state::GalaxyState& g = s->galaxy;

    if (g.node_count <= 0) return;

    f32 zoom     = s->camera_state.camera.zoom;

    f32 inv_zoom = zoom > 0.0f ? 1.0f / zoom : 1.0f;



    // Ensure the occupancy grid matches the framebuffer, then advance the frame stamp.

    i32 bw = (s->fb_width  + OCC_BUCKET_PX - 1) / OCC_BUCKET_PX;

    i32 bh = (s->fb_height + OCC_BUCKET_PX - 1) / OCC_BUCKET_PX;

    if (bw < 1) bw = 1; if (bh < 1) bh = 1;

    if (bw != g_occ_w || bh != g_occ_h || !g_occ) {

        if (g_occ) bs_memory_free(g_occ, sizeof(u32) * g_occ_w * g_occ_h, MEMORY_TAG_GAME);

        g_occ_w = bw; g_occ_h = bh;

        g_occ = (u32*)bs_memory_allocator(sizeof(u32) * bw * bh, MEMORY_TAG_GAME);

        for (i32 i = 0; i < bw * bh; ++i) g_occ[i] = 0;

        g_occ_stamp = 0;

    }

    if (++g_occ_stamp == 0) { for (i32 i = 0; i < bw * bh; ++i) g_occ[i] = 0; g_occ_stamp = 1; }



    i32 budget = GALAXY_OVERVIEW_BUDGET;

    f32 cull_x = (f32)s->fb_width + 32.0f, cull_y = (f32)s->fb_height + 32.0f;



    // ---- Far-system dots (nodes beyond the detailed materialisation radius) ----

    // One quad each; the first (nearest-in-array) node to claim a screen bucket wins, so the dense

    // galactic core does not emit thousands of overlapping sub-pixel dots.

    for (i32 i = 0; i < g.node_count && budget > 0; ++i) {

        Vec2 rel = hierpos_diff(&g.nodes[i].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

        if (vec2_length(rel) < (f32)GALAXY_MATERIALIZE_RADIUS) continue; // drawn in detail below

        Vec2 pos = rel;

        Vec2 sc  = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, pos);

        if (sc.x < -32.0f || sc.x > cull_x || sc.y < -32.0f || sc.y > cull_y) continue;

        i32 bx = (i32)(sc.x / OCC_BUCKET_PX), by = (i32)(sc.y / OCC_BUCKET_PX);

        if (bx < 0 || bx >= bw || by < 0 || by >= bh) continue;

        i32 bi = by * bw + bx;

        if (g_occ[bi] == g_occ_stamp) continue;   // a nearer node already claimed this pixel

        g_occ[bi] = g_occ_stamp;

        f32 vis = get_sensor_visibility(s, pos);

        if (vis <= 0.0f) continue;

        bs_color col = g.nodes[i].star_color;

        f32 dot_px = NODE_DOT_PX;

        if (g.map_draw_habitability) {

            // Habitability overlay (F10): blend the star colour toward green with the node's best

            // habitability, dim the barren majority, and enlarge life-bearing worlds so the sparse

            // civilization cradles stand out at a glance.

            f32 hab = g.nodes[i].best_habitability / 255.0f;

            col.r = col.r * (1.0f - hab) + 0.15f * hab;

            col.g = col.g * (1.0f - hab) + 1.00f * hab;

            col.b = col.b * (1.0f - hab) + 0.25f * hab;

            if (hab <= 0.0f) col.a *= 0.35f;

            else             dot_px = NODE_DOT_PX * (1.0f + 1.2f * hab);

        }

        if (g.map_draw_civs && g.node_owner) {

            // Civilization overlay (F11): paint each system in its controller's banner colour;

            // wild/unclaimed space dims out so the empires' territories read at a glance.

            i16 owner = g.node_owner[i];

            if (owner >= 0 && owner < g.civ_count) { col = g.civs[owner].color; dot_px = NODE_DOT_PX * 1.3f; }

            else col.a *= 0.25f;

        }

        col.a *= (0.40f + 0.60f * vis);

        renderer_draw_quad(pos, Vec2{ dot_px * inv_zoom, dot_px * inv_zoom }, col, LAYER_CELESTIAL);

        --budget;

    }



    // ---- Civilization homeworld markers (Phase 1): civ-coloured pips at each cradle (F11) ----

    if (g.map_draw_civs)

    for (i32 c = 0; c < g.civ_count; ++c) {

        const Civilization& cv = g.civs[c];

        if (cv.origin_node < 0 || cv.origin_node >= g.node_count) continue;

        Vec2 crel = hierpos_diff(&g.nodes[cv.origin_node].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

        Vec2 cpos = crel;

        Vec2 csc  = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, cpos);

        if (csc.x < -32.0f || csc.x > cull_x || csc.y < -32.0f || csc.y > cull_y) continue;

        f32 cvis = get_sensor_visibility(s, cpos);

        if (cvis <= 0.0f) continue;

        bs_color mc = cv.color; mc.a *= (0.55f + 0.45f * cvis);

        renderer_draw_quad(cpos, Vec2{ 3.0f * NODE_DOT_PX * inv_zoom, 3.0f * NODE_DOT_PX * inv_zoom }, mc, LAYER_CELESTIAL);

    }



    // ---- Uninhabited station-market markers (F11): white filled discs -----------------------

    // Drawn in a dedicated pass (NOT subject to the far-dot occupancy dedup) so these rare frontier

    // markets always paint and are not lost to a nearer non-station dot claiming the same 2px bucket.

    // Owned/habited systems already show their civ colour above, so this pass skips them.

    if (g.map_draw_civs && g.node_has_stations && g.node_owner) {

        i32 mk_budget = 4000;

        for (i32 i = 0; i < g.node_count && mk_budget > 0; ++i) {

            if (!g.node_has_stations[i]) continue;

            i16 owner = g.node_owner[i];

            if (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0) continue; // habited -> civ colour

            Vec2 pos = hierpos_diff(&g.nodes[i].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

            Vec2 sc  = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, pos);

            if (sc.x < -32.0f || sc.x > cull_x || sc.y < -32.0f || sc.y > cull_y) continue;

            f32 vis = get_sensor_visibility(s, pos);

            if (vis <= 0.0f) continue;

            f32 Rv = 2.0f * NODE_DOT_PX * inv_zoom;   // visual disc radius (screen-constant)

            bs_color wc = bs_color{ 1.0f, 1.0f, 1.0f, 0.55f + 0.45f * vis };

            renderer_draw_circle(pos, Rv * 0.5f, 16, Rv, wc, LAYER_CELESTIAL); // filled disc (thickness == radius)

            --mk_budget;

        }

    }



    // ---- Travel lanes (only when long enough on screen to see; global toggle-gated) ----

    if (g.map_draw_lanes && g.lanes.lane_count > 0) {

        bs_color lane_col = bs_color{ 0.25f, 0.40f, 0.55f, 0.35f };

        for (i32 l = 0; l < g.lanes.lane_count && budget > 0; ++l) {

            Vec2 ra = hierpos_diff(&g.nodes[g.lanes.lane_a[l]].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

            Vec2 rb = hierpos_diff(&g.nodes[g.lanes.lane_b[l]].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

            Vec2 pa = ra, pb = rb;

            Vec2 sa = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, pa);

            Vec2 sb = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, pb);

            if (vec2_length(vec2_sub(sb, sa)) < GALAXY_LANE_MIN_SCREEN) continue; // too short / zoomed out

            if ((sa.x < -32.0f && sb.x < -32.0f) || (sa.x > cull_x && sb.x > cull_x) ||

                (sa.y < -32.0f && sb.y < -32.0f) || (sa.y > cull_y && sb.y > cull_y)) continue;

            renderer_draw_line(pa, pb, 1.0f, lane_col, LAYER_CELESTIAL);

            --budget;

        }

    }



    // ---- Trading ships (station-issued contracts): civ-coloured ship markers ----

    // Drawn on the UI layer above the static dots/lanes and NOT sensor-gated: every active trade

    // contract IS a trading ship, so each renders as a small directional triangle -- nose along the

    // travel lane in transit, nose outward from the star while docked at a station. Brightened

    // toward white so they stand out from their owner's static system dots.

    for (i32 mi = 0; mi < g.mission_count && budget > 0; ++mi) {

        const ShipMission& m = g.missions[mi];

        if (!m.active) continue;

        bs_math::HierPos2 mp = ship_mission_position(s, m);

        Vec2 rel = hierpos_diff(&mp, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);

        Vec2 sc  = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, rel);

        if (sc.x < -32.0f || sc.x > cull_x || sc.y < -32.0f || sc.y > cull_y) continue;

        bs_color col = (m.owner >= 0 && m.owner < g.civ_count) ? g.civs[m.owner].color

                                                              : bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };

        col.r = col.r * 0.4f + 0.6f; col.g = col.g * 0.4f + 0.6f; col.b = col.b * 0.4f + 0.6f;

        col.a = 1.0f;

        // Swell the marker while docked at a station so trade stops read as brief pulses.

        b8 docked = (m.stage == MISSION_STAGE_ORIGIN_DOCK || m.stage == MISSION_STAGE_MARKET_DOCK);

        // Nose direction: toward the current movement leg's target while flying, else outward

        // from the docked station's star.

        Vec2 dir{ 0.0f, 1.0f };

        if (!docked) {

            dir = hierpos_diff(&m.leg_target, &mp, BS_HIERPOS_CELL_SIZE);

        } else if (m.at_node >= 0 && m.at_node < g.node_count) {

            dir = hierpos_diff(&mp, &g.nodes[m.at_node].galaxy_center, BS_HIERPOS_CELL_SIZE);

        }

        f32 dlen = vec2_length(dir);

        if (dlen > 1e-3f) dir = vec2_scale(dir, 1.0f / dlen); else dir = Vec2{ 0.0f, 1.0f };

        f32 marker_px = NODE_DOT_PX * (docked ? 2.8f : 1.8f);



        // ---- Zoom LOD: once the REAL hull would be bigger on screen than the map triangle, draw

        // the actual trader hull sprite at world scale instead (the marker is a map glyph only).

        // Missions bound to a live in-system trader skip entirely -- the NpcShip renders itself.

        if (s->npc_template_ready) {

            const ShipVisual& tv = s->npc_template.visual;

            f32 hull_world = (tv.size_local.x > tv.size_local.y) ? tv.size_local.x : tv.size_local.y;

            if (hull_world * zoom >= marker_px) {

                if (m.ship_slot >= 0) continue;         // live bound trader is the real ship

                f32 angle = atan2f(-dir.x, dir.y);      // ship convention: angle 0 => nose +Y

                for (i32 li = 0; li < tv.layer_count; ++li) {

                    const VisualLayer& vl = tv.layers[li];

                    if (!vl.texture.id) continue;

                    bs_sprite sp{};

                    sp.position = vec2_add(rel, vec2_rotate(vl.offset_local, angle));

                    sp.size     = tv.size_local;

                    sp.origin   = Vec2{ 0.5f, 0.5f };

                    sp.rotation = angle;

                    sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };

                    sp.tint     = col;

                    sp.texture  = vl.texture;

                    sp.blend    = BLEND_ALPHA;

                    sp.layer    = LAYER_UI;

                    renderer_draw_sprite(&sp);

                }

                --budget;

                continue;

            }

        }



        f32 px = marker_px * inv_zoom;

        Vec2 perp{ -dir.y, dir.x };

        Vec2 nose { rel.x + dir.x * px,                          rel.y + dir.y * px };

        Vec2 left { rel.x - dir.x * px * 0.7f + perp.x * px * 0.6f, rel.y - dir.y * px * 0.7f + perp.y * px * 0.6f };

        Vec2 right{ rel.x - dir.x * px * 0.7f - perp.x * px * 0.6f, rel.y - dir.y * px * 0.7f - perp.y * px * 0.6f };

        renderer_draw_line(nose, left,  1.5f, col, LAYER_UI);

        renderer_draw_line(left, right, 1.5f, col, LAYER_UI);

        renderer_draw_line(right, nose, 1.5f, col, LAYER_UI);

        --budget;

    }

}



void draw_galaxy_map_look(game_state* s, f32 dt) {

    // ---- Galaxy-map look render (was MODE_SYSTEM) -- cross-fades in by map weight ----------

    if (s->view_arena_w < 1.0f) {

        f32 map_w = 1.0f - s->view_arena_w;

        renderer_set_draw_alpha(map_w);



        // Galaxy overview: local travel lanes + far-system dots straight from the lightweight

        // node array (the ~10k systems the hot cache does not materialise in full detail).

        draw_galaxy_overview(s);



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

            Vec2 sys_pos = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star);

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

            // The hero star's OPAQUE dark pocket is drawn twice across the blend band (here + the

            // arena renderer). Two partially-transparent premult-over pockets do not composite to

            // opaque, so the nebula bleeds through mid-band. Saturate the hero pocket weight to 1 by

            // mid-band (full at map_w>=0.45, i.e. view_arena_w<=0.55), mirroring the arena pass, so

            // at least one pass fully occludes at every zoom (with a small overlap around 0.5).

            // Non-hero stars are drawn only here, so they keep the plain linear map weight.

            b8  is_hero_star = (cd.sys == s->galaxy.current_system);

            f32 star_w = is_hero_star ? clampf(map_w / 0.45f, 0.0f, 1.0f) : map_w;

            s->render.star_fx.draw_star(css, cd.star_pos, cd.star_screen, cd.scaled_base_r, cd.screen_radius,

                                 cd.vis * star_w, s->galaxy.galaxy_map_time, LAYER_CELESTIAL,

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



            // LOD/screen cull: a cached system that is off-screen or too small to show any detail

            // contributes only invisible sub-pixel orbit rings (64 line-sprites each). Skip it so

            // zoomed-out frames don't emit ~20k orbit sprites and overflow the sprite batch.

            {

                f32 z = s->camera_state.camera.zoom;

                f32 mo = 0.0f;

                for (i32 i = 0; i < ss.planet_count; ++i) mo = fmaxf(mo, ss.planets[i].semi_major_axis);

                Vec2 cst = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star);

                Vec2 sp  = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, vec2_add(cst, ss.star.position));

                f32 ext  = mo * z + 48.0f;

                if (sp.x < -ext || sp.x > (f32)s->fb_width + ext || sp.y < -ext || sp.y > (f32)s->fb_height + ext) continue;

            }



            // Depth-based parallax: per-element-type system centers. depth_* scales the

            // camera-relative anchor offset (via celestial_center_render) so the backdrop drifts

            // slower than the camera; parallax ratios stay identical across Map and Arena modes.

            Vec2 center_star   = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star);

            Vec2 center_orbit  = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_orbit);

            Vec2 center_planet = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_planet);

            Vec2 star_pos = vec2_add(center_star, ss.star.position);



            // F11 overlay: uninhabited station-market systems keep a white filled circle at the

            // star up close, matching the far-dot overlay marker so they stay identifiable.

            if (s->galaxy.map_draw_civs && s->galaxy.node_has_stations && s->galaxy.node_owner) {

                i32 node = s->galaxy.cache_node[sys];

                if (node >= 0 && node < s->galaxy.node_count && s->galaxy.node_has_stations[node]) {

                    i16 owner = s->galaxy.node_owner[node];

                    b8 habited = (owner >= 0 && owner < s->galaxy.civ_count && s->galaxy.civs[owner].status == 0);

                    if (!habited && vis > 0.0f) {

                        f32 R = 9.0f / s->camera_state.camera.zoom;   // disc radius (screen-constant)

                        bs_color wc = bs_color{ 1.0f, 1.0f, 1.0f, 0.85f * (0.40f + 0.60f * vis) };

                        renderer_draw_circle(star_pos, R * 0.5f, 16, R, wc, LAYER_CELESTIAL);

                    }

                }

            }



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



            // Planets + orbit rings — true elliptical orbit paths under the lit spheres.

            f32 inv_zoom = 1.0f / s->camera_state.camera.zoom;

            f32 max_orbit = 0.0f;

            for (i32 i = 0; i < ss.planet_count; ++i) {

                const CelestialBody& p = ss.planets[i];

                max_orbit = fmaxf(max_orbit, p.semi_major_axis);

                if (!s->render.celestial_draw_planets) continue; // editor toggle (max_orbit still needed)

                if (p.semi_major_axis * s->camera_state.camera.zoom < 6.0f) continue; // orbit sub-6px: skip planet

                // Orbit ring — the true ellipse (focus at the star, centre offset by a*e along the
                // rotated major axis), as 64 chords at screen-constant ~1px thickness. Alpha fades
                // in just past the sub-6px LOD gate so rings don't pop when zooming.
                {
                    f32 orbit_px = p.semi_major_axis * s->camera_state.camera.zoom;
                    f32 fade = clampf((orbit_px - 6.0f) / 18.0f, 0.0f, 1.0f);
                    bs_color ring_col = p.color; ring_col.a = 0.25f * vis * map_w * fade;
                    if (ring_col.a > 0.01f) {
                        f32 rcw = cosf(p.arg_periapsis), rsw = sinf(p.arg_periapsis);
                        f32 semi_minor = p.semi_major_axis * sqrtf(1.0f - p.eccentricity * p.eccentricity);
                        const i32 SEGMENTS = 64;
                        Vec2 prev{};
                        for (i32 k = 0; k <= SEGMENTS; ++k) {
                            f32 E = (f32)k / (f32)SEGMENTS * 2.0f * BS_PI;
                            f32 ex = p.semi_major_axis * (cosf(E) - p.eccentricity);
                            f32 ey = semi_minor * sinf(E);
                            Vec2 pt = vec2_add(center_orbit, Vec2{ rcw * ex - rsw * ey, rsw * ex + rcw * ey });
                            if (k > 0) renderer_draw_line(prev, pt, inv_zoom, ring_col, LAYER_CELESTIAL);
                            prev = pt;
                        }
                    }
                }

                Vec2 planet_off = p.position;

                Vec2 planet_vis = vec2_add(center_planet, planet_off);

                // Size the sphere like the star: physical radius * per-type multiplier * zoom, with a

                // per-type min-px floor when zoomed out and a max-px cap so zooming in keeps the planet

                // at a comfortable distance (a sphere with space around it, never a screen-filling

                // close-up).

                const PlanetTypeParams& pe = s->render.star_fx.planet_params[(i32)ss.planet_props[i].type];

                f32 planet_max_px = 0.32f * (f32)s->fb_height;

                f32 planet_body_px = clampf(p.radius * pe.size_mul * s->camera_state.camera.zoom, pe.min_px, planet_max_px);

                Vec2 pscreen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, planet_vis);

                s->render.star_fx.draw_planet_3d(p, ss.planet_props[i], ss.star.color, pscreen,

                                                 planet_body_px, vis * map_w, s->elapsed_time,

                                                 LAYER_CELESTIAL, s->fb_width, s->fb_height);

            }



            // Moons — small lit spheres riding their parent planet (same gates as planets).

            for (i32 m = 0; m < ss.moon_count; ++m) {

                if (!s->render.celestial_draw_planets) break;

                const CelestialBody& mn = ss.moons[m];

                if (mn.radius <= 0.0f) continue;

                // Skip when the moon's parent-relative orbit is sub-pixel (reads as noise).

                if (mn.semi_major_axis * s->camera_state.camera.zoom < 4.0f) continue;

                Vec2 moon_vis = vec2_add(center_planet, mn.position);

                const PlanetTypeParams& me = s->render.star_fx.planet_params[(i32)ss.moon_props[m].type];

                f32 moon_max_px = 0.20f * (f32)s->fb_height;

                f32 moon_body_px = clampf(mn.radius * me.size_mul * s->camera_state.camera.zoom, me.min_px * 0.6f, moon_max_px);

                Vec2 mscreen = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, moon_vis);

                s->render.star_fx.draw_planet_3d(mn, ss.moon_props[m], ss.star.color, mscreen,

                                                 moon_body_px, vis * map_w, s->elapsed_time,

                                                 LAYER_CELESTIAL, s->fb_width, s->fb_height);

            }



            // ---- Test sprites: colored dots orbiting the CURRENT star (volumetric light demo)

            // Editor-gated (celestial_draw_testsprites, default OFF): dev-only content.

            if (sys == s->galaxy.current_system && s->render.celestial_draw_testsprites) {

                const i32 TEST_COUNT = 8;

                // Test sprites orbit the star; anchor them at the depth_testsprite center so they

                // stay co-located with the star (default depth_testsprite == depth_star).

                Vec2 ts_center = vec2_add(celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_testsprite), ss.star.position);

                for (i32 ti = 0; ti < TEST_COUNT; ++ti) {

                    f32 t_angle = (f32)ti / (f32)TEST_COUNT * 2.0f * BS_PI + s->galaxy.galaxy_map_time * 0.3f;

                    f32 t_orbit = max_orbit * 0.3f + max_orbit * 0.7f * ((f32)ti / (f32)TEST_COUNT);

                    Vec2 tpos = Vec2{

                        ts_center.x + cosf(t_angle) * t_orbit,

                        ts_center.y + sinf(t_angle) * t_orbit

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



        // Hover tooltips (map entity + star system) are built game-side in game_push_hud via

        // galaxy_map_hover_tooltip() and drawn by the RmlUi HUD (#tooltip). The old ImGui

        // bs_ui_tooltip_at calls were retired here so the tooltip renders the same frame it is

        // computed (update path) instead of lagging a frame from this render pass.



        // Restore full opacity for the arena/gameplay passes that follow.

        renderer_set_draw_alpha(1.0f);

    }

}

