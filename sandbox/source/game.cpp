#include "game.h"

// Aggregate include of every extracted module header. game.cpp is the frame orchestrator that
// calls into all modules, so it (and only it) pulls the whole cascade.
#include "game_modules.h"

#include "text.h"

#include "travel.h"

#include "coord_diag.h"

#include "weapon.h"

#include "projectile.h"

#include "ss_generation.h"

#include "voronoi_cell_hover_effect.h"

#include "global_background.h"

#include "mapped_system_layer.h"

#include <core/logger.h>

#include <core/input.h>

#include <math/math_utils.h>

#include <math/bs_hierpos.h>

#include <renderer/renderer.h>

#include <renderer/camera2d.h>

#include <renderer/bs_imgui.h> // bs_imgui_wants_mouse: gate world input while ImGui owns the cursor

#include <renderer/bs_ui.h>

#include <math.h>   // powf

#include <stdio.h>  // snprintf, vsnprintf

#include <stdarg.h> // va_list, va_start, va_end (action_log_push)

#include <new>      // placement-new (construct game_state in place; see game_init)

#include <chrono>   // std::chrono::steady_clock for heat map timing

using namespace bs_math;

// =====================================================================================

// Tuning constants.

// =====================================================================================

// ---- Camera / zoom ----

// ZOOM_MIN / ZOOM_MAX / ZOOM_STEP / ZOOM_GLOBAL_MIN now live in sim/camera_controller.cpp
// (used only by update_zoom_and_mode).

static const f32 ZOOM_START        = 0.50f;  // begins in global mode

static const f32 ZOOM_SYSTEM       = 0.06f;  // system-view zoom (wide enough for 3-planet orbits)

// ---- STEP 2: zoom-parameterized view blend (arena look <-> galaxy-map look) ----

// The arena<->galaxy-map cross-fade weight (view_arena_weight) and its VIEW_MAP_ZOOM /

// VIEW_ARENA_ZOOM band now live in core/view_transform.cpp (declared via core/view_transform.h).

// HEAT_FADE_FULL_ZOOM / HEAT_FADE_ZERO_ZOOM and the radiation heat map
// (heat_map_fade_weight / draw_ship_metaballs) now live in sim/heat_map.cpp
// (declared via sim/heat_map.h, included by game.h).

// ---- Star zoom-distance scaling (MODE_SYSTEM) ----
// STAR_MIN_SCREEN_RADIUS / STAR_DIST_SCALE_FACTOR / STAR_MAX_DIST_SCALE / STAR_HERO_MAP_MIN_RADIUS
// now live in render/galaxy_map_render.cpp (used only by the galaxy-map look pass).

static const f32 STAR_MAX_STREAK_LENGTH = 20.0f;

static const f32 GALAXY_PLANET_SCALE      = 0.00006f; // visually shrink planet orbits for galaxy view (~5-30 px orbits at default zoom)

// ---- Edit mode camera pan (WASD + middle-mouse drag) ----

static const f32 EDIT_PAN_SPEED    = 2000.0f; // units/s, camera pan in edit mode

// ---- Ship movement (global mode): Starsector-style inertial flight ----

// Linear: W accelerates forward (along heading) toward SHIP_MAX_SPEED using SHIP_ACCEL.

// S accelerates in reverse toward SHIP_MAX_SPEED using SHIP_DECEL. C brakes the current

// velocity toward zero using SHIP_DECEL. Strafe (Q/E) thrusts sideways at SHIP_ACCEL.

// The ship coasts: no passive drag, only active thrust/brake.

const f32 SHIP_ACCEL        = 600.0f;  // forward / strafe thrust (units/s^2)

const f32 SHIP_DECEL        = 400.0f;  // reverse + brake thrust (units/s^2)

const f32 SHIP_MAX_SPEED    = 800.0f;  // linear speed cap (units/s)

// Angular: A/D ramp angular velocity using SHIP_TURN_ACCEL toward +/- SHIP_MAX_TURN.

// Releasing both lets SHIP_TURN_ACCEL bleed the spin back to zero (auto-stabilize).

const f32 SHIP_TURN_ACCEL   = 12.0f;   // rad/s^2

const f32 SHIP_MAX_TURN     = 3.0f;    // rad/s

// ---- Engine exhaust tuning (EXHAUST_* consts) now live in render/ship_scene.cpp.

// ---- Procedural generation PRNG + system-name table + MIN_SYSTEM_SEPARATION now live in
// sim/galaxy_map.cpp (used only by the galaxy generation that moved into galaxy_map_init). ----

// ---- Render layers (lower draws first) ---- now shared via core/render_layers.h
#include "core/render_layers.h"

// Debug collider outline colour (COLLIDER_COLOR) now lives in render/ship_scene.cpp.

// DEBUG: draw the HierPos2 cell-grid overlay (boundaries + parity checker + labels + HUD).
// Toggled from the COORDINATE SPACE editor panel; off by default, MODE_GLOBAL only.
// g_debug_cell_grid now lives in render/debug_overlay.cpp (extern via render/debug_overlay.h).

// =====================================================================================

// Action Log helpers (action_log_push / build_action_log_panel) now live in
// sim/action_log.cpp (declared via sim/action_log.h, included by game.h).

// =====================================================================================

// Galaxy coordinate helpers (find_nearest_system / galaxy_to_system_local /
// world_to_galaxy_pos / get_system_zone) now live in core/galaxy_coords.cpp
// (declared via core/galaxy_coords.h, included by game.h).

// =====================================================================================

// =====================================================================================

// Lifecycle.

// =====================================================================================

b8 game_init(Game* game_inst) {

    BS_LOG_DEBUG("game_init: tile-ship prototype starting.");

    game_state* s = (game_state*)game_inst->state;

    if (!s) return FALSE;

    // Construct game_state in place with placement-new: value-initializing the whole struct

    // (including the two Ship members, each ~3 MB with embedded SysGraph arrays)

    // as a STACK TEMPORARY would blow the ~1 MB Windows default stack. Placement-new calls

    // the constructor on the already-heap-allocated memory from bs_memory_allocator.

    new (s) game_state();

    new (&s->rts_controls) RtsControls(s);

    s->fb_width  = 1280;

    s->fb_height = 720;

    // Editor lights start empty (scene fullbright). The player spawns them from the EDITOR PANEL.

    s->render.lights           = {};

    s->render.light_ambient    = bs_color{ 0.18f, 0.19f, 0.24f, 1.0f }; // dim cool floor

    s->render.light_selected   = -1;

    // Default glow params (match hardcoded shader defaults).

    s->render.glow_params = bs_glow_params{

        1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,

        bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },

        bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },

        bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },

        bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }

    };

    // Bloom defaults (disabled by default until user opts in).

    s->render.bloom_enabled    = FALSE;

    s->render.bloom_threshold  = 1.2f;

    s->render.bloom_intensity  = 0.3f;

    s->render.dynamic_bloom    = TRUE;

    s->render.star_light_enabled     = TRUE;

    s->render.bg_layer0_enabled      = TRUE;

    s->render.bg_layer1_enabled      = TRUE;

    s->render.bg_layer2_enabled      = TRUE;

    s->render.bg_nebula_enabled      = TRUE;

    s->render.bg_parallax_enabled    = TRUE;

    s->render.depth_star             = 0.85f;   // star sits deep in the backdrop

    s->render.depth_planet           = 0.50f;   // planets mid-depth (== orbit so they stay on the ring)

    s->render.depth_orbit            = 0.50f;   // orbit rings mid-depth

    s->render.depth_testsprite       = 0.85f;   // demo dots orbit the star (== star to stay co-located)

    s->render.bg_parallax_fade_zoom  = 0.05f;   // parallax gone at/below this zoom (matches map look start)

    s->render.nebula_intensity       = 0.5f;

    s->render.nebula_dust_intensity  = 0.4f;

    s->render.nebula_gas_color_a     = bs_color{0.05f, 0.20f, 0.40f, 1.0f}; // deep teal

    s->render.nebula_gas_color_b     = bs_color{0.10f, 0.60f, 0.35f, 1.0f}; // vivid green

    s->render.nebula_gas_color_c     = bs_color{0.85f, 0.75f, 0.15f, 1.0f}; // warm yellow

    s->render.nebula_dust_color      = bs_color{0.02f, 0.015f, 0.015f, 1.0f}; // brown-black

    s->render.nebula_gas_brightness_mul = 1.0f;

    s->render.nebula_highlight_power    = 1.0f;

    s->render.nebula_palette_shift      = 0.0f;

    s->render.nebula_swirl_strength     = 0.8f;

    s->render.nebula_falloff_radius     = 0.7f;

    s->render.nebula_band_strength      = 0.5f;

    s->render.nebula_lod_target         = 3500.0f;

    s->render.nebula_parallax           = 0.08f;

    s->render.nebula_biome_strength     = 0.6f;

    s->render.nebula_biome_scale        = 55000.0f;

    s->render.nebula_biome_hue_spread   = 0.5f;

    s->render.nebula_zoom_detail        = 0.6f;

    s->render.nebula_zoom_saturation    = 0.5f;

    s->render.starfield_lod_density    = 0.5f;

    s->render.starfield_lod_size       = 0.25f;

    s->render.starfield_lod_brightness = 3.0f;

    s->render.starfield_lod_target_px  = 12.0f;

    s->render.starfield_lod_levels     = 6.0f;

    s->render.starfield_lod_factor     = 4.0f;

    s->render.starfield_parallax_near   = 0.009f;

    s->render.starfield_parallax_falloff = 1.0f;

    s->star_pos = bs_math::Vec2{0,0};

    s->render.star_dazzle_inner_radius = 5000.0f;

    s->render.star_dazzle_outer_radius = 15000.0f;

    s->render.star_dazzle_intensity    = 1.0f;

    s->render.star_light_intensity_mul = 1.0f;

    s->render.star_light_radius_mul    = 1.0f;

    // Per-entity glow defaults (all start identical to global defaults).

    s->render.exhaust_glow = s->render.glow_params;

    s->render.bullet_glow  = s->render.glow_params;

    s->editor.edit_mode_active = FALSE;

    s->editor.edit_selection   = EditSelection{ EDIT_NONE, -1 };

    s->editor.edit_drag        = EditorDrag{ FALSE, EDIT_DRAG_NONE, bs_math::HierPos2{}, bs_math::HierPos2{}, 0.0f };

    s->action_log.count = 0;

    s->action_log.inactivity_timer = 0.0f;

    s->view.alt_movement_active = FALSE;

    // ---- Fleet: flagship (member 0) loaded from the player hull --------------------------

    s->fleet_state.fleet.init();

    {

        FleetShip& fs = s->fleet_state.fleet.add();

        if (!ship_load(&fs.ship, "assets/ships/ship/ship.ship")) {

            BS_LOG_FATAL("game_init: failed to load player ship.");

            return FALSE;

        }

        fs.ship_type = SHIP_TYPE_DRONE;

        fs.ship.origin = hierpos_from_vec2(Vec2{ 25000.0f, 0.0f }, BS_HIERPOS_CELL_SIZE);

        fs.ship.glow   = s->render.glow_params;

        fs.ship.radiation_emission = 0.05f;

        for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) fs.ship.weapons[i] = nullptr;

        fs.ship.weapons[0]       = weapon_create_ballistic_cannon(fs.ship.faction);

        fs.ship.weapon_count     = 1;

        fs.ship.active_weapon_idx = 0;

    }

    // ---- Fleet: demo escort ships (members 1..N) -----------------------------------------

    {

        const VesselFaction player_faction = s->fleet_state.fleet.flagship().ship.faction;

        const Vec2 escort_offsets[4] = {

            Vec2{ 24000.0f,  1500.0f },

            Vec2{ 24000.0f, -1500.0f },

            Vec2{ 22500.0f,  1500.0f },

            Vec2{ 22500.0f, -1500.0f },

        };

        for (i32 i = 0; i < 4; ++i) {

            FleetShip& fs = s->fleet_state.fleet.add();

            if (!ship_load(&fs.ship, "assets/ships/ship/ship.ship")) {

                BS_LOG_ERROR("game_init: failed to load escort ship %d.", i);

                continue;

            }

            fs.ship_type = SHIP_TYPE_DRONE;

            fs.ship.origin      = hierpos_from_vec2(escort_offsets[i], BS_HIERPOS_CELL_SIZE);

            fs.ship.faction     = player_faction;

            fs.ship.vessel_name = "Escort";

            fs.ship.glow        = s->render.glow_params;

            fs.ship.radiation_emission = 0.05f;

            for (i32 w = 0; w < SHIP_MAX_WEAPONS; ++w) fs.ship.weapons[w] = nullptr;

            fs.ship.weapons[0]        = weapon_create_ballistic_cannon(fs.ship.faction);

            fs.ship.weapon_count      = 1;

            fs.ship.active_weapon_idx = 0;

        }

    }

    if (!ship_load(&s->fleet_state.enemy_ship, "assets/enemy_ship.ship")) {

        BS_LOG_FATAL("game_init: failed to load enemy ship.");

        return FALSE;

    }

    s->fleet_state.enemy_ship.origin     = hierpos_from_vec2(Vec2{ 1e4f, 0.0f }, BS_HIERPOS_CELL_SIZE);

    s->fleet_state.enemy_ship.angle      = 2.36f;

    s->fleet_state.enemy_orbit_phase     = 0.0f;

    s->fleet_state.enemy_ship.faction    = VESSEL_PIRATE;

    s->fleet_state.enemy_ship.vessel_name = "Raider-class Interceptor";

    s->fleet_state.enemy_ship.glow = s->render.glow_params;

    s->fleet_state.enemy_ship.radiation_emission = 1.0f;

    // ---- Enemy weapon inventory ----------------------------------------------------------

    s->fleet_state.enemy_ship.weapon_count = 0;

    s->fleet_state.enemy_ship.active_weapon_idx = -1;

    for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) s->fleet_state.enemy_ship.weapons[i] = nullptr;

    s->fleet_state.enemy_ship.weapons[0] = weapon_create_ballistic_cannon(s->fleet_state.enemy_ship.faction);

    s->fleet_state.enemy_ship.weapon_count = 1;

    s->fleet_state.enemy_ship.active_weapon_idx = 0;

    // ---- Combat arena: projectile pool + combat entities + sensor/heat/encounter tunables --
    // (projectiles, combat_entities registration, out_sensor_fx, heat-signature params and the
    //  encounter state now live in sim/combat_arena.cpp -> combat_arena_init.)
    combat_arena_init(s);

    // Camera starts zoomed out (global mode), centered on the flagship.

    s->camera_state.camera          = camera2d_default();

    s->camera_state.camera.zoom     = ZOOM_START;

    s->camera_state.target_zoom     = ZOOM_START;

    s->camera_state.zoom_smooth_rate = 16.0f;

    s->camera_state.camera.position = hierpos_to_vec2(&s->player_ship().origin, BS_HIERPOS_CELL_SIZE);

    s->view.mode            = MODE_GLOBAL;

    // Floating-origin camera starts at system 0's galaxy center.

    s->camera_state.camera_hierpos = s->galaxy.systems[0].galaxy_center;

    // Drag anchors zeroed so system-view middle-mouse panning starts clean.

    s->camera_state.system_drag_cam    = Vec2{ 0.0f, 0.0f };

    s->camera_state.system_drag_world  = Vec2{ 0.0f, 0.0f };

    s->camera_state.system_zoom        = ZOOM_SYSTEM;

    // Galaxy cluster generation + all galaxy-map animation/draw/recenter state now lives in
    // sim/galaxy_map.cpp (galaxy_map_init).
    galaxy_map_init(s);

    // (Sensor range, heat-signature params and encounter state are set by combat_arena_init above.)
    s->time_scale             = 1.0f;

    s->elapsed_time           = 0.0f;

    // (Galaxy cluster generation + galaxy-map state init moved to galaxy_map_init above.)

    // Global-mode flight starts at rest (flagship).

    s->player_flight().velocity         = Vec2{ 0.0f, 0.0f };

    s->player_flight().angular_velocity = 0.0f;

    // Travel system: disabled by default; zero-init so the first frame is idle.

    s->travel_enabled = FALSE;

    s->travel_paused  = FALSE;

    s->travel = {};

#if BS_DEBUG

    if (!bs_hierpos_selftest()) {

        BS_LOG_FATAL("game_init: bs_hierpos_selftest failed; hierarchical coordinates are broken.");

        return FALSE;

    }

#endif

    renderer_set_clear_color(bs_color{ 0.03f, 0.03f, 0.06f, 1.0f });

    // Bake the bitmap-font atlas now that the renderer is live (it backs the HUD/UI text).

    if (!text_init()) {

        BS_LOG_ERROR("game_init: text_init failed; HUD text will be disabled.");

    }

    // Generate a soft radial-gradient texture for engine exhaust plumes.

    // White center with smooth alpha falloff — when tinted and additively blended,

    // it produces a natural tapered jet shape instead of a hard rectangle.

    {

        const u32 EX_SIZE = 64;

        const u32 EX_HALF = EX_SIZE / 2;

        u8 ex_pixels[EX_SIZE * EX_SIZE * 4];

        for (u32 y = 0; y < EX_SIZE; ++y) {

            for (u32 x = 0; x < EX_SIZE; ++x) {

                f32 dx = (f32)x - EX_HALF + 0.5f;

                f32 dy = (f32)y - EX_HALF + 0.5f;

                f32 dist = sqrtf(dx * dx + dy * dy) / (f32)EX_HALF;

                f32 t = clampf(dist, 0.0f, 1.0f);

                f32 alpha = 1.0f - t * t * t;  // cubic falloff for smooth plume shape

                u32 i = (y * EX_SIZE + x) * 4;

                ex_pixels[i + 0] = 255;

                ex_pixels[i + 1] = 255;

                ex_pixels[i + 2] = 255;

                ex_pixels[i + 3] = (u8)(alpha * 255.0f);

            }

        }

        s->render.exhaust_texture = renderer_create_texture(ex_pixels, EX_SIZE, EX_SIZE);

    }

    // ---- Ship type emblems for system-mode battlefield -----------------------------------

    s->render.emblem_drone     = renderer_load_texture("assets/fleet/emblems/fleet_emblem_type_drone.png");

    s->render.emblem_extractor = renderer_load_texture("assets/fleet/emblems/fleet_emblem_type_extractor.png");

    if (!s->render.emblem_drone.id)     BS_LOG_WARN("game_init: failed to load drone emblem texture.");

    if (!s->render.emblem_extractor.id) BS_LOG_WARN("game_init: failed to load extractor emblem texture.");

    // Initialize star visual effect system (generates procedural textures).

    s->render.star_fx.init();

    // Initialize global-mode parallax background (layers + mapped system).

    s->render.global_background.init(s, &s->render.star_fx);

    // Initialize the per-subsystem CPU profiler.

    s->profiler.init();

    // Resolve sprite textures now that the renderer is live.

    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i)

        ship_visual_resolve_textures(&s->fleet_state.fleet.at(i).ship.visual);

    ship_visual_resolve_textures(&s->fleet_state.enemy_ship.visual);

    return TRUE;

}

// =====================================================================================

// Update.

// =====================================================================================

// compression_factor / cosmetic_compress / game_compression_factor / view_arena_weight and the
// screen<->true-world<->render-space transforms (game_screen_to_true_world/render, camera_center,
// hierpos variants) now live in core/view_transform.cpp (declared via core/view_transform.h,
// included by game.h).

// Camera to hand the parallax background. Its layers scroll by cam.position * parallax, but
// camera.position is a render-space residual under the floating-origin path, so they must be driven
// by the absolute camera center instead, or the background would sit still while the ship moves.
// MappedSystemLayer (parallax 1.0) re-derives the residual internally from camera_hierpos, so this
// single center works for every layer.
// bg_cam_for_parallax now lives in render/parallax_background.cpp (used only by that pass).

// update_zoom_and_mode now lives in sim/camera_controller.cpp
// (declared via sim/camera_controller.h, included by game.h).

// draw_rotated_rect_outline now lives in render/galaxy_map_render.cpp
// (used only by the galaxy-map look pass).

// World-space position under the mouse cursor.
// mouse_world / mouse_true_world / mouse_true_hierpos now live in core/cursor_world.cpp
// (declared via core/cursor_world.h, included by game.h).

// ---- Edit mode picking -----------------------------------------------------------------

// point_in_polygon / point_to_segment now live in core/geom2d.cpp (core/geom2d.h).
// The in-world entity editor (edit_entity_position/angle, edit_entity_set_*, edit_pick,
// gizmo_* helpers, edit_pick_gizmo, update_edit_mode) now lives in sim/editor_tools.cpp
// (declared via sim/editor_tools.h, included by game.h).

// ---- Ship CONTROL: pilot input -> forces.
// control_ship_global now lives in sim/ship_control.cpp
// (declared via sim/ship_control.h, included by game.h).

// ---- Ship SIMULATION: momentum -> motion. Now owned by Fleet::simulate_all (per-ship

// FleetShip::simulate). Every fleet member integrates its own pose every frame; the piloted

// ship uses the pilot's turn_commanded flag while the rest auto-stabilize residual spin.

// =====================================================================================

// Ship-ship collision response (resolve_ship_collision) and piloted_ship_origin now live in
// sim/ship_control.cpp (declared via sim/ship_control.h, included by game.h).

// =====================================================================================
// build_action_log_panel moved to sim/action_log.cpp (declared via sim/action_log.h).
// =====================================================================================

// ---- Floating-origin re-centering (big_space-style) -----------------------------------------

// Fold the galaxy-map camera's accumulated pan (camera.position, a small f32 Vec2) back into the

// hierarchical reference cell (camera_hierpos) each frame, then reset the residual to zero. This

// keeps every on-screen star/orbit rendered near the origin in f32 (hierpos_diff stays small), so

// circle tessellation never quantizes into dashed outlines for systems far from Sol.

//

// Seamlessness: when compression_factor == 1 the rendered position is hierpos_diff(entity,

// camera_hierpos) and the view subtracts camera.position. Folding `delta` shifts both by -delta,

// which cancels exactly -> no visible pop. Below the compression threshold cosmetic_compress

// scales hierpos_diff but not camera.position, so folding would not cancel; we skip the rebase

// there (stars are sub-pixel at that zoom, so large coordinates are invisible anyway).

static void galaxy_camera_rebase(game_state* s) {

    if (compression_factor(s->camera_state.camera.zoom) < 1.0f) return; // extreme zoom-out: skip (see note)

    Vec2 delta = s->camera_state.camera.position;

    if (delta.x == 0.0f && delta.y == 0.0f) return; // nothing accumulated this frame

    // Absorb the pan into the hierarchical reference (re-canonicalizes cell + local internally).

    s->camera_state.camera_hierpos  = hierpos_add_f64(&s->camera_state.camera_hierpos, (f64)delta.x, (f64)delta.y,

                                         BS_HIERPOS_CELL_SIZE);

    s->camera_state.camera.position = Vec2{ 0.0f, 0.0f };

    // Persistent camera-space anchors must shift by the same -delta to stay valid across the

    // rebase. (system_drag_world is screen-space pixels and is intentionally left untouched.)

    s->camera_state.system_drag_cam         = vec2_sub(s->camera_state.system_drag_cam,         delta);

    // map_recenter_from_pos / target_pos are HierPos2 (absolute), so they are invariant under the

    // floating-origin rebase and need no adjustment here.

}

// update_enemy_orbit now lives in sim/combat_arena.cpp (combat_arena_update_enemy_orbit).

b8 game_update(Game* game_inst, f32 dt) {

    game_state* s = (game_state*)game_inst->state;

    if (!s) return TRUE;

    s->perf_frame_start_ns = (f64)std::chrono::steady_clock::now().time_since_epoch().count();

    s->profiler.begin_frame();
    s->profiler.set_wall_dt(dt); // real frame delta (before the hitch clamp below)
    {
        f32 eng_render_ms = 0.0f, eng_present_ms = 0.0f;
        renderer_get_frame_timing(&eng_render_ms, &eng_present_ms);
        s->profiler.set_present_ms(eng_present_ms);
    }

    BS_PROFILE(&s->profiler, PROF_UPDATE_TOTAL);

    if (dt > 0.05f) dt = 0.05f; // clamp hitches

    f32 sim_dt = dt * s->time_scale;

    if (sim_dt > 0.05f) sim_dt = 0.05f; // still clamp scaled hitches

    s->profiler.begin(PROF_OUT_SENSOR_FX);

    s->out_sensor_fx.update(dt);

    s->profiler.end(PROF_OUT_SENSOR_FX);

    // ---- Encounter detection: blob merge (sim/combat_arena.cpp) --------------------------
    combat_arena_update_encounter(s);

    s->galaxy.galaxy_map_time += sim_dt;

    // ---- SHIFT key: toggle alternative mouse-follow movement system in global mode -----------

    if (input_is_key_down(KEY_LSHIFT) && !input_was_key_down(KEY_LSHIFT) && s->view.mode == MODE_GLOBAL && !s->camera_state.free_camera_active) {

        s->view.alt_movement_active = !s->view.alt_movement_active;

        if (s->view.alt_movement_active)

            action_log_push(s, "Alternative movement system activated.");

        else

            action_log_push(s, "Alternative movement system deactivated.");

    }

    // ---- R key: toggle radiation detector overlay in global mode -------------------------

    if (input_is_key_down(KEY_R) && !input_was_key_down(KEY_R) && s->view.mode == MODE_GLOBAL) {

        s->show_metaball_ui = !s->show_metaball_ui;

        action_log_push(s, "Radiation detector %s.", s->show_metaball_ui ? "ON" : "OFF");

    }

    // ---- TAB key: recenter the camera on the player ship, then follow it (works at any zoom) --

    if (input_is_key_down(KEY_TAB) && !input_was_key_down(KEY_TAB)) {

        s->galaxy.map_recentering       = TRUE;

        s->galaxy.map_recenter_t        = 0.0f;

        s->galaxy.map_recenter_from_pos = game_camera_center_hierpos(s); // true-world start center for the ease

        s->camera_state.free_camera_active    = TRUE;                  // detached while animating; follow at the end

        action_log_push(s, "'TAB' key pressed - camera recentering on player ship");

    }

    // ---- M key: retired. The arena <-> galaxy-map "look" is now driven continuously by zoom, so
    // there is no discrete mode to toggle here anymore. ----------------------------------------

    // ---- P key: toggle detached free camera <-> following the piloted ship (works at any zoom) --

    if (input_is_key_down(KEY_P) && !input_was_key_down(KEY_P)) {

        if (!s->camera_state.free_camera_active) {

            s->camera_state.free_camera_active = TRUE;

            s->camera_state.free_camera_pos    = game_camera_center_hierpos(s);

            action_log_push(s, "Free camera active.");

        } else {

            s->camera_state.free_camera_active = FALSE;

            action_log_push(s, "Free camera disabled - following ship.");

        }

    }

    // ---- Profiling A/B toggles (F6..F9): isolate GPU fill cost of individual layers ----------

    if (input_is_key_down(KEY_F6) && !input_was_key_down(KEY_F6)) {

        s->render.bg_nebula_enabled = s->render.bg_nebula_enabled ? FALSE : TRUE;

        action_log_push(s, s->render.bg_nebula_enabled ? "[F6] Nebula ON" : "[F6] Nebula OFF");

    }

    if (input_is_key_down(KEY_F7) && !input_was_key_down(KEY_F7)) {

        s->render.bg_layer0_enabled = s->render.bg_layer0_enabled ? FALSE : TRUE;

        action_log_push(s, s->render.bg_layer0_enabled ? "[F7] Starfield ON" : "[F7] Starfield OFF");

    }

    if (input_is_key_down(KEY_F8) && !input_was_key_down(KEY_F8)) {

        s->galaxy.map_draw_lanes = s->galaxy.map_draw_lanes ? FALSE : TRUE;

        action_log_push(s, s->galaxy.map_draw_lanes ? "[F8] Map lanes ON" : "[F8] Map lanes OFF");

    }

    if (input_is_key_down(KEY_F9) && !input_was_key_down(KEY_F9)) {

        b8 imm = renderer_is_present_immediate() ? FALSE : TRUE;

        renderer_set_present_mode(imm);

        s->profiler.present_immediate = imm;

        action_log_push(s, imm ? "[F9] Present: IMMEDIATE (uncapped)" : "[F9] Present: VSYNC");

    }

    // Cache which system the ship is in (always updated for gameplay logic).

    s->galaxy.current_system = find_system_by_cell(&s->galaxy.map_entities[0].galaxy_pos, &s->galaxy.galaxy_voronoi, s->galaxy.systems);

    update_zoom_and_mode(s, dt);

    // EDIT MODE: click-to-select, drag-to-reposition ships and lights. While active, the flight

    // simulation is suspended so dragging the player ship doesn't fight the integrator.

    update_edit_mode(s);

    // CONTROL: in global mode, WASD/Q/E pilot the ship directly. In system mode, no ship control.

    // turn_commanded reports whether a turn is actively commanded this frame; when FALSE the

    // simulator auto-stabilizes any carried-over spin.

    b8 turn_commanded = FALSE;

    // The manually-piloted fleet member (defaults to the flagship). Clamped to a valid index.

    i32 piloted_idx = s->rts_controls.piloted_index();

    if (piloted_idx < 0 || piloted_idx >= s->fleet_state.fleet.count()) piloted_idx = 0;

    if (s->editor.edit_mode_active) {

        // Editing: freeze ALL fleet flight so dragged poses stay put. Kill residual velocity so

        // ships don't drift the instant edit mode is switched off.

        for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {

            s->fleet_state.fleet.at(i).flight.velocity = Vec2{ 0.0f, 0.0f };

            s->fleet_state.fleet.at(i).flight.angular_velocity = 0.0f;

        }

        // ---- Edit-mode camera panning --------------------------------------------------------

        // WASD pans the camera so the user can navigate around while placing/moving entities.

        // Middle-mouse drag also pans (same screen-space logic as system view).

        f32 pan_speed = EDIT_PAN_SPEED * dt;

        Vec2 pan = Vec2{ 0.0f, 0.0f };

        if (input_is_key_down(KEY_W)) pan.y += pan_speed;

        if (input_is_key_down(KEY_S)) pan.y -= pan_speed;

        if (input_is_key_down(KEY_A)) pan.x -= pan_speed;

        if (input_is_key_down(KEY_D)) pan.x += pan_speed;

        s->camera_state.camera.position = vec2_add(s->camera_state.camera.position, pan);

        // Middle-mouse drag: screen-space delta -> world delta (no live-camera feedback loop).

        if (!input_is_button_down(BUTTON_MIDDLE)) {

            s->galaxy.map_drag_needs_fresh_press = FALSE;

        }

        if (!s->galaxy.map_drag_needs_fresh_press && input_is_button_down(BUTTON_MIDDLE)) {

            if (!input_was_button_down(BUTTON_MIDDLE)) {

                s->camera_state.system_drag_cam = s->camera_state.camera.position;

                i32 mx, my;

                input_get_mouse_position(&mx, &my);

                s->camera_state.system_drag_world = Vec2{ (f32)mx, (f32)my };

            } else {

                i32 mx, my;

                input_get_mouse_position(&mx, &my);

                Vec2 screen_delta = Vec2{ (f32)mx - s->camera_state.system_drag_world.x,

                                          (f32)my - s->camera_state.system_drag_world.y };

                Vec2 world_delta = Vec2{ screen_delta.x / s->camera_state.camera.zoom,

                                        -screen_delta.y / s->camera_state.camera.zoom };

                s->camera_state.camera.position = vec2_sub(s->camera_state.system_drag_cam, world_delta);

            }

        }

    } else if (s->travel_enabled && s->travel.active) {

        // Travel mode: flagship is on rails. Flight controls are overridden.

        if (!s->travel_paused) {

            s->profiler.begin(PROF_TRAVEL);

            travel_update(&s->travel, sim_dt);

            s->profiler.end(PROF_TRAVEL);

        }

        s->player_ship().origin    = s->travel.current;

        s->player_flight().velocity = Vec2{ 0.0f, 0.0f };

    } else if (s->view.mode == MODE_GLOBAL) {

        // Pilot the manually-controlled fleet member directly.

        FleetShip* pf = &s->fleet_state.fleet.at(piloted_idx);

        Ship* psh = &pf->ship;

        turn_commanded = control_ship_global(s, pf, sim_dt);

        // ---- Weapon firing (left click, gated on ImGui not owning the cursor) ------------

        if (!s->editor.edit_mode_active && !s->camera_state.free_camera_active && !bs_imgui_wants_mouse()) {

            // weapon slot switching: 1-4

            if (input_is_key_down(KEY_NUM1) && !input_was_key_down(KEY_NUM1)) {

                if (psh->weapon_count > 0) psh->active_weapon_idx = 0;

            }

            if (input_is_key_down(KEY_NUM2) && !input_was_key_down(KEY_NUM2)) {

                if (psh->weapon_count > 1) psh->active_weapon_idx = 1;

            }

            if (input_is_key_down(KEY_NUM3) && !input_was_key_down(KEY_NUM3)) {

                if (psh->weapon_count > 2) psh->active_weapon_idx = 2;

            }

            if (input_is_key_down(KEY_NUM4) && !input_was_key_down(KEY_NUM4)) {

                if (psh->weapon_count > 3) psh->active_weapon_idx = 3;

            }

            // left click -> fire active weapon toward mouse cursor

            if (input_is_button_down(BUTTON_LEFT) && !input_was_button_down(BUTTON_LEFT)) {

                if (psh->active_weapon_idx >= 0 && psh->active_weapon_idx < psh->weapon_count) {

                    Weapon* w = psh->weapons[psh->active_weapon_idx];

                    if (w) {

                        Vec2 mw = mouse_true_world(s);

                        (void)mw;

                        bs_math::HierPos2 mw_hp = mouse_true_hierpos(s);

                        Vec2 dir = hierpos_diff(&mw_hp, &psh->origin, BS_HIERPOS_CELL_SIZE);

                        w->fire(psh->origin, dir, pf->flight.velocity, &s->projectiles);

                    }

                }

            }

        }

    }

    // ---- Update weapons (cooldowns) -----------------------------------------------------

    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {

        Ship& sh = s->fleet_state.fleet.at(i).ship;

        for (i32 w = 0; w < sh.weapon_count; ++w)

            if (sh.weapons[w]) sh.weapons[w]->update(sim_dt);

    }

    for (i32 i = 0; i < s->fleet_state.enemy_ship.weapon_count; ++i) {

        if (s->fleet_state.enemy_ship.weapons[i]) s->fleet_state.enemy_ship.weapons[i]->update(sim_dt);

    }

    // ---- RTS controls update (orders, selection, hover) --------------------------------

    s->profiler.begin(PROF_RTS);

    s->rts_controls.update(sim_dt);

    s->profiler.end(PROF_RTS);

    if (!s->editor.edit_mode_active) {

        // AUTOPILOT: drive ordered ships toward their targets. Only skip the flagship when it is

        // actually under manual control -- that is, in the arena look with the camera following it

        // (MODE_GLOBAL && !free_camera_active). In the galaxy-map look, or with a detached free

        // camera, nothing pilots the flagship, so it must obey its move/attack order like the rest.

        b8  manually_piloting = (s->view.mode == MODE_GLOBAL) && !s->camera_state.free_camera_active;

        i32 auto_skip = manually_piloting ? piloted_idx : -1;

        s->profiler.begin(PROF_FLEET_AUTOPILOT);

        s->fleet_state.fleet.update_autopilot(s, sim_dt, auto_skip);

        s->profiler.end(PROF_FLEET_AUTOPILOT);

        // SIMULATION: integrate every fleet ship's pose. The piloted ship uses turn_commanded;

        // the rest auto-stabilize residual spin.

        s->profiler.begin(PROF_FLEET_SIM);

        s->fleet_state.fleet.simulate_all(sim_dt, turn_commanded, piloted_idx);

        s->profiler.end(PROF_FLEET_SIM);

        // Ship-ship collision response runs immediately AFTER the poses are integrated.

        s->profiler.begin(PROF_SHIP_COLLISION);

        resolve_ship_collision(s);

        s->profiler.end(PROF_SHIP_COLLISION);

        // Hardcoded demo: enemy ship orbits the player flagship.

        combat_arena_update_enemy_orbit(s, sim_dt);

    }

    // ---- Sync combat entity positions / velocities from their ships (sim/combat_arena.cpp) --
    combat_arena_sync_entities(s, sim_dt);

    // ---- Update projectiles + projectile/entity collision (sim/combat_arena.cpp) ------------
    combat_arena_update_projectiles(s, sim_dt);

    // ---- Sync world entities to galaxy map --------------------------------------------
    // Rebuild the generic map entity list every frame (sim/galaxy_map.cpp).
    galaxy_map_sync_entities(s);

    // Camera follows the piloted ship, or holds a detached free-camera / edit center -- the SAME

    // model at every zoom (arena and galaxy-map looks). The renderer draws each entity at

    // comp*(world - camera_hierpos), then camera2d subtracts camera.position; we choose ONE

    // true-world center per frame, fold it into camera_hierpos (canonicalized, keeps f32 coords

    // tiny) and keep the sub-cell residual in camera.position. This IS the floating-origin rebase.

    {

        // Recenter animation (TAB): ease the detached center onto the piloted ship, then follow.

        if (s->galaxy.map_recentering) {

            s->galaxy.map_recenter_t += dt / 0.80f; // ~0.8 second duration

            if (s->galaxy.map_recenter_t > 1.0f) s->galaxy.map_recenter_t = 1.0f;

            f32 eased = s->galaxy.map_recenter_t * s->galaxy.map_recenter_t * (3.0f - 2.0f * s->galaxy.map_recenter_t);

            HierPos2 ship = piloted_ship_origin(s);

            s->camera_state.free_camera_pos = hierpos_lerp(&s->galaxy.map_recenter_from_pos, &ship, eased,

                                              BS_HIERPOS_CELL_SIZE);

            if (s->galaxy.map_recenter_t >= 1.0f) {

                s->galaxy.map_recentering    = FALSE;

                s->camera_state.free_camera_active = FALSE; // hand control back to ship-follow

            }

        }

        HierPos2 true_center;

        if (s->camera_state.free_camera_active) {

            true_center = s->camera_state.free_camera_pos;

        } else if (s->editor.edit_mode_active) {

            // Edit mode leaves the camera fixed: reconstruct the current true center.

            true_center = game_camera_center_hierpos(s);

        } else {

            true_center = piloted_ship_origin(s);

        }

        s->camera_state.camera_hierpos  = true_center;

        s->camera_state.camera.position = Vec2{ 0.0f, 0.0f };

        s->camera_state.camera.rotation = 0.0f;

    }

    // ---- Coordinate diagnostics: dump/verify HierPos2 state now that the frame's positions

    // and the camera anchor are finalized (throttled; compiled out in release builds).

    coord_diag_update(s, dt);

    // ---- Orbital motion (always simulated, only visible in system view) --------------------
    galaxy_map_update_orbits(s, sim_dt);

    return TRUE;

}

// =====================================================================================

// Render.

// =====================================================================================

// draw_ship_visual_ex / draw_ship_visual / draw_enemy_ship_sensor / draw_engine_exhaust and
// the EXHAUST_* / COLLIDER_COLOR constants now live in render/ship_scene.cpp (the ship-rendering
// pass, draw_ship_scene, declared via render/ship_scene.h, included by game.h).

// draw_collider_outline now lives in render/ship_render.cpp
// (declared via render/ship_render.h, included by game.h).

// ---- DEBUG: HierPos2 cell-grid overlay ------------------------------------------------
// draw_hierpos_cell_grid now lives in render/debug_overlay.cpp
// (declared via render/debug_overlay.h, included by game.h).

// ---- Ship type emblem (system mode) ----------------------------------------------------
// EMBLEM_NO_GLOW, draw_ring_arc, draw_one_ship_emblem, emblem_find/union, and
// draw_fleet_emblems now live in render/ship_render.cpp (draw_fleet_emblems declared via
// render/ship_render.h, included by game.h).

// ---- Editor / debug UI panels ----------------------------------------------------------
// draw_glow_editor, build_editor_panel, build_transform_panel, draw_time_control_panel and
// build_profiler_panel now live in ui/editor_ui.cpp (declared via ui/editor_ui.h, included by
// game.h). The non-UI sensor-visibility helpers (sensor_visibility_from_dist /
// get_sensor_visibility) now live in sim/galaxy_map.cpp (declared in state/game_state.h).

// ---- Radiation detector heat map ------------------------------------------------------
// heat_map_fade_weight / draw_ship_metaballs now live in sim/heat_map.cpp
// (draw_ship_metaballs declared via sim/heat_map.h, included by game.h).

// ---- Time-control panel: top-center toggle for pause/resume (visible in ALL modes) ----
// draw_time_control_panel now lives in ui/editor_ui.cpp (declared via ui/editor_ui.h).

// game_compression_factor / view_arena_weight moved to core/view_transform.cpp (see game.h).

// PROFILER panel -- bottom-left, collapsible per-subsystem CPU timing readout (Profiler system).
// build_profiler_panel now lives in ui/editor_ui.cpp (declared via ui/editor_ui.h).

b8 game_render(Game* game_inst, f32 dt) {

    game_state* s = (game_state*)game_inst->state;

    if (!s) return TRUE;

    BS_PROFILE(&s->profiler, PROF_RENDER_TOTAL);

    s->elapsed_time += dt;

    // STEP 2: continuous arena<->map blend weight for this frame, derived purely from zoom. Draw

    // sites and background subsystems read s->view_arena_w to cross-fade instead of branching on mode.

    s->view_arena_w = view_arena_weight(s->camera_state.camera.zoom);

    // Shared parallax anchor for all celestial content this frame (the camera's current system).
    // Computed once here so the Map renderer, Arena renderer and star lighting all parallax against
    // the SAME anchor ->galaxy.systems keep their relative layout and never fuse, and no seam appears
    // across the map<->arena cross-fade.
    s->celestial_anchor = celestial_shared_anchor(s);

    // ---- World scene render passes -- extracted to render/scene_renderer.cpp (render_scene),
    // which owns the pass ORDER (galaxy-map look -> parallax background -> frame lighting ->
    // ships -> gameplay overlays). UI panels + frame timing stay below.
    render_scene(s, dt);

    s->profiler.begin(PROF_UI);

    // Action Log Panel -- bottom-right HUD. Shows last 3 messages (fades after 3s), expands to

    // full 30-entry history on hover. Logs significant player actions.

    if (!s->editor.edit_mode_active)

        build_action_log_panel(s, dt);

    // ---- Encounter panel (centered, modal) -----------------------------------------------

    draw_encounter_panel(s);

    // Editor Panel (always visible: contains the "Edit mode active" checkbox)

    build_editor_panel(s);

    // Transform panel: only in edit mode when an entity is selected

    if (s->editor.edit_mode_active)

        build_transform_panel(s);

    // Time-control panel (visible in ALL modes, even edit mode)

    draw_time_control_panel(s);

    // Per-subsystem CPU profiler panel (collapsible)

    build_profiler_panel(s);

    // ---- Navigation HUD + Ship HUD (arena-side affordance, hidden in edit mode) -------------

    // Shown on the arena side of the blend band; suppressed on the galaxy-map side to avoid

    // cluttering the map. Uses the arena weight midpoint so it fades in with the arena look.

    draw_nav_ship_hud(s);

    // Periodic stats to the log (the only on-screen text is the diagnostic helm-status HUD above).

    {

        // static f32 acc = 0.0f;

    }

    s->profiler.end(PROF_UI);

    f64 frame_end_ns = (f64)std::chrono::steady_clock::now().time_since_epoch().count();

    f32 frame_dt_ms = (f32)(frame_end_ns - s->perf_frame_start_ns) / 1e6f;

    s->frame_ms = s->frame_ms * 0.9f + frame_dt_ms * 0.1f;

    return TRUE;

}

void game_on_resize(Game* game_inst, u32 width, u32 height) {

    game_state* s = (game_state*)game_inst->state;

    if (s) {

        s->fb_width  = (u16)width;

        s->fb_height = (u16)height;

    }

    BS_LOG_DEBUG("game_on_resize: %u x %u", width, height);

}

