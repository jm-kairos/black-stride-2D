#include "game.h"

// Aggregate include of every extracted module header. game.cpp is the frame orchestrator that
// calls into all modules, so it (and only it) pulls the whole cascade.
#include "game_modules.h"

#include "render/text.h"

#include "sim/travel.h"

#include "core/coord_diag.h"

#include "sim/weapon.h"

#include "sim/projectile.h"

#include "sim/point_defense.h"
#include "sim/flak_screen.h"

#include "render/projectile_fx.h" // projectile_fx_render_init (bakes the VFX textures)

#include <core/event.h>  // event_fire(EVENT_CODE_APPLICATION_QUIT): game-side ESC-quit policy

#include "sim/ss_generation.h"

#include "render/voronoi_cell_hover_effect.h"

#include "render/global_background.h"

#include "render/mapped_system_layer.h"

#include <core/logger.h>

#include <core/input.h>

#include <math/math_utils.h>

#include <math/bs_hierpos.h>

#include <renderer/renderer.h>

#include <renderer/camera2d.h>

#include <renderer/bs_imgui.h> // bs_imgui_wants_mouse: gate world input while ImGui owns the cursor
#include <renderer/bs_rml.h>   // bs_rml_*: in-game UI (RmlUi) documents + input gating
#include "render/galaxy_map_render.h" // galaxy_pick_planet: hit-test a planet under the cursor
#include "render/weapon_hub.h"      // weapon_hub_update/_close: middle-mouse micro-selection hub
#include "sim/celestial_parallax.h" // celestial_parallax_fade: keep a followed planet screen-centered

#include <renderer/bs_ui.h>

#include <math.h>   // powf

#include <stdio.h>  // snprintf, vsnprintf

#include <string.h> // strcmp, memset (RmlUi HUD snapshot + action polling)

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

// Action Log data helper (action_log_push) lives in sim/action_log.cpp (declared via
// sim/action_log.h, included by game.h). The rolling log is rendered by the RmlUi HUD
// (game_push_hud below), which replaced the old bs_ui build_action_log_panel.

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

    // Bloom defaults. ON by default: weapon fire draws on LAYER_PROJECTILE / LAYER_PROJECTILE_FX,
    // which sit below BS_LAYER_BLOOM_THRESHOLD precisely so muzzle flashes, tracers and impacts
    // go through the bloom pass. Leaving this FALSE made that layer choice inert -- every hit
    // composited after the pass and read as a flat coloured decal.

    s->render.bloom_enabled    = TRUE;

    s->render.bloom_threshold  = 1.2f;

    s->render.bloom_intensity  = 0.3f;

    s->render.dynamic_bloom    = TRUE;

    s->render.star_light_enabled     = TRUE;

    s->render.bg_layer0_enabled      = FALSE;   // far starfield hidden by default

    s->render.bg_layer1_enabled      = FALSE;   // mid starfield hidden by default

    s->render.bg_layer2_enabled      = TRUE;

    s->render.bg_nebula_enabled      = FALSE;   // nebula/dust cloud hidden by default

    s->render.celestial_draw_planets     = TRUE;  // planets + orbit rings

    s->render.celestial_draw_testsprites = FALSE; // volumetric-light demo dots (dev-only)

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

    // ---- Per-family projectile glow -------------------------------------------------------
    // All three start from the global defaults, then differ only where the family's PHYSICS
    // differs. The shader ramps temp_cool -> temp_warm -> temp_hot from a sprite's tail to its
    // head whenever custom.x > 0, which is the streak and the missile plume; glow_tint is the
    // additive halo every projectile sprite picks up; distort_amp is the heat shimmer.

    for (i32 i = 0; i < 3; ++i) s->render.projectile_glow[i] = s->render.glow_params;

    // SHELL -- an inert slug of hot metal. This is the established look and stays the baseline
    // the other two are read against; changing it would re-tune every existing tracer.
    s->render.projectile_glow[VFX_SHELL].glow_tint = bs_color{ 1.00f, 0.85f, 0.50f, 1.0f };
    s->render.projectile_glow[VFX_SHELL].temp_cool = bs_color{ 0.90f, 0.15f, 0.02f, 1.0f };
    s->render.projectile_glow[VFX_SHELL].temp_warm = bs_color{ 1.00f, 0.45f, 0.05f, 1.0f };
    s->render.projectile_glow[VFX_SHELL].temp_hot  = bs_color{ 1.00f, 0.98f, 0.90f, 1.0f };

    // SLUG -- rail-driven, so the light is electrical rather than thermal: cold blue through
    // cyan to a blue-white head. Distortion is nearly off because there is no combustion gas to
    // shimmer -- a railgun's flash is ionisation, not burning propellant. Brighter core and
    // stronger head bloom because the discharge, not the projectile, is the event.
    s->render.projectile_glow[VFX_SLUG].glow_tint   = bs_color{ 0.55f, 0.80f, 1.00f, 1.0f };
    s->render.projectile_glow[VFX_SLUG].temp_cool   = bs_color{ 0.10f, 0.25f, 0.85f, 1.0f };
    s->render.projectile_glow[VFX_SLUG].temp_warm   = bs_color{ 0.35f, 0.70f, 1.00f, 1.0f };
    s->render.projectile_glow[VFX_SLUG].temp_hot    = bs_color{ 0.92f, 0.98f, 1.00f, 1.0f };
    s->render.projectile_glow[VFX_SLUG].intensity   = 1.25f;
    s->render.projectile_glow[VFX_SLUG].head_mult   = 5.0f;
    s->render.projectile_glow[VFX_SLUG].distort_amp = 0.02f;

    // ORDNANCE -- a chemical motor actually burning. Yellow-white at the nozzle into a dark
    // smoky red aft, and the ONE family where the shader's heat distortion is literally correct
    // rather than decorative, so it runs at roughly double the default amplitude.
    s->render.projectile_glow[VFX_ORDNANCE].glow_tint    = bs_color{ 1.00f, 0.62f, 0.28f, 1.0f };
    s->render.projectile_glow[VFX_ORDNANCE].temp_cool    = bs_color{ 0.55f, 0.10f, 0.02f, 1.0f };
    s->render.projectile_glow[VFX_ORDNANCE].temp_warm    = bs_color{ 1.00f, 0.55f, 0.10f, 1.0f };
    s->render.projectile_glow[VFX_ORDNANCE].temp_hot     = bs_color{ 1.00f, 0.95f, 0.75f, 1.0f };
    s->render.projectile_glow[VFX_ORDNANCE].distort_amp  = 0.15f;
    s->render.projectile_glow[VFX_ORDNANCE].wave_speed   = 22.0f;
    s->render.projectile_glow[VFX_ORDNANCE].jitter_speed = 60.0f;

    s->editor.edit_mode_active = FALSE;

    // The player commands the WHOLE WING by default. This was FALSE -- one hull, escorts spawned
    // but hidden behind the editor checkbox -- which made the fleet a debug feature. Everything
    // the RTS layer exists for (selection, formations, standing orders) is inert with one ship,
    // and box selection was retired precisely because every drag resolved to "the ship" or
    // "nothing". The checkbox still works and still collapses the wing back to the flagship.
    s->editor.multi_ship_enabled = TRUE;

    s->editor.draw_discovery_sensor_range = FALSE;

    s->editor.edit_selection   = EditSelection{ EDIT_NONE, -1 };

    s->editor.edit_drag        = EditorDrag{ FALSE, EDIT_DRAG_NONE, bs_math::HierPos2{}, bs_math::HierPos2{}, 0.0f };

    s->action_log.count = 0;

    s->action_log.inactivity_timer = 0.0f;

    s->show_discoveries = false;

    s->show_flagship_inspector = false;
    s->inspected_ship_idx = 0;
    s->insp_tab = 0;
    s->insp_view_zoom = 1.0f;       // portrait view at the fit framing
    s->insp_view_pan = Vec2{ 0.0f, 0.0f };
    s->insp_view_dragging = false;
    s->hovered_station_id = -1;
    s->station_menu_visible = false;
    s->station_menu_station_id = -1;
    s->station_menu_x = 0;
    s->station_menu_y = 0;
    s->ship_menu_visible = false;
    s->ship_menu_member = -1;
    s->ship_menu_x = 0;
    s->ship_menu_y = 0;
    s->show_station_inspector = false;
    s->inspect_station_id = -1;
    s->station_insp_tab = 0;
    s->show_planet_inspector = false;
    s->planet_insp_center = bs_math::HierPos2{};
    s->planet_insp_planet = -1;
    s->pending_weapon_drag = -1;    // Arsenal drag-drop: no source armed
    s->pending_weapon_drag_kind = 0;
    s->world_module_drag = FALSE;   // no ship-side loadout drag in flight
    s->weapon_hub_open = FALSE;     // weapon micro-selection hub closed
    s->weapon_hub_hover = -1;
    s->weapon_hub_open_time = 0.0f;
    s->weapon_hub_press_px = bs_math::Vec2{ 0.0f, 0.0f };
    s->weapon_hub_target = bs_math::HierPos2{};
    s->ui_font_kit = 3;             // default to the "Frontier" kit (Barlow Condensed / Barlow / B612 Mono)
    s->ui_sharpen  = 0.5f;          // UI atlas unsharp-mask amount (editor-tunable)

    // ---- Missile flight model (Phase A; see docs/POINT_DEFENSE_AND_MISSILES.md) -----------
    s->missile_tuning.turn_rate       = 1.6f;      // ~90 deg/s: corvettes can out-turn it
    s->missile_tuning.accel           = 2500.0f;
    s->missile_tuning.max_speed       = 9000.0f;   // < cannon shell speed (12000)
    s->missile_tuning.seeker_cone_deg = 60.0f;
    s->missile_tuning.seeker_range    = 12000.0f;

    // ---- Flak burst model (Phase D) --------------------------------------------------------
    s->flak_tuning.fuse_radius  = 90.0f;
    s->flak_tuning.burst_radius = 240.0f;
    s->flak_tuning.burst_damage = 4.0f;    // one-shots a 3.5hp missile at burst center

    // ---- Ship module registry (immutable defs; ships mount entries by pointer) -----------

    // ---- Ship def registry (hull stat cards): every hull the game can spawn is one card --
    // Loaded FIRST: the fleet/enemy instantiations below and ai_ships_init both draw on it.

    ship_registry_load(&s->ship_registry, "assets/ships/ships.list");

    module_registry_load(&s->module_registry, "assets/modules/modules.list");

    // ---- Weapon def registry (stat system S2): immutable stat blocks from assets/weapons --

    weapon_registry_load(&s->weapon_registry, "assets/weapons/weapons.list");

    s->view.alt_movement_active = FALSE;

    // ---- Fleet: flagship (member 0) loaded from the player hull --------------------------

    s->fleet_state.fleet.init();

    {

        FleetShip& fs = s->fleet_state.fleet.add();

        // The Vanguard is by DESIGN a late-game, high-tier hull; the starting fleet flies it
        // only until a dedicated starter hull card exists (it is the one hull with authored
        // art + hardpoints). Swap this id when that card lands.
        if (!ship_instantiate(ship_registry_find(&s->ship_registry, "vanguard_cruiser"), &fs.ship)) {

            BS_LOG_FATAL("game_init: failed to instantiate player ship 'vanguard_cruiser'.");

            return FALSE;

        }

        // Hull cards may author a shipped temperament (roe / flak / missiles / cap_floor).
        fs.apply_card_doctrine();

        fs.ship_type = SHIP_TYPE_DRONE;

        fs.ship.origin = hierpos_from_vec2(Vec2{ 25000.0f, 0.0f }, BS_HIERPOS_CELL_SIZE);

        fs.ship.faction_id = FACTION_PLAYER;

        // Named at spawn like the escorts below: the roster, the stance action-log lines and
        // the fleet readout all print vessel_name, and every one of them showed an empty
        // flagship before this was set. vessel_name is the ship's PROPER NAME (its callsign),
        // not its role -- "flagship"/"escort" are states the fleet model already tracks, and
        // the class name lives on the card (ship->def->name).
        fs.ship.vessel_name = "Iron Meridian";

        fs.ship.glow   = s->render.glow_params;

        fs.ship.radiation_emission = 0.05f;

        // The flagship's weapons start UNMOUNTED in the loadout stash; the player mounts them
        // onto hardpoints from the Arsenal inspector (ship_instantiate cleared all hardpoint mounts).
        // A deliberately diverse starting rack -- baseline gauss, rapid autocannon (flak
        // platform), salvo artillery, and a missile rack -- so the stat cards read differently
        // from minute one. The stash is the FLEET-WIDE pool: the escorts' cannons join it at
        // their spawn below (escorts start unarmed), so SHIP_MAX_WEAPONS covers 4 + 4.
        fs.ship.weapon_stash[0]     = weapon_instantiate(weapon_registry_find(&s->weapon_registry, "gauss_mk1"),      fs.ship.faction);

        fs.ship.weapon_stash[1]     = weapon_instantiate(weapon_registry_find(&s->weapon_registry, "trident_mk1"),    fs.ship.faction);

        fs.ship.weapon_stash[2]     = weapon_instantiate(weapon_registry_find(&s->weapon_registry, "autocannon_mk1"), fs.ship.faction);

        // Guided-missile launcher (Phase A): starts stashed like the cannons; mount and
        // fire-group it from the Arsenal inspector.
        fs.ship.weapon_stash[3]     = weapon_instantiate(weapon_registry_find(&s->weapon_registry, "harpoon_rack"),   fs.ship.faction);

        fs.ship.weapon_stash_count  = 4;

        // The fleet owns ONE point-defense device, stocked in the fleet-wide pool like the
        // cannons above (mount it onto any hull's defense hardpoint from the inspector).
        // The explicit disable below is redundant with DefenseLaser's default (belt and
        // braces at the one spawn site).
        s->fleet_pd_stock = 1;

        fs.ship.point_defense.enabled = FALSE;

        fs.ship.point_defense_mount   = -1;

        // Ship modules (Phase 4): the module registry was loaded above; the flagship starts with
        // its sensor arrays UNMOUNTED in the module rack, like the cannon and point-defense. Sensors
        // run on the hull baseline until a module is installed on a sensor hardpoint.
        {
            const ModuleDef* mods[2] = {
                module_registry_find(&s->module_registry, "surveyor_mk1"),
                module_registry_find(&s->module_registry, "surveyor_mk2"),
            };
            for (i32 m = 0; m < 2; ++m)
                if (mods[m] && fs.ship.module_stash_count < SHIP_MAX_MODULES)
                    fs.ship.module_stash[fs.ship.module_stash_count++] = mods[m];
        }

    }

    // ---- Fleet: demo escort ships (members 1..N) -----------------------------------------

    {

        const VesselFaction player_faction = s->fleet_state.fleet.flagship().ship.faction;

        // Proper names, not roles ("Escort" is a state): one per hull, in spawn order. The
        // pointers land in vessel_name, so the literals' static storage is what keeps them alive.
        const char* escort_names[4] = { "Vesper", "Halcyon", "Cinderwake", "Duskrunner" };

        // Cruiser hulls are ~1586 units long (~817-unit bounding radius), so keep the demo
        // formation spread wide enough that no hulls overlap at spawn.
        const Vec2 escort_offsets[4] = {

            Vec2{ 24000.0f,  2500.0f },

            Vec2{ 24000.0f, -2500.0f },

            Vec2{ 21000.0f,  2500.0f },

            Vec2{ 21000.0f, -2500.0f },

        };

        for (i32 i = 0; i < 4; ++i) {

            FleetShip& fs = s->fleet_state.fleet.add();

            if (!ship_instantiate(ship_registry_find(&s->ship_registry, "vanguard_cruiser"), &fs.ship)) {

                BS_LOG_ERROR("game_init: failed to instantiate escort ship %d.", i);

                continue;

            }

            fs.apply_card_doctrine();   // card-authored temperament, if the hull ships one

            fs.ship_type = SHIP_TYPE_DRONE;

            fs.ship.origin      = hierpos_from_vec2(escort_offsets[i], BS_HIERPOS_CELL_SIZE);

            fs.ship.faction     = player_faction;

            fs.ship.faction_id  = FACTION_PLAYER;

            fs.ship.vessel_name = escort_names[i];

            fs.ship.glow        = s->render.glow_params;

            fs.ship.radiation_emission = 0.05f;

            // Escorts spawn UNARMED: their cannon goes into the FLEET-WIDE pool (the
            // flagship's stash) instead of auto-mounting, so arming ANY hull is the player's
            // act in the Arsenal inspector -- the same philosophy the flagship follows. Until
            // armed, an escort contributes stances and screening geometry but no fire.
            Ship& pool = s->fleet_state.fleet.flagship().ship;

            if (pool.weapon_stash_count < SHIP_MAX_WEAPONS)

                pool.weapon_stash[pool.weapon_stash_count++] =
                    weapon_instantiate(weapon_registry_find(&s->weapon_registry, "gauss_mk1"), pool.faction);

        }

    }

    if (!ship_instantiate(ship_registry_find(&s->ship_registry, "raider"), &s->fleet_state.enemy_ship)) {

        BS_LOG_FATAL("game_init: failed to instantiate enemy ship 'raider'.");

        return FALSE;

    }

    s->fleet_state.enemy_ship.origin     = hierpos_from_vec2(Vec2{ 85000.0f, 25000.0f }, BS_HIERPOS_CELL_SIZE);

    s->fleet_state.enemy_ship.angle      = 2.36f;

    s->fleet_state.enemy_orbit_phase     = 0.0f;

    s->fleet_state.enemy_ship.faction    = VESSEL_PIRATE;

    s->fleet_state.enemy_ship.faction_id = FACTION_PIRATE;

    s->fleet_state.enemy_ship.vessel_name = "Raider-class Interceptor";

    s->fleet_state.enemy_ship.glow = s->render.glow_params;

    s->fleet_state.enemy_ship.radiation_emission = 1.0f;

    // ---- Enemy weapon inventory ----------------------------------------------------------

    // Auto-mount the enemy cannon on its first weapon hardpoint (legacy hulls get a synthetic
    // center mount from the card load, so this always succeeds).
    i32 enemy_hp = ship_first_free_hardpoint(&s->fleet_state.enemy_ship, MODULE_TYPE_WEAPON);

    if (enemy_hp < 0) enemy_hp = 0;

    s->fleet_state.enemy_ship.mounts[enemy_hp] = weapon_instantiate(weapon_registry_find(&s->weapon_registry, "gauss_mk1"), s->fleet_state.enemy_ship.faction);

    // Extend the enemy's effective firing range ~10x by lengthening its projectile lifetime

    // (reach = speed * lifetime). Lets it land shots for long-range sensor-detection testing.

    if (BallisticWeapon* enemy_cannon = static_cast<BallisticWeapon*>(s->fleet_state.enemy_ship.mounts[enemy_hp])) {

        enemy_cannon->projectile_lifetime *= 10.0f;

        // Slow the cadence: long-lived projectiles (rate * lifetime) would otherwise saturate the

        // 256-slot pool and firing would stall. 0.5/s * 200s = ~100 live shots, well under the cap.

        enemy_cannon->fire_rate         = 0.5f;

        enemy_cannon->cooldown_duration = 1.0f / enemy_cannon->fire_rate;

    }

    s->fleet_state.enemy_ship.active_weapon_idx = enemy_hp;

    // Phase A: mount a guided-missile launcher on the enemy's next free weapon hardpoint so
    // the player faces seekers immediately (fired independently of the bearing-weapon pick in
    // combat_arena_update_enemy). Skipped silently if the hull has no second weapon slot.
    i32 enemy_ml_hp = ship_first_free_hardpoint(&s->fleet_state.enemy_ship, MODULE_TYPE_WEAPON);

    if (enemy_ml_hp >= 0)
        s->fleet_state.enemy_ship.mounts[enemy_ml_hp] =
            weapon_instantiate(weapon_registry_find(&s->weapon_registry, "harpoon_rack"),
                               s->fleet_state.enemy_ship.faction);

    // The escorts spawned above stay ACTIVE unless the editor checkbox collapses the wing. No
    // rebuild is needed on this path: combat_arena_init below registers the whole active fleet.
    // (The checkbox's own handler must still call combat_arena_rebuild_player_entities itself,
    // because it changes the count after the entity windows are packed.)
    if (!s->editor.multi_ship_enabled) s->fleet_state.fleet.set_count(1);

    // ---- Combat arena: projectile pool + combat entities + sensor/heat/encounter tunables --
    // (projectiles, combat_entities registration, out_sensor_fx, heat-signature params and the
    //  encounter state now live in sim/combat_arena.cpp -> combat_arena_init.)
    combat_arena_init(s);

    // Resolve the hull cards' textures BEFORE ai_ships_init: its templates (and every NpcShip
    // struct-copied from them) carry the def's handles, so the defs must be live first. This is
    // the same point the retired per-template resolve calls ran at, so the renderer is up here;
    // the fleet/enemy instances above still get their own per-instance resolve pass later.
    ship_registry_resolve_textures(&s->ship_registry);

    // ---- General Ship AI: load the NPC hull template and reserve the NPC combat-entity window --
    // The NPC window begins right after the persistent fleet + enemy slots registered above.
    ai_ships_init(s);
    s->npc_combat_base = s->combat_entity_count;

    // The flagship starts SELECTED so the first RMB in the (default) RTS view orders something.
    // Selection is otherwise free: clicking empty space deselects everything, and nothing
    // re-asserts a selection behind the player's back (the old overlay-down flagship fallback
    // is gone with the overlay itself).
    s->fleet_state.fleet.set_selected(0, TRUE);

    // Camera starts zoomed out (global mode), centered on the flagship.

    s->camera_state.camera          = camera2d_default();

    s->camera_state.camera.zoom     = ZOOM_START;

    s->camera_state.target_zoom     = ZOOM_START;

    s->camera_state.zoom_smooth_rate = 16.0f;

    s->planet_approach = {};

    s->camera_state.camera.position = hierpos_to_vec2(&s->player_ship().origin, BS_HIERPOS_CELL_SIZE);

    s->view.mode            = MODE_GLOBAL;

    // AUTO-PILOT / RTS IS THE DEFAULT CONTROL MODE: the game opens with the camera DETACHED,
    // parked over the fleet, and every hull obeying its orders from frame one. Piloting is
    // entered PER-SHIP from the right-click context menu ("Pilot") -- the old TAB / button
    // mode toggle is retired. piloted_idx stays 0 (flagship) so the fleet panel, the cursor
    // turrets and player_gunnery keep a well-defined subject before the first takeover.
    s->camera_state.free_camera_active = TRUE;
    s->camera_state.free_camera_pos    = s->player_ship().origin;

    // Camera->ship recenter glide (context-menu Pilot / galaxy on-screen re-entry).
    s->camera_state.recentering       = FALSE;
    s->camera_state.recenter_t        = 0.0f;
    s->camera_state.recenter_from_pos = {};

    // Floating-origin camera starts at the home system, which the galaxy generator pins to the
    // origin (node 0 == "Sol"). Set before galaxy_map_init so its initial cache materialisation
    // (which focuses on camera_hierpos) populates the home neighbourhood.

    s->camera_state.camera_hierpos = bs_math::HierPos2{ bs_math::GridCell{ 0, 0 }, bs_math::Vec2{ 0.0f, 0.0f } };

    // Drag anchors zeroed so system-view middle-mouse panning starts clean.

    s->camera_state.system_drag_cam    = Vec2{ 0.0f, 0.0f };

    s->camera_state.system_drag_world  = Vec2{ 0.0f, 0.0f };

    s->camera_state.system_zoom        = ZOOM_SYSTEM;

    // ---- New Game (Phase A): DEFER galaxy generation until after the setup screen --------
    // game_init only prepares the setup screen; the galaxy + history are generated by the staged
    // APP_GENERATING pipeline (game_update) once the player clicks "Generate". Nothing below this
    // depends on the galaxy existing, and game_update/game_render early-return until APP_PLAYING.
    s->app_phase    = APP_SETUP;
    s->gen_stage    = 0;
    s->gen_progress = 0.0f;
    s->gen_label    = "";
    s->setup.seed                = 0x9E3779B97F4A7C15ull;
    s->setup.galaxy_size         = 10000;
    s->setup.galaxy_shape        = 0;   // spiral
    s->setup.history_depth_years = 1000000;
    s->setup.chronicle_detail    = 1;   // balanced
    s->setup.abundance           = 1;   // sparse
    s->setup.civ_density         = 1;   // moderate (a few dozen)
    s->setup.conflict            = 1;   // balanced
    s->setup.ambition            = 1;   // steady
    s->setup.cataclysm           = 1;   // rare
    s->setup.starting_era        = 0;

    // (Sensor range, heat-signature params and encounter state are set by combat_arena_init above.)
    s->time_scale             = 1.0f;


    s->elapsed_time           = 0.0f;

    s->sim_hours              = 0.0;

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

    // Planetary-evolution pipeline health check: per-phase stats + invariants via BS_LOG_INFO.

    system_evolution_selftest();

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

    // Generate a DIRECTIONAL teardrop plume for engine jets: hot wide head at v=1 -- the

    // sprite batcher maps v=1 to the quad's -Y edge, where origin {0.5,1} pivots, so a jet

    // anchored at a nozzle stretches with throttle like a flame instead of scaling a blob.

    // Tapers to a narrow faint tail at v=0. White; tint supplies the colour, and the shader's

    // custom.x temperature ramp (white-hot head -> red tail) keys off the same v axis.

    {

        const u32 PL_W = 64;

        const u32 PL_H = 128;

        static u8 pl_pixels[PL_W * PL_H * 4];   // static, not stack: text.cpp/star_fx.cpp pattern

        for (u32 y = 0; y < PL_H; ++y) {

            f32 v = ((f32)y + 0.5f) / (f32)PL_H;            // 0 = tail ... 1 = head (nozzle)

            f32 hw = 0.5f * (f32)PL_W * (0.16f + 0.84f * powf(v, 0.65f));

            for (u32 x = 0; x < PL_W; ++x) {

                f32 dx = ((f32)x + 0.5f - (f32)PL_W * 0.5f) / hw;

                f32 lat = 1.0f - dx * dx;

                if (lat < 0.0f) lat = 0.0f;

                f32 alpha = lat * lat * powf(v, 1.35f);

                u32 i = (y * PL_W + x) * 4;

                pl_pixels[i + 0] = 255;

                pl_pixels[i + 1] = 255;

                pl_pixels[i + 2] = 255;

                pl_pixels[i + 3] = (u8)(alpha * 255.0f);

            }

        }

        s->render.exhaust_plume_texture = renderer_create_texture(pl_pixels, PL_W, PL_H);

    }

    // Generate the MACH-DIAMOND core column for main drives: a narrow jet whose boundary

    // necks periodically and whose brightness peaks at each neck -- the shock-diamond train

    // of a real underexpanded rocket exhaust. Five nodes marching aft from the head (v=1),

    // fading toward the tail. Drawn as a thin extra layer inside the plume core at high

    // throttle only, so a cruising flame stays soft and a full burn reads supersonic.

    {

        const u32 CO_W = 32;

        const u32 CO_H = 256;

        const f32 CO_NODES = 5.0f;

        static u8 co_pixels[CO_W * CO_H * 4];

        for (u32 y = 0; y < CO_H; ++y) {

            f32 v = ((f32)y + 0.5f) / (f32)CO_H;            // 0 = tail ... 1 = head

            f32 sphase = CO_NODES * (1.0f - v);             // node phase, 0 at the head

            f32 neck   = 0.74f + 0.26f * fabsf(sinf(BS_PI * sphase));

            f32 nodec  = cosf(BS_PI * sphase);

            f32 bright = nodec * nodec; bright *= bright; bright *= bright;  // cos^8: sharp nodes

            f32 hw = 0.5f * (f32)CO_W * (0.30f + 0.50f * v) * neck;

            for (u32 x = 0; x < CO_W; ++x) {

                f32 dx = ((f32)x + 0.5f - (f32)CO_W * 0.5f) / hw;

                f32 lat = 1.0f - dx * dx;

                if (lat < 0.0f) lat = 0.0f;

                f32 alpha = lat * lat * powf(v, 1.3f) * (0.30f + 0.70f * bright);

                u32 i = (y * CO_W + x) * 4;

                co_pixels[i + 0] = 255;

                co_pixels[i + 1] = 255;

                co_pixels[i + 2] = 255;

                co_pixels[i + 3] = (u8)(alpha * 255.0f);

            }

        }

        s->render.exhaust_core_texture = renderer_create_texture(co_pixels, CO_W, CO_H);

    }

    // ---- Ship type emblems for system-mode battlefield -----------------------------------

    s->render.emblem_drone     = renderer_load_texture("assets/fleet/emblems/fleet_emblem_type_drone.png");

    s->render.emblem_extractor = renderer_load_texture("assets/fleet/emblems/fleet_emblem_type_extractor.png");

    if (!s->render.emblem_drone.id)     BS_LOG_WARN("game_init: failed to load drone emblem texture.");

    if (!s->render.emblem_extractor.id) BS_LOG_WARN("game_init: failed to load extractor emblem texture.");

    // ---- Weapon mount art -----------------------------------------------------------------

    // weapon_registry_load ran before the renderer existed, so it only recorded path strings;
    // this is the mandatory second phase that turns them into handles (cf. ship_visual).

    weapon_registry_resolve_textures(&s->weapon_registry);

    // Bake the projectile-VFX textures (flare / ring / spark). Same mandatory-second-phase
    // shape as the two resolve calls above: it needs a live renderer, so it cannot run from
    // combat_arena_init where the FX ring itself is wired.

    projectile_fx_render_init();

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

    // ---- In-game UI (RmlUi): register the font faces in assets/ui/fonts, then create the
    // HUD data model and load + show the HUD document. The HUD (nav / ship / encounter /
    // action-log / discoveries) is driven each frame from game_update via game_push_hud().
    bs_rml_load_fonts("assets/ui/fonts");
    if (!bs_rml_hud_init("assets/ui/hud.rml"))
        BS_LOG_WARN("game_init: RmlUi HUD failed to initialize.");
    bs_rml_set_sharpen(s->ui_sharpen);   // apply the game default (editor slider tunes it live)

    return TRUE;

}

// =====================================================================================

// Update.

// =====================================================================================

// view_arena_weight and the
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
// The action log is rendered by the RmlUi HUD (game_push_hud); the old bs_ui panel is retired.
// =====================================================================================

// ---- Floating-origin re-centering (big_space-style) -----------------------------------------

// update_enemy_orbit now lives in sim/combat_arena.cpp (combat_arena_update_enemy_orbit).
// The per-frame floating-origin camera rebase happens inline in game_update (the block that
// folds the chosen true-world center into camera_hierpos).

// Phase A: the staged New Game generation pipeline. One heavy step per frame (so the progress bar
// advances between them); when the last stage completes we enter APP_PLAYING. Deterministic.
static void run_generation_stage(game_state* s) {
    const i32 STAGE_COUNT = 5;
    switch (s->gen_stage) {
        case 0: s->gen_label = "Placing star systems";  galaxy_map_worldgen(s);       s->gen_stage++; break;
        case 1: s->gen_label = "Surveying habitability";                              s->gen_stage++; break;
        case 2: s->gen_label = "Seeding civilizations"; galaxy_history_sim_begin(s);   s->gen_stage++; break;
        case 3: {   // multi-frame: simulate deep-time history in budgeted chunks (progress by year)
            s->gen_label = "Simulating history";
            b8 done = galaxy_history_sim_step(s, 250);
            if (done) { galaxy_history_finalize_view(s); galaxy_history_log_summary(s); galaxy_history_seed_garrison(s); ship_missions_seed(s); s->gen_stage++; }
            break;
        }
        case 4: s->gen_label = "Finalizing";            galaxy_map_finalize(s);         s->gen_stage++; break;
        default:                                                                      s->gen_stage++; break;
    }
    f32 base = (f32)s->gen_stage / (f32)STAGE_COUNT;
    if (s->gen_stage == 3) base = (3.0f + galaxy_history_sim_progress(s)) / (f32)STAGE_COUNT;
    s->gen_progress = base;
    if (s->gen_stage >= STAGE_COUNT) { s->app_phase = APP_PLAYING; s->gen_progress = 1.0f; s->gen_label = "Ready"; }
}

// ---- Flagship loadout stash helpers ------------------------------------------------------
// The Arsenal inspector moves weapon pointers between the ship's unmounted stash (weapon_stash[])
// and its hardpoints (mounts[]). These small helpers keep the stash compact and the active-weapon
// bookkeeping consistent after every move.
static void ship_stash_append(Ship& sh, Weapon* w) {
    if (!w) return;
    if (sh.weapon_stash_count < SHIP_MAX_WEAPONS)
        sh.weapon_stash[sh.weapon_stash_count++] = w;
}

static void ship_stash_remove_at(Ship& sh, i32 k) {
    if (k < 0 || k >= sh.weapon_stash_count) return;
    for (i32 i = k; i < sh.weapon_stash_count - 1; ++i)
        sh.weapon_stash[i] = sh.weapon_stash[i + 1];
    sh.weapon_stash[--sh.weapon_stash_count] = nullptr;
}

static void ship_rehome_weapons(Ship& sh) {
    // Keep the active index pointing at a hardpoint with a live mounted weapon (or -1 when
    // no weapons are mounted anywhere).
    if (sh.active_weapon_idx < 0 || sh.active_weapon_idx >= sh.hardpoint_count ||
        !sh.mounts[sh.active_weapon_idx]) {
        sh.active_weapon_idx = -1;
        for (i32 k = 0; k < sh.hardpoint_count; ++k) if (sh.mounts[k]) { sh.active_weapon_idx = k; break; }
    }
    // Weapon-group masks follow the same invariant (no mask on empty slots, default group 1
    // on unassigned weapons); rehome runs after every loadout mutation, so enforce it here.
    ship_groups_sanitize(&sh);
}

// Module-rack twins of the weapon-stash helpers: modules move between module_stash[] (the
// left "Ship modules" list) and module_mounts[] (hardpoints). Every mount/unmount is
// followed by ship_recompute_stats at the call site.
static void ship_module_stash_append(Ship& sh, const ModuleDef* m) {
    if (!m) return;
    if (sh.module_stash_count < SHIP_MAX_MODULES)
        sh.module_stash[sh.module_stash_count++] = m;
}

static void ship_module_stash_remove_at(Ship& sh, i32 k) {
    if (k < 0 || k >= sh.module_stash_count) return;
    for (i32 i = k; i < sh.module_stash_count - 1; ++i)
        sh.module_stash[i] = sh.module_stash[i + 1];
    sh.module_stash[--sh.module_stash_count] = nullptr;
}

// Evict whatever module occupies hardpoint dst back into the module rack. TRUE when the
// slot ends up module-free (also when it already was); FALSE when the rack is full.
static b8 ship_evict_module(Ship& sh, i32 dst) {
    if (dst < 0 || dst >= sh.hardpoint_count || !sh.module_mounts[dst]) return TRUE;
    if (sh.module_stash_count >= SHIP_MAX_MODULES) return FALSE;
    sh.module_stash[sh.module_stash_count++] = sh.module_mounts[dst];
    sh.module_mounts[dst] = nullptr;
    ship_recompute_stats(&sh);
    return TRUE;
}

// Short label for a hardpoint's accepts-mask, used by Arsenal slot tooltips.
static const char* hardpoint_kind_label(u32 accepts) {
    if ((accepts & MODULE_TYPE_WEAPON) && (accepts & MODULE_TYPE_DEFENSE)) return "weapon/defense";
    if (accepts & MODULE_TYPE_WEAPON)  return "weapon";
    if (accepts & MODULE_TYPE_DEFENSE) return "defense";
    if (accepts & MODULE_TYPE_SENSOR)  return "sensor";
    if (accepts & MODULE_TYPE_ENGINE)  return "engine";
    return "utility";
}

// Short display label for a hardpoint: its id up to the first '_' ("bow_gun" -> "bow").
// Shared by the fire-group matrix column headers and the fleet panel's group rows.
static void hardpoint_short_id(const HardpointDef& hp, char* out, i32 cap) {
    i32 c = 0;
    for (; hp.id[c] && hp.id[c] != '_' && c < cap - 1; ++c) out[c] = hp.id[c];
    out[c] = '\0';
}

// Resolve the armed arsenal drag (pending_weapon_drag / _kind) dropping onto INSPECTED-ship
// hardpoint dst. Shared by the inspector's slot strip ("slot:M" dragdrop) and world-side
// drops straight onto the hull's hardpoint boxes ("dragend" released over the ship).
// TYPE-GATED by the hardpoint's accepts-mask (weapons need a weapon slot, the point-defense
// a defense slot, modules must fit type AND size). A displaced occupant returns to its own
// inventory -- and every stash/rack here is the FLEET-WIDE POOL stored on the flagship, so
// refitting an escort draws from and returns to the same inventory as refitting the flagship.
// Per-ship point defense is the exception: each hull owns its own device. Always disarms the
// pending drag.
static void arsenal_drop_on_slot(game_state* s, i32 dst) {
    Ship& fs   = s->inspected_ship();   // the hull being refitted
    Ship& pool = s->player_ship();      // the fleet-wide inventory lives on the flagship
    i32 src  = s->pending_weapon_drag;
    i32 kind = s->pending_weapon_drag_kind;
    if (dst >= 0 && dst < fs.hardpoint_count) {
        const HardpointDef& dhp = fs.hardpoints[dst];
        b8 dst_takes_weapon  = hardpoint_accepts(&dhp, MODULE_TYPE_WEAPON);
        b8 dst_takes_defense = hardpoint_accepts(&dhp, MODULE_TYPE_DEFENSE);
        if (kind == 1 && src >= 0 && src < pool.weapon_stash_count) {
            // Mount an unmounted offensive weapon onto hardpoint dst; evict any occupant.
            // Size-gated like modules: a weapon mounts on slots of its own size or LARGER.
            Weapon* mounting = pool.weapon_stash[src];
            if (!dst_takes_weapon) {
                action_log_push(s, "'%s' is a %s slot - weapons don't fit.",
                                dhp.id, hardpoint_kind_label(dhp.accepts));
            } else if (mounting && mounting->size > (u8)dhp.size) {
                action_log_push(s, "'%s' is too small for the %s (needs %s).",
                                dhp.id, mounting->name ? mounting->name : "weapon",
                                hardpoint_size_name((HardpointSize)mounting->size));
            } else if (!ship_evict_module(fs, dst)) {
                action_log_push(s, "Module rack full - can't displace '%s'.",
                                fs.module_mounts[dst]->name);
            } else {
                if (fs.mounts[dst]) {
                    ship_stash_append(pool, fs.mounts[dst]);      // occupant weapon -> pool stash
                } else if (fs.point_defense_mount == dst) {
                    fs.point_defense_mount   = -1;                // occupant PD -> the fleet pool
                    fs.point_defense.enabled = FALSE;
                    ++s->fleet_pd_stock;
                }
                fs.mounts[dst] = mounting;
                fs.mount_groups[dst] = 1;                         // fresh mount -> group 1
                ship_stash_remove_at(pool, src);
                if (fs.active_weapon_idx < 0) fs.active_weapon_idx = dst; // first mount becomes active
                ship_rehome_weapons(fs);
                action_log_push(s, "%s mounted on '%s'.",
                                mounting && mounting->name ? mounting->name : "Weapon", dhp.id);
            }
        } else if (kind == 0 && src >= 0 && src < fs.hardpoint_count && src != dst && fs.mounts[src]) {
            // Rearrange a mounted offensive weapon from hardpoint src onto hardpoint dst.
            if (!dst_takes_weapon) {
                action_log_push(s, "'%s' is a %s slot - weapons don't fit.",
                                dhp.id, hardpoint_kind_label(dhp.accepts));
            } else if (fs.mounts[src]->size > (u8)dhp.size) {
                action_log_push(s, "'%s' is too small for the %s (needs %s).",
                                dhp.id, fs.mounts[src]->name ? fs.mounts[src]->name : "weapon",
                                hardpoint_size_name((HardpointSize)fs.mounts[src]->size));
            } else if (fs.mounts[dst] && fs.mounts[dst]->size > (u8)fs.hardpoints[src].size) {
                action_log_push(s, "Can't swap: '%s' is too small for the %s.",
                                fs.hardpoints[src].id,
                                fs.mounts[dst]->name ? fs.mounts[dst]->name : "occupant");
            } else if (fs.mounts[dst]) {
                Weapon* tmp    = fs.mounts[src];                  // dst holds a weapon -> swap
                fs.mounts[src] = fs.mounts[dst];
                fs.mounts[dst] = tmp;
                u8 gtmp = fs.mount_groups[src];                   // group assignment travels along
                fs.mount_groups[src] = fs.mount_groups[dst];
                fs.mount_groups[dst] = gtmp;
                if      (fs.active_weapon_idx == src) fs.active_weapon_idx = dst;
                else if (fs.active_weapon_idx == dst) fs.active_weapon_idx = src;
                ship_rehome_weapons(fs);
                action_log_push(s, "Weapon moved to '%s'.", dhp.id);
            } else if (fs.point_defense_mount == dst) {
                // dst holds the PD -> exchange places, but only if the PD fits on src.
                if (!hardpoint_accepts(&fs.hardpoints[src], MODULE_TYPE_DEFENSE)) {
                    action_log_push(s, "Can't swap: '%s' doesn't take the point-defense.",
                                    fs.hardpoints[src].id);
                } else {
                    fs.mounts[dst]         = fs.mounts[src];
                    fs.mounts[src]         = nullptr;
                    fs.mount_groups[dst]   = fs.mount_groups[src];
                    fs.mount_groups[src]   = 0;
                    fs.point_defense_mount = src;
                    if (fs.active_weapon_idx == src) fs.active_weapon_idx = dst;
                    ship_rehome_weapons(fs);
                    action_log_push(s, "Weapon moved to '%s'.", dhp.id);
                }
            } else if (!ship_evict_module(fs, dst)) {
                action_log_push(s, "Module rack full - can't displace '%s'.",
                                fs.module_mounts[dst]->name);
            } else {
                fs.mounts[dst] = fs.mounts[src];                  // dst empty -> move
                fs.mounts[src] = nullptr;
                fs.mount_groups[dst] = fs.mount_groups[src];
                fs.mount_groups[src] = 0;
                if (fs.active_weapon_idx == src) fs.active_weapon_idx = dst;
                ship_rehome_weapons(fs);
                action_log_push(s, "Weapon moved to '%s'.", dhp.id);
            }
        } else if (kind == 2 && s->fleet_pd_stock > 0 && fs.point_defense_mount < 0) {
            // Mount a point-defense device from the FLEET POOL onto hardpoint dst. Stock-gated:
            // the bay tile only shows while stock > 0, and a hull carries at most one device.
            if (!dst_takes_defense) {
                action_log_push(s, "'%s' is a %s slot - the point-defense doesn't fit.",
                                dhp.id, hardpoint_kind_label(dhp.accepts));
            } else if (!ship_evict_module(fs, dst)) {
                action_log_push(s, "Module rack full - can't displace '%s'.",
                                fs.module_mounts[dst]->name);
            } else {
                if (fs.mounts[dst]) {
                    ship_stash_append(pool, fs.mounts[dst]);      // occupant weapon -> pool stash
                    fs.mounts[dst] = nullptr;
                }
                --s->fleet_pd_stock;
                fs.point_defense_mount   = dst;
                fs.point_defense.enabled = TRUE;
                ship_rehome_weapons(fs);
                action_log_push(s, "Point Defense Laser mounted on '%s'.", dhp.id);
            }
        } else if (kind == 3 && src == fs.point_defense_mount && src != dst) {
            // Move the mounted point-defense from hardpoint src onto hardpoint dst.
            if (!dst_takes_defense) {
                action_log_push(s, "'%s' is a %s slot - the point-defense doesn't fit.",
                                dhp.id, hardpoint_kind_label(dhp.accepts));
            } else if (fs.mounts[dst] &&
                       !hardpoint_accepts(&fs.hardpoints[src], MODULE_TYPE_WEAPON)) {
                action_log_push(s, "Can't swap: '%s' doesn't take weapons.",
                                fs.hardpoints[src].id);
            } else if (!ship_evict_module(fs, dst)) {
                action_log_push(s, "Module rack full - can't displace '%s'.",
                                fs.module_mounts[dst]->name);
            } else {
                if (fs.mounts[dst]) {
                    fs.mounts[src] = fs.mounts[dst];              // dst holds a weapon -> exchange places
                    fs.mounts[dst] = nullptr;
                    fs.mount_groups[src] = fs.mount_groups[dst];
                    fs.mount_groups[dst] = 0;
                    if (fs.active_weapon_idx == dst) fs.active_weapon_idx = src;
                }
                fs.point_defense_mount = dst;
                ship_rehome_weapons(fs);
                action_log_push(s, "Point Defense Laser moved to '%s'.", dhp.id);
            }
        } else if (kind == 4 && src >= 0 && src < pool.module_stash_count) {
            // Install a rack module onto hardpoint dst; evict any occupant (weapon/PD/module).
            const ModuleDef* m = pool.module_stash[src];
            if (!hardpoint_fits_module(&dhp, m)) {
                if (!(dhp.accepts & m->type))
                    action_log_push(s, "'%s' is a %s slot - %s modules don't fit.",
                                    dhp.id, hardpoint_kind_label(dhp.accepts),
                                    hardpoint_kind_label(m->type));
                else
                    action_log_push(s, "'%s' is too small for %s (needs %s).",
                                    dhp.id, m->name, hardpoint_size_name(m->size));
            } else if (!ship_evict_module(fs, dst)) {
                action_log_push(s, "Module rack full - can't displace '%s'.",
                                fs.module_mounts[dst]->name);
            } else {
                if (fs.mounts[dst]) {
                    ship_stash_append(pool, fs.mounts[dst]);      // occupant weapon -> pool stash
                    fs.mounts[dst] = nullptr;
                    ship_rehome_weapons(fs);
                } else if (fs.point_defense_mount == dst) {
                    fs.point_defense_mount   = -1;                // occupant PD -> the fleet pool
                    fs.point_defense.enabled = FALSE;
                    ++s->fleet_pd_stock;
                }
                fs.module_mounts[dst] = m;
                ship_module_stash_remove_at(pool, src);
                ship_recompute_stats(&fs);
                action_log_push(s, "%s installed in '%s'.", m->name, dhp.id);
            }
        } else if (kind == 5 && src >= 0 && src < fs.hardpoint_count && src != dst &&
                   fs.module_mounts[src]) {
            // Move an installed module from hardpoint src onto hardpoint dst.
            const ModuleDef* m = fs.module_mounts[src];
            if (!hardpoint_fits_module(&dhp, m)) {
                if (!(dhp.accepts & m->type))
                    action_log_push(s, "'%s' is a %s slot - %s modules don't fit.",
                                    dhp.id, hardpoint_kind_label(dhp.accepts),
                                    hardpoint_kind_label(m->type));
                else
                    action_log_push(s, "'%s' is too small for %s (needs %s).",
                                    dhp.id, m->name, hardpoint_size_name(m->size));
            } else if (fs.mounts[dst] || fs.point_defense_mount == dst) {
                action_log_push(s, "'%s' is occupied - clear it first.", dhp.id);
            } else if (fs.module_mounts[dst]) {
                // dst holds another module -> swap, but only if it fits back on src.
                if (!hardpoint_fits_module(&fs.hardpoints[src], fs.module_mounts[dst])) {
                    action_log_push(s, "Can't swap: '%s' doesn't fit '%s'.",
                                    fs.module_mounts[dst]->name, fs.hardpoints[src].id);
                } else {
                    const ModuleDef* tmp  = fs.module_mounts[src];
                    fs.module_mounts[src] = fs.module_mounts[dst];
                    fs.module_mounts[dst] = tmp;
                    ship_recompute_stats(&fs);
                    action_log_push(s, "%s moved to '%s'.", m->name, dhp.id);
                }
            } else {
                fs.module_mounts[dst] = m;                        // dst empty -> move
                fs.module_mounts[src] = nullptr;
                ship_recompute_stats(&fs);
                action_log_push(s, "%s moved to '%s'.", m->name, dhp.id);
            }
        }
    }
    s->pending_weapon_drag = -1;
}

// Which INSPECTED-ship hardpoint box (if any) sits under the mouse cursor? While the
// full-screen inspector is open the hull lives in the PORTRAIT, so the portrait mapping is
// tried first; a cursor inside the portrait square that hits no box is a MISS (it must not
// fall through to the world hull hidden behind the panel). Outside the square, the world-space
// test still runs -- the hull can poke out beyond the portrait when the camera happens to
// frame it. Boxes are tested in ship-local space, each rotated by its mount's facing, with a
// grace margin over the drawn overlay box; when boxes overlap the nearest center wins.
static i32 inspected_hardpoint_at_cursor(game_state* s) {
    if (s->show_flagship_inspector) {
        i32 pmx = 0, pmy = 0;
        input_get_mouse_position(&pmx, &pmy);
        i32 hit = ship_portrait_hardpoint_at(s, (f32)pmx, (f32)pmy);
        if (hit >= 0) return hit;
        f32 rx, ry, rw, rh;
        ship_portrait_rect(s, &rx, &ry, &rw, &rh);
        if ((f32)pmx >= rx && (f32)pmx <= rx + rw && (f32)pmy >= ry && (f32)pmy <= ry + rh)
            return -1;
    }
    Ship& fs   = s->inspected_ship();
    Vec2  lp   = ship_world_to_local(&fs, mouse_true_hierpos(s));
    i32   hit  = -1;
    f32   best = 1.0e30f;
    for (i32 i = 0; i < fs.hardpoint_count; ++i) {
        const HardpointDef& hp = fs.hardpoints[i];
        Vec2 d = vec2_rotate(vec2_sub(lp, hp.pos_local), -hp.facing);
        f32  e = hardpoint_half_extent(hp.size) * 1.5f;
        if (fabsf(d.x) <= e && fabsf(d.y) <= e) {
            f32 d2 = d.x * d.x + d.y * d.y;
            if (d2 < best) { best = d2; hit = i; }
        }
    }
    return hit;
}

// Portrait view back to the fit framing. Opening the inspector and switching hulls both go
// through here -- a new ship must never inherit the previous one's zoom/pan.
static void inspector_view_reset(game_state* s) {
    s->insp_view_zoom     = 1.0f;
    s->insp_view_pan      = Vec2{ 0.0f, 0.0f };
    s->insp_view_dragging = false;
}

// ---- Inspector portrait view input (wheel zoom + drag pan) -------------------------------
// The portrait is a camera onto the hull: the wheel zooms toward the cursor and a left-drag
// on EMPTY portrait space pans. Presses on a hardpoint box are left alone -- they arm the
// loadout drag in the pickup block below, which keeps priority. Both gestures work through
// ship_portrait_screen_to_world, so the pin/pan math shares the exact camera the portrait is
// drawn and hit-tested with. The world camera cannot fight over the wheel: CameraControl
// discards it while the inspector is open (camera_controller.cpp).
static const f32 PORTRAIT_ZOOM_STEP = 1.15f;   // multiplicative per wheel notch
static const f32 PORTRAIT_ZOOM_MAX  = 8.0f;    // x the fit framing

static void update_portrait_view_input(game_state* s) {
    i32 mxi = 0, myi = 0;
    input_get_mouse_position(&mxi, &myi);
    f32 mx = (f32)mxi, my = (f32)myi;
    f32 rx, ry, rw, rh;
    ship_portrait_rect(s, &rx, &ry, &rw, &rh);
    b8 inside  = mx >= rx && mx <= rx + rw && my >= ry && my <= ry + rh;
    b8 over_ui = bs_imgui_wants_mouse() || bs_rml_wants_mouse();

    // Wheel: zoom toward the cursor. Re-projecting the cursor before and after the zoom
    // change and folding the drift into the pan keeps the pointed-at spot pinned in place.
    i32 wheel = input_get_mouse_wheel();
    if (wheel != 0 && inside && !over_ui) {
        f32 bx, by, ax, ay;
        ship_portrait_screen_to_world(s, mx, my, &bx, &by);
        s->insp_view_zoom = clampf(s->insp_view_zoom * powf(PORTRAIT_ZOOM_STEP, (f32)wheel),
                                   1.0f, PORTRAIT_ZOOM_MAX);
        ship_portrait_screen_to_world(s, mx, my, &ax, &ay);
        s->insp_view_pan = vec2_add(s->insp_view_pan, Vec2{ bx - ax, by - ay });
    }

    // Left-drag pan: armed by a press on empty portrait space (inside the square, over no
    // hardpoint box, no loadout drag airborne); keeps tracking after the cursor leaves the
    // square; any release disarms. The grabbed world point stays under the cursor.
    b8 held    = input_is_button_down(BUTTON_LEFT);
    b8 pressed = held && !input_was_button_down(BUTTON_LEFT);
    if (pressed && inside && !over_ui && !s->world_module_drag &&
        s->pending_weapon_drag < 0 && ship_portrait_hardpoint_at(s, mx, my) < 0) {
        s->insp_view_dragging = true;
        s->insp_view_drag_px  = Vec2{ mx, my };
    }
    if (s->insp_view_dragging) {
        if (!held) {
            s->insp_view_dragging = false;
        } else {
            f32 px, py, cx, cy;
            ship_portrait_screen_to_world(s, s->insp_view_drag_px.x, s->insp_view_drag_px.y, &px, &py);
            ship_portrait_screen_to_world(s, mx, my, &cx, &cy);
            s->insp_view_pan     = vec2_add(s->insp_view_pan, Vec2{ px - cx, py - cy });
            s->insp_view_drag_px = Vec2{ mx, my };
        }
    }

    // Keep the hull reachable: the pan may never exceed its bounding radius, so some part of
    // the ship always stays inside (or one screen away from) the frame.
    f32 r   = ship_bounding_radius(&s->inspected_ship());
    f32 len = sqrtf(s->insp_view_pan.x * s->insp_view_pan.x + s->insp_view_pan.y * s->insp_view_pan.y);
    if (len > r) s->insp_view_pan = vec2_scale(s->insp_view_pan, r / len);
}

// ---- RmlUi HUD snapshot ------------------------------------------------------------------
// Gather the in-game HUD state (nav / ship / encounter / action-log / discoveries) into a
// plain-old-data snapshot, push it to the RmlUi data model, then drain any button clicks the
// document produced this frame. The engine owns the RmlUi data model; the game only ever sees
// this POD facade (RmlUi symbols are not exported from engine.dll). Replaces the old bs_ui
// immediate-mode panels (draw_nav_ship_hud / draw_encounter_panel / build_action_log_panel /
// build_discoveries_panel).
static void game_push_hud(game_state* s, f32 dt) {
    static bs_rml_hud_state hud;
    memset(&hud, 0, sizeof(hud));

    // ---- Navigation + Ship (arena-side affordance, hidden in edit mode) -----------------
    if (s->view_arena_w > 0.5f && !s->editor.edit_mode_active) {
        i32 nearest = galaxy_nearest_node(s, &s->galaxy.map_entities[0].galaxy_pos);
        if (nearest >= 0) {
            f64 sx, sy, nx, ny;
            hierpos_to_f64(&s->galaxy.map_entities[0].galaxy_pos, BS_HIERPOS_CELL_SIZE, &sx, &sy);
            hierpos_to_f64(&s->galaxy.nodes[nearest].galaxy_center, BS_HIERPOS_CELL_SIZE, &nx, &ny);
            f64 dist = sqrt((sx - nx) * (sx - nx) + (sy - ny) * (sy - ny));

            hud.nav_visible = TRUE;
            snprintf(hud.nav_sector, sizeof(hud.nav_sector), "Alpha");
            snprintf(hud.nav_system, sizeof(hud.nav_system), "%s",
                     s->galaxy.nodes[nearest].name[0] ? s->galaxy.nodes[nearest].name : "?");
            if (dist >= 1000000.0)
                snprintf(hud.nav_distance, sizeof(hud.nav_distance), "%.2f M u", dist / 1000000.0);
            else if (dist >= 1000.0)
                snprintf(hud.nav_distance, sizeof(hud.nav_distance), "%.2f k u", dist / 1000.0);
            else
                snprintf(hud.nav_distance, sizeof(hud.nav_distance), "%.0f u", dist);
            snprintf(hud.nav_zone, sizeof(hud.nav_zone), "%d", get_system_zone(s, s->galaxy.current_system));

            if (!s->camera_state.free_camera_active) {
                hud.ship_visible = TRUE;
                f32 speed = vec2_length(s->player_flight().velocity);
                snprintf(hud.ship_speed, sizeof(hud.ship_speed), "%.1f u/s", speed);
            }
        }
    }

    // ---- Time control (top-center): pause + speed tiers on the shared in-game calendar --
    if (!s->editor.edit_mode_active) {
        hud.time_visible = TRUE;
        i64 total_hours = (i64)s->sim_hours;
        i32 day = (i32)((total_hours % HOURS_PER_YEAR) / HOURS_PER_DAY) + 1;
        snprintf(hud.time_date, sizeof(hud.time_date), "Year %d, Day %d",
                 s->galaxy.clock.present_year, day);
        // Active tier index for button highlight (0=Pause,1=1x,2=3x,3=5x,4=10x). A scale
        // between 0 and 1 matches NO tier (-1, which no button binds to) -- unreachable since
        // the command overlay's 0.25x dilation was retired, kept so a future fractional scale
        // cannot claim the game is paused while it is merely slowed.
        f32 ts = s->time_scale;
        hud.time_tier = (ts >= 10.0f) ? 4 : (ts >= 5.0f) ? 3 : (ts >= 3.0f) ? 2 : (ts >= 1.0f) ? 1
                      : (ts <= 0.0f) ? 0 : -1;
    }

    // ---- Encounter modal (centered) -----------------------------------------------------
    if (s->encounter_active && !s->editor.edit_mode_active) {
        hud.enc_visible = TRUE;
        snprintf(hud.enc_name, sizeof(hud.enc_name), "%s", s->fleet_state.enemy_ship.vessel_name);
        Vec2 delta = hierpos_diff(&s->player_ship().origin, &s->fleet_state.enemy_ship.origin, BS_HIERPOS_CELL_SIZE);
        snprintf(hud.enc_distance, sizeof(hud.enc_distance), "%.1f m", vec2_length(delta));
        snprintf(hud.enc_faction, sizeof(hud.enc_faction), "%s",
                 vessel_faction_name(s->fleet_state.enemy_ship.faction));
        snprintf(hud.enc_desc, sizeof(hud.enc_desc), "%s",
                 vessel_faction_desc(s->fleet_state.enemy_ship.faction));
    }

    // ---- Action log (bottom-right; game-driven idle fade after 3s) ----------------------
    if (!s->editor.edit_mode_active) {
        s->action_log.inactivity_timer += dt;
        f32 alpha = 1.0f;
        if (s->action_log.inactivity_timer > 3.0f) {
            f32 t = (s->action_log.inactivity_timer - 3.0f) / 2.0f;
            if (t > 1.0f) t = 1.0f;
            alpha = 1.0f - t * 0.85f; // fade to 15%
        }
        hud.log_visible = TRUE;
        hud.log_alpha   = alpha;
        i32 first = s->action_log.count - BS_RML_LOG_MAX;
        if (first < 0) first = 0;
        i32 n = 0;
        for (i32 i = first; i < s->action_log.count && n < BS_RML_LOG_MAX; ++i, ++n)
            snprintf(hud.log[n].text, sizeof(hud.log[n].text), "%s", s->action_log.entries[i]);
        hud.log_count = n;
    }

    // ---- Discoveries browser (newest first) ---------------------------------------------
    if (s->show_discoveries) {
        static const char* kind_labels[] = { "Patrol", "Miner", "Trader", "Warship", "Station", "Other" };
        hud.disc_visible = TRUE;
        snprintf(hud.disc_count_label, sizeof(hud.disc_count_label), "%d objects logged", s->discovery_log_count);
        i32 n = 0;
        for (i32 i = s->discovery_log_count - 1; i >= 0 && n < BS_RML_DISC_MAX; --i, ++n) {
            const DiscoveryLogEntry& e = s->discovery_log[i];
            const char* kind  = (e.kind < 6) ? kind_labels[e.kind] : kind_labels[5];
            const char* color = "#ccd9e6ff"; // neutral
            if      (e.kind == DISCOVERY_KIND_STATION)                                     color = "#8cbff2ff";
            else if (e.kind == DISCOVERY_KIND_PATROL || e.kind == DISCOVERY_KIND_WARSHIP)  color = "#f28c73ff";
            else if (e.kind == DISCOVERY_KIND_MINER)                                       color = "#8cf28cff";
            else if (e.kind == DISCOVERY_KIND_TRADER)                                      color = "#f2d859ff";
            snprintf(hud.disc[n].text, sizeof(hud.disc[n].text), "[%.1fs] %s  -  %s  (%s)",
                     e.time, e.system, e.name, kind);
            snprintf(hud.disc[n].color, sizeof(hud.disc[n].color), "%s", color);
        }
        hud.disc_count = n;
    }

    // ---- Fleet ship panel (top-right; piloted-ship combat readout + controls) -----------
    // fleet_cap_w must ALWAYS hold a valid CSS length (same rule as tip_left/tip_top below):
    // the gauge's data-style-width binding evaluates even while the panel is hidden.
    // Hidden while the flagship inspector is open: the inspector overlaps the fleet panel's
    // screen region (the capacitor bar peeked out beside the window) and shows the same data.
    snprintf(hud.fleet_cap_w, sizeof(hud.fleet_cap_w), "0%%");
    if (!s->editor.edit_mode_active && !s->show_flagship_inspector) {
        FleetShip* piloted = s->fleet_state.fleet.piloted();
        if (!piloted) piloted = &s->fleet_state.fleet.at(0);
        if (piloted) {
            Ship* ship = &piloted->ship;
            hud.fleet_visible = TRUE;
            snprintf(hud.fleet_name, sizeof(hud.fleet_name), "%s",
                     ship->vessel_name ? ship->vessel_name : "Ship");
            snprintf(hud.fleet_faction, sizeof(hud.fleet_faction), "%s", vessel_faction_name(ship->faction));
            snprintf(hud.fleet_speed, sizeof(hud.fleet_speed), "%.0f", vec2_length(piloted->flight.velocity));
            snprintf(hud.fleet_heading, sizeof(hud.fleet_heading), "%.0f", ship->angle * bs_math::BS_RAD2DEG);
            snprintf(hud.fleet_health, sizeof(hud.fleet_health), "%s", "--");
            // Fire-group rows, always all five: "K - weapon, weapon, ..." listing each group's
            // member weapons by mount (hardpoint id up to the first '_'). The active group row
            // is highlighted; empty groups render dim ("K -") but stay clickable. Clicking a
            // row selects that group ("group:N"), same as its number key.
            for (i32 g = 0; g < SHIP_WEAPON_GROUPS && g < BS_RML_GROUP_MAX; ++g) {
                bs_rml_weapon_line& row = hud.fleet_group[g];
                i32 len = snprintf(row.text, sizeof(row.text), "%d -", g + 1);
                i32 members = 0;
                for (i32 i = 0; i < ship->hardpoint_count; ++i) {
                    if (!ship->mounts[i] || !((ship->mount_groups[i] >> g) & 1)) continue;
                    char shortid[12];
                    hardpoint_short_id(ship->hardpoints[i], shortid, (i32)sizeof(shortid));
                    if (len < (i32)sizeof(row.text))
                        len += snprintf(row.text + len, sizeof(row.text) - (size_t)len, "%s %s",
                                        members ? "," : "", shortid);
                    ++members;
                }
                snprintf(row.action, sizeof(row.action), "group:%d", g);
                row.selected = (g == ship->active_group);
                row.empty    = (members == 0);   // drives the dim style (still clickable)
            }
            // RELEASE-ONLY button: shown while piloting, reads "Auto-pilot / RTS", hands the
            // hull back to its autopilot. The attach direction ("Pilot unit") is retired --
            // piloting is entered per-ship from the right-click context menu, so while
            // detached the panel simply has no mode control. Hidden (not dimmed) during the
            // recenter glide too: free_camera_active stays TRUE until the glide lands.
            snprintf(hud.fleet_mode_label, sizeof(hud.fleet_mode_label), "Auto-pilot / RTS");
            hud.fleet_mode_visible = s->camera_state.free_camera_active ? FALSE : TRUE;
            hud.fleet_mode_enabled = s->camera_state.recentering ? FALSE : TRUE;
            // Capacitor + PD doctrine readout (Phase E): the piloted ship's live bank and
            // the stance/priority/gate line; amber cue whenever the stance is not STANDARD.
            f32 cap_frac = (ship->cap_max > 1.0e-4f) ? ship->cap_current / ship->cap_max : 0.0f;
            snprintf(hud.fleet_cap_w, sizeof(hud.fleet_cap_w), "%.0f%%", cap_frac * 100.0f);
            snprintf(hud.fleet_cap_label, sizeof(hud.fleet_cap_label), "Capacitor %.0f / %.0f",
                     ship->cap_current, ship->cap_max);
            static const char* PD_ST[3] = { "HOLD", "STANDARD", "OVERDRIVE" };
            // Short priority names for the one-line panel label (the doctrine chips carry the
            // full names); gate as a bare percentage. "PD: --" when this hull carries no
            // device -- doctrine on a PD that is not there would be a lie.
            static const char* PD_PR[3] = { "IMPACT", "MISSILES", "NEAREST" };
            static const char* PD_GT[3] = { "60%", "80%", "100%" };
            const DefenseLaser& pdl = ship->point_defense;
            if (ship->point_defense_mount >= 0) {
                snprintf(hud.fleet_pd_label, sizeof(hud.fleet_pd_label), "PD: %s - %s - %s",
                         PD_ST[pdl.stance % 3], PD_PR[pdl.priority % 3], PD_GT[pdl.gate_tier % 3]);
                hud.fleet_pd_warn = (pdl.stance != PD_STANDARD) ? TRUE : FALSE;
            } else {
                snprintf(hud.fleet_pd_label, sizeof(hud.fleet_pd_label), "PD: --");
                hud.fleet_pd_warn = FALSE;
            }
            // Speed-limit gear chips: the hull's governor list (first 5 gears, u/s labels)
            // and the selected index. The chips emit "spd:N" into the action drain below.
            {
                const ShipDef* def = ship->def;
                i32 nlim = def ? def->speed_limit_count : 0;
                i32 sel  = ship->speed_limit_sel;
                if (sel < 0) sel = 0;
                if (nlim > 0 && sel >= nlim) sel = nlim - 1;
                hud.speed_sel = sel;
                for (i32 k = 0; k < 5; ++k) {
                    f32 v = (def && k < nlim) ? def->speed_limits[k] : ship->motion.max_speed;
                    snprintf(hud.speed_lim[k], sizeof(hud.speed_lim[k]), "%.0f", v);
                }
            }
        }
    }

    // ---- Fleet roster (top-left; always up) ---------------------------------------------
    // One row per fleet member: label, live order readout, stance chips. ALWAYS visible now
    // (the command overlay is retired): commanding is not a mode any more, and the roster's
    // clicks are UI-owned (bs_rml_wants_mouse), so it never fights the world in either
    // control mode -- it is also the mid-flight selection surface, since box selection needs
    // the detached LMB. All interaction is action strings ("fsel:N" / "fstance:N:S") drained
    // below; shift-additive selection is resolved at drain time so the engine carries no
    // modifier state.
    if (s->fleet_state.fleet.count() > 0 &&
        !s->editor.edit_mode_active) {
        Fleet& roster_fleet = s->fleet_state.fleet;
        hud.roster_visible = TRUE;
        i32 rn = roster_fleet.count();
        if (rn > BS_RML_ROSTER_MAX) rn = BS_RML_ROSTER_MAX;
        hud.roster_count = rn;
        const i32 piloted_row = roster_fleet.piloted_index();
        const i32 hovered_row = s->rts_controls.hovered_index();
        static const char* const STANCE_CHIP[4] = { "A", "D", "P", "H" };
        for (i32 i = 0; i < rn; ++i) {
            const FleetShip& fs = roster_fleet.at(i);
            bs_rml_roster_row& row = hud.roster[i];
            const char* nm = fs.ship.vessel_name ? fs.ship.vessel_name : "ship";
            snprintf(row.label, sizeof(row.label), "%d %.24s", i + 1, nm);
            // Order state, so the roster answers "what is this ship doing" without a second
            // panel. Attack is orthogonal to the position orders and wins the readout.
            const char* what = "IDLE";
            if      (fs.has_attack_target) what = "ATTACK";
            else if (fs.has_escort_target) what = "ESCORT";
            else if (fs.has_avoid_target)  what = "AVOID";
            else if (fs.has_move_target)   what = "MOVE";
            snprintf(row.status, sizeof(row.status), "%s", what);
            snprintf(row.action, sizeof(row.action), "fsel:%d", i);
            snprintf(row.action_insp, sizeof(row.action_insp), "insp:%d", i);
            row.selected = roster_fleet.is_selected(i);
            row.piloted  = (i == piloted_row);
            row.hovered  = (i == hovered_row);
            for (i32 st = 0; st < 4; ++st) {
                snprintf(row.chip[st].label,  sizeof(row.chip[st].label),  "%s", STANCE_CHIP[st]);
                snprintf(row.chip[st].action, sizeof(row.chip[st].action), "fstance:%d:%d", i, st);
                row.chip[st].on = (fs.stance == (u8)st) ? TRUE : FALSE;
            }
            // ROE cycling chip (slot 4): shows the current rules-of-engagement letter, click
            // cycles free -> return -> hold. Lit only when the ship DEVIATES from the
            // weapons-free default, so a glance finds the hulls on a leash.
            {
                static const char* ROE_CHIP[3] = { "F", "R", "H" };
                u8 r = (fs.roe <= (u8)ROE_HOLD) ? fs.roe : (u8)ROE_WEAPONS_FREE;
                snprintf(row.chip[4].label,  sizeof(row.chip[4].label),  "%s", ROE_CHIP[r]);
                snprintf(row.chip[4].action, sizeof(row.chip[4].action), "froe:%d", i);
                row.chip[4].on = (r != (u8)ROE_WEAPONS_FREE) ? TRUE : FALSE;
            }
        }
    }

    // ---- FTL jump-mode banner (bottom-center) -------------------------------------------
    if (!s->editor.edit_mode_active && s->rts_controls.jump_mode_active())
        hud.jump_visible = TRUE;

    // Active UI font kit (editor-panel selectable): drives the body class that swaps typefaces.
    hud.ui_kit = s->ui_font_kit;

    // ---- Flagship inspector (floating side window; bottom-center Inspector button) -------
    // The launcher button shows during gameplay; the window shows while it is open. The single
    // MODULES tab presents the flagship's UNMOUNTED inventory as ONE unified bay grid: weapons
    // (drag action "inv:K"), the point-defense ("defdrag") and ship modules ("mod:K") together,
    // padded to BS_RML_BAY_MAX with inert empty sockets. Mounting happens by dragging an item
    // out of the window onto one of the cruiser's world hardpoint boxes; the bay well is one
    // unmount drop target ("baydrop", routed game-side by dragged kind). An item lives in
    // EITHER the bay OR on a hardpoint, never both. Each tile carries a structured hover STAT
    // CARD (TYPE/SIZE/INTEGRITY rows + per-mode stat blocks + keybind footer) built from the
    // instance's def-driven stats; card_rows uses '\n' (the RCSS is pre-line). The stat sheet
    // is COMPLETE and NUMERIC: every def property appears as its actual value (derived numbers
    // like DPS/range shown alongside their inputs, parentheticals explain but never replace).
    // SIZE lives in its own card_size element so the RCSS can paint it green (fits a flagship
    // hardpoint) or red (no hardpoint big enough / of the right kind).
    // Any hardpoint of matching kind and sufficient size counts, occupied or not: the player
    // can always evict/swap, so "fits" answers "COULD this ever mount here", not "is a slot free".
    auto ship_has_weapon_slot = [](const Ship& fs, u8 wsize) -> b8 {
        for (i32 h = 0; h < fs.hardpoint_count; ++h)
            if (hardpoint_accepts(&fs.hardpoints[h], MODULE_TYPE_WEAPON)
                && (u8)fs.hardpoints[h].size >= wsize) return TRUE;
        return FALSE;
    };
    auto fill_card_weapon = [s, &ship_has_weapon_slot](bs_rml_bay_line& row, const Ship& fs,
                                                       const Weapon* w) {
        const char* name = w->name ? w->name : "?";
        // Mount-size element (a weapon mounts on slots of its own size or LARGER).
        static const char* SIZE_ROW[3] = { "Small - fits S/M/L mounts", "Medium - fits M/L mounts",
                                           "Large - fits L mounts" };
        snprintf(row.card_title, sizeof(row.card_title), "%s", name);
        snprintf(row.card_size, sizeof(row.card_size), "SIZE       %s",
                 SIZE_ROW[(w->size < 3) ? w->size : 1]);
        row.card_size_ok = ship_has_weapon_slot(fs, w->size);
        snprintf(row.card_desc, sizeof(row.card_desc), "%s", w->def ? w->def->desc : "");
        if (w->wkind == WEAPON_KIND_MISSILE) {
            const MissileLauncher* ml = (const MissileLauncher*)w;
            i32 n = snprintf(row.card_rows, sizeof(row.card_rows),
                     "TYPE       Weapon - Missile (guided)\n"
                     "INTEGRITY  100%%\n"
                     "DAMAGE     %.0f per missile\n"
                     "RELOAD     %.1fs per tube\n"
                     "RANGE      %.0fk  (%.0f u/s x %.0fs)\n"
                     "SEEKER     %.0f deg cone  %.0fk lock range\n"
                     "ENERGY     %.1f per launch\n"
                     "SIGNATURE  %.1f  (sensor emission, 0-1)\n"
                     "MISSILE HP %.1f  (vs point-defense)",
                     w->damage, ml->reload_time,
                     ml->missile_speed * ml->missile_lifetime / 1000.0f,
                     ml->missile_speed, ml->missile_lifetime,
                     s->missile_tuning.seeker_cone_deg, s->missile_tuning.seeker_range / 1000.0f,
                     ml->cap_cost_value, ml->missile_emission, ml->missile_hp);
            if (w->def && n > 0 && n < (i32)sizeof(row.card_rows))
                snprintf(row.card_rows + n, sizeof(row.card_rows) - n,
                         "\nVALUE      %d cr  -  tier %d", w->def->price, w->def->tier);
            snprintf(row.card_mode_a, sizeof(row.card_mode_a), "> GUIDED - anti-ship");
            snprintf(row.card_mode_a_stats, sizeof(row.card_mode_a_stats),
                     "DMG %.0f  RELOAD %.1fs  PWR %.0f", w->damage, ml->reload_time, ml->cap_cost_value);
            row.card_mode_b[0] = row.card_mode_b_stats[0] = row.card_foot[0] = '\0';
        } else {
            const BallisticWeapon* bw = (const BallisticWeapon*)w;
            i32 n = snprintf(row.card_rows, sizeof(row.card_rows),
                     "TYPE       Weapon - Ballistic (dual role)\n"
                     "INTEGRITY  100%%\n"
                     "DAMAGE     %.0f per shell\n"
                     "RATE       %.1f/s  (%.0f dmg/s)\n"
                     "RANGE      %.0fk  (%.0f u/s x %.0fs)\n"
                     "ENERGY     %.1f per shot  (%.1f/s sustained)\n"
                     "SIGNATURE  %.1f  (sensor emission, 0-1)\n"
                     "SHELL HP   %.1f  (vs enemy point-defense)",
                     w->damage, bw->fire_rate, w->damage * bw->fire_rate,
                     bw->projectile_speed_value * bw->projectile_lifetime / 1000.0f,
                     bw->projectile_speed_value, bw->projectile_lifetime,
                     bw->cap_cost_value, bw->cap_cost_value * bw->fire_rate,
                     bw->projectile_emission, bw->proj_hp_value);
            if (w->def && n > 0 && n < (i32)sizeof(row.card_rows))
                snprintf(row.card_rows + n, sizeof(row.card_rows) - n,
                         "\nVALUE      %d cr  -  tier %d", w->def->price, w->def->tier);
            const b8 flak = (bw->fire_mode == MODE_FLAK);
            // The LIVE mode leads (amber block, "> " chevron); the other follows dimmed.
            if (!flak) {
                snprintf(row.card_mode_a, sizeof(row.card_mode_a), "> AP SHELLS - anti-ship");
                snprintf(row.card_mode_a_stats, sizeof(row.card_mode_a_stats),
                         "DMG %.0f  RATE %.1f/s  PWR %.1f", w->damage, bw->fire_rate, bw->cap_cost_value);
                snprintf(row.card_mode_b, sizeof(row.card_mode_b), "FLAK SCREEN - anti-ordnance");
                snprintf(row.card_mode_b_stats, sizeof(row.card_mode_b_stats), "no hull damage");
            } else {
                snprintf(row.card_mode_a, sizeof(row.card_mode_a), "> FLAK SCREEN - anti-ordnance");
                snprintf(row.card_mode_a_stats, sizeof(row.card_mode_a_stats), "no hull damage");
                snprintf(row.card_mode_b, sizeof(row.card_mode_b), "AP SHELLS - anti-ship");
                snprintf(row.card_mode_b_stats, sizeof(row.card_mode_b_stats),
                         "DMG %.0f  RATE %.1f/s  PWR %.1f", w->damage, bw->fire_rate, bw->cap_cost_value);
            }
            snprintf(row.card_foot, sizeof(row.card_foot), "[T] switches fire mode on the active group");
        }
    };
    // Portrait element placement: must ALWAYS hold valid CSS lengths (the data-style bindings
    // evaluate even while the inspector is hidden -- same rule as tip_left/tip_top). left/top
    // are relative to the inspector's middle zone; the same rect drives the game-side portrait
    // hardpoint hit-test, so the element and the hit-test cannot drift from each other.
    {
        f32 prx, pry, prw, prh, pcx, pcy;
        ship_portrait_rect(s, &prx, &pry, &prw, &prh);
        ship_portrait_center_origin(s, &pcx, &pcy);
        snprintf(hud.insp_portrait_left, sizeof(hud.insp_portrait_left), "%.0fpx", prx - pcx);
        snprintf(hud.insp_portrait_top,  sizeof(hud.insp_portrait_top),  "%.0fpx", pry - pcy);
        snprintf(hud.insp_portrait_w,    sizeof(hud.insp_portrait_w),    "%.0fpx", prw);
        snprintf(hud.insp_portrait_h,    sizeof(hud.insp_portrait_h),    "%.0fpx", prh);
    }
    if (!s->editor.edit_mode_active) {
        hud.inspector_btn_visible = TRUE;
        if (s->show_flagship_inspector) {
            Ship& fs   = s->inspected_ship();   // the hull under inspection
            Ship& pool = s->player_ship();      // fleet-wide inventory (flagship's stash arrays)
            hud.inspector_visible = TRUE;
            snprintf(hud.insp_ship_name, sizeof(hud.insp_ship_name), "%s",
                     fs.vessel_name ? fs.vessel_name : "Ship");
            // ---- Left-column status block (pre-formatted; '\n' rows) ----------------------
            {
                i32 ii = s->inspected_ship_idx;
                if (ii < 0 || ii >= s->fleet_state.fleet.count()) ii = 0;
                const FleetShip& ifs = s->fleet_state.fleet.at(ii);
                // Engagement doctrine chips (Doctrine tab): the inspected ship's live values
                // drive the active-chip highlights; clicks come back as "doc:*" actions.
                hud.insp_roe     = (i32)ifs.roe;
                hud.insp_flak    = (i32)ifs.flak_doctrine;
                hud.insp_missile = (i32)ifs.missile_policy;
                // Nearest tier for the highlight (off / 15% / 25% / 40%): the editor slider
                // can set in-between values; the chip shows the closest step.
                hud.insp_capfloor = (ifs.cap_fire_floor < 0.075f) ? 0
                                  : (ifs.cap_fire_floor < 0.20f)  ? 1
                                  : (ifs.cap_fire_floor < 0.325f) ? 2 : 3;
                static const char* const ST_NAME[4] =
                    { "AGGRESSIVE", "DEFENSIVE", "PASSIVE", "HOLD FIRE" };
                const char* what = "IDLE";
                if      (ifs.has_attack_target) what = "ATTACK";
                else if (ifs.has_escort_target) what = "ESCORT";
                else if (ifs.has_avoid_target)  what = "AVOID";
                else if (ifs.has_move_target)   what = "MOVE";
                i32 fitted = 0;
                for (i32 h = 0; h < fs.hardpoint_count; ++h)
                    if (fs.mounts[h] || fs.module_mounts[h] || fs.point_defense_mount == h)
                        ++fitted;
                static const char* PD_ST[3] = { "HOLD", "STANDARD", "OVERDRIVE" };
                static const char* PD_PR[3] = { "IMPACT", "MISSILES", "NEAREST" };
                static const char* PD_GT[3] = { "60%", "80%", "100%" };
                // Doctrine reads out only while this hull carries the fleet's PD device.
                char pd_line[48];
                if (fs.point_defense_mount >= 0) {
                    const DefenseLaser& ipd = fs.point_defense;
                    snprintf(pd_line, sizeof(pd_line), "%s - %s - %s",
                             PD_ST[ipd.stance % 3], PD_PR[ipd.priority % 3], PD_GT[ipd.gate_tier % 3]);
                } else {
                    snprintf(pd_line, sizeof(pd_line), "--");
                }
                snprintf(hud.insp_status, sizeof(hud.insp_status),
                         "MEMBER     %d of %d\n"
                         "STANCE     %s\n"
                         "ORDER      %s\n"
                         "CAPACITOR  %.0f / %.0f\n"
                         "PD         %s\n"
                         "MOUNTS     %d of %d fitted",
                         ii + 1, s->fleet_state.fleet.count(),
                         ST_NAME[ifs.stance & 3], what,
                         fs.cap_current, fs.cap_max,
                         pd_line,
                         fitted, fs.hardpoint_count);
            }
            hud.insp_show_loadout  = (s->insp_tab == 0) ? TRUE : FALSE;
            hud.insp_show_doctrine = (s->insp_tab == 1) ? TRUE : FALSE;
            // ---- Fleet list: one live-thumbnail entry per member; click retargets the
            // inspector via the existing "insp:N" action. rect slices the "bs:thumbs" strip.
            {
                Fleet& fl = s->fleet_state.fleet;
                i32 n = fl.count();
                if (n > BS_RML_ROSTER_MAX) n = BS_RML_ROSTER_MAX;
                hud.insp_ship_count = n;
                i32 cur = s->inspected_ship_idx;
                if (cur < 0 || cur >= fl.count()) cur = 0;
                for (i32 i = 0; i < n; ++i) {
                    const FleetShip& m = fl.at(i);
                    bs_rml_insp_ship& e = hud.insp_ships[i];
                    snprintf(e.sprite, sizeof(e.sprite), "bs-thumb-%d", i);
                    snprintf(e.name, sizeof(e.name), "%s",
                             m.ship.vessel_name ? m.ship.vessel_name : "Ship");
                    const char* mwhat = "IDLE";
                    if      (m.has_attack_target) mwhat = "ATTACK";
                    else if (m.has_escort_target) mwhat = "ESCORT";
                    else if (m.has_avoid_target)  mwhat = "AVOID";
                    else if (m.has_move_target)   mwhat = "MOVE";
                    snprintf(e.status, sizeof(e.status), "%s", mwhat);
                    snprintf(e.action, sizeof(e.action), "insp:%d", i);
                    e.selected = (i == cur) ? TRUE : FALSE;
                }
            }
            // ---- Unified Modules bay (the FLEET-WIDE pool: drag sources "inv:K"/"mod:K") --
            i32 nb = 0;
            // 1) Weapons from the pool stash (drag sources "inv:K").
            for (i32 k = 0; k < pool.weapon_stash_count && nb < BS_RML_BAY_MAX; ++k) {
                Weapon* w = pool.weapon_stash[k];
                if (!w) continue;
                bs_rml_bay_line& row = hud.bay[nb++];
                const char* name = w->name ? w->name : "?";
                snprintf(row.glyph,  sizeof(row.glyph),  "%c", name[0] ? name[0] : '*');
                snprintf(row.icon,   sizeof(row.icon),   "%s", w->icon ? w->icon : "");
                snprintf(row.action, sizeof(row.action), "inv:%d", k);
                row.empty    = FALSE;
                row.selected = FALSE;
                fill_card_weapon(row, fs, w);
            }
            // 2) Point-defense, while the fleet POOL holds a device (a mounted PD lives on its
            // hardpoint, on whichever hull carries it). The card's numbers read the INSPECTED
            // hull's tuning storage, which is what the device would run with if mounted here.
            if (s->fleet_pd_stock > 0 && nb < BS_RML_BAY_MAX) {
                bs_rml_bay_line& row = hud.bay[nb++];
                snprintf(row.glyph,  sizeof(row.glyph),  "P");
                snprintf(row.icon,   sizeof(row.icon),   "ic-pd");
                snprintf(row.action, sizeof(row.action), "defdrag");
                row.empty    = FALSE;
                row.selected = FALSE;
                snprintf(row.card_title, sizeof(row.card_title), "Point Defense Laser");
                {
                    // Live engagement radius from the shared resolver (one maths site with the
                    // simulation and the range ring).
                    const DefenseLaser& pdl = fs.point_defense;
                    const f32 gate = pd_gate_fraction(pdl.gate_tier);
                    const f32 pd_range = pd_resolved_range(&fs);
                    snprintf(row.card_rows, sizeof(row.card_rows),
                             "TYPE       Defense - Point defense\n"
                             "INTEGRITY  100%%\n"
                             "DPS        %.0f  (burns ordnance HP)\n"
                             "DRAIN      %.0f/s while firing\n"
                             "RANGE      %.1fk  (last-ditch x %.0f%% gate)",
                             pdl.damage_per_second, pdl.cap_drain_per_s,
                             pd_range / 1000.0f, gate * 100.0f);
                }
                snprintf(row.card_size, sizeof(row.card_size), "SIZE       Small - defense mounts");
                {
                    b8 ok = FALSE;
                    for (i32 h = 0; h < fs.hardpoint_count; ++h)
                        if (hardpoint_accepts(&fs.hardpoints[h], MODULE_TYPE_DEFENSE)) { ok = TRUE; break; }
                    row.card_size_ok = ok;
                }
                snprintf(row.card_desc, sizeof(row.card_desc),
                         "An automated last line: the beam picks its own targets and burns "
                         "incoming ordnance while the capacitor holds.");
                snprintf(row.card_mode_a, sizeof(row.card_mode_a), "AUTO BEAM - anti-ordnance");
                snprintf(row.card_mode_a_stats, sizeof(row.card_mode_a_stats),
                         "DPS %.0f  DRAIN %.0f/s", fs.point_defense.damage_per_second,
                         fs.point_defense.cap_drain_per_s);
                row.card_mode_b[0] = row.card_mode_b_stats[0] = '\0';
                snprintf(row.card_foot, sizeof(row.card_foot), "[P] cycles stance - doctrine below");
            }
            // 3) Ship modules from the pool rack (drag sources "mod:K").
            for (i32 k = 0; k < pool.module_stash_count && nb < BS_RML_BAY_MAX; ++k) {
                const ModuleDef* m = pool.module_stash[k];
                if (!m) continue;
                bs_rml_bay_line& row = hud.bay[nb++];
                snprintf(row.glyph,  sizeof(row.glyph),  "%s", m->glyph);
                snprintf(row.icon,   sizeof(row.icon),   "%s", m->icon);
                snprintf(row.action, sizeof(row.action), "mod:%d", k);
                row.empty    = FALSE;
                row.selected = FALSE;
                snprintf(row.card_title, sizeof(row.card_title), "%s", m->name);
                snprintf(row.card_rows, sizeof(row.card_rows),
                         "TYPE       Module - %s\n"
                         "INTEGRITY  100%%\n"
                         "LAYER 0    x%.2f  (proximity radius)\n"
                         "LAYER 1    x%.2f  (identification radius)\n"
                         "LAYER 2    x%.2f  (detection radius)",
                         hardpoint_kind_label(m->type),
                         m->sensor_mult[0], m->sensor_mult[1], m->sensor_mult[2]);
                {
                    static const char* MSIZE[3] = { "Small - fits S/M/L mounts",
                                                    "Medium - fits M/L mounts",
                                                    "Large - fits L mounts" };
                    snprintf(row.card_size, sizeof(row.card_size), "SIZE       %s",
                             MSIZE[(m->size < 3) ? m->size : 1]);
                    b8 ok = FALSE;
                    for (i32 h = 0; h < fs.hardpoint_count; ++h)
                        if (hardpoint_fits_module(&fs.hardpoints[h], m)) { ok = TRUE; break; }
                    row.card_size_ok = ok;
                }
                snprintf(row.card_desc, sizeof(row.card_desc), "%s", m->desc);
                if (m->type == MODULE_TYPE_SENSOR) {
                    snprintf(row.card_mode_a, sizeof(row.card_mode_a), "PASSIVE - sensor array");
                    snprintf(row.card_mode_a_stats, sizeof(row.card_mode_a_stats),
                             "RANGE x%.2f (layer 1)", m->sensor_mult[1]);
                } else {
                    snprintf(row.card_mode_a, sizeof(row.card_mode_a), "PASSIVE");
                    row.card_mode_a_stats[0] = '\0';
                }
                row.card_mode_b[0] = row.card_mode_b_stats[0] = row.card_foot[0] = '\0';
            }
            // 4) Pad to a full grid with inert empty sockets.
            while (nb < BS_RML_BAY_MAX) {
                bs_rml_bay_line& row = hud.bay[nb++];
                snprintf(row.glyph, sizeof(row.glyph), "+");
                snprintf(row.icon,  sizeof(row.icon),  "ic-empty");
                row.action[0] = '\0';
                row.empty     = TRUE;
                row.selected  = FALSE;
                row.card_title[0] = row.card_rows[0] = '\0';
                row.card_size[0] = row.card_desc[0] = '\0';
                row.card_size_ok = TRUE;
                row.card_mode_a[0] = row.card_mode_a_stats[0] = '\0';
                row.card_mode_b[0] = row.card_mode_b_stats[0] = row.card_foot[0] = '\0';
            }
            hud.bay_count = nb;
            // Fire-group assignment matrix: one column per MOUNTED weapon (hardpoint order),
            // one row per group key 1..5. Column headers are the hardpoint id up to the first
            // '_' ("bow_gun" -> "bow"); cell actions toggle membership ("gm:H:G").
            ship_groups_sanitize(&fs);
            i32 gw = 0;
            i32 gm_hp[BS_RML_GM_COLS];
            for (i32 h = 0; h < fs.hardpoint_count && gw < BS_RML_GM_COLS; ++h) {
                if (!fs.mounts[h]) continue;
                gm_hp[gw] = h;
                hardpoint_short_id(fs.hardpoints[h], hud.gm_col[gw], (i32)sizeof(hud.gm_col[gw]));
                ++gw;
            }
            hud.gm_col_count = gw;
            for (i32 g = 0; g < SHIP_WEAPON_GROUPS && g < BS_RML_GROUP_MAX; ++g) {
                bs_rml_gm_row& row = hud.gm_row[g];
                snprintf(row.label, sizeof(row.label), "%d", g + 1);
                row.selected = (g == fs.active_group);
                for (i32 i = 0; i < gw; ++i) {
                    snprintf(row.cell[i].action, sizeof(row.cell[i].action), "gm:%d:%d", gm_hp[i], g);
                    row.cell[i].on = ((fs.mount_groups[gm_hp[i]] >> g) & 1) ? TRUE : FALSE;
                }
            }
            // Point-defense doctrine chips (Phase C): stance / priority / engagement gate.
            // Shown only while the INSPECTED hull carries the device -- doctrine for a PD
            // that is not there is noise.
            hud.pd_visible  = (fs.point_defense_mount >= 0) ? TRUE : FALSE;
            hud.pd_stance   = (i32)fs.point_defense.stance;
            hud.pd_priority = (i32)fs.point_defense.priority;
            hud.pd_gate     = (i32)fs.point_defense.gate_tier;
        }
    }

    // ---- HierPos2 debug readout (top-left; dev toggle from the COORDINATE SPACE panel) ---
    if (g_debug_cell_grid) {
        const HierPos2& flag = s->player_ship().origin;
        hud.debug_visible = TRUE;
        snprintf(hud.debug_text, sizeof(hud.debug_text),
                 "HIERPOS2 GRID\nflag cell=(%lld,%lld) local=(%.1f,%.1f)\ncam  cell=(%lld,%lld)",
                 (long long)flag.cell.x, (long long)flag.cell.y, flag.local.x, flag.local.y,
                 (long long)s->camera_state.camera_hierpos.cell.x,
                 (long long)s->camera_state.camera_hierpos.cell.y);
    }

    // ---- Galaxy-map hover tooltip (cursor-anchored star-system / map-entity readout) -----
    // Computed here in the update path (camera + map entities are already finalized this frame)
    // so the RmlUi HUD consumes the snapshot the same frame it is built — no cursor lag. Skipped
    // in edit mode, matching the other in-game HUD panels. tip_left/tip_top must ALWAYS hold a
    // valid CSS length even while hidden: data-if only toggles `display`, so the element stays in
    // the DOM and its data-style-left/top bindings still evaluate every frame — an empty string
    // would log "Syntax error parsing inline property declaration 'left: ;'".
    snprintf(hud.tip_left, sizeof(hud.tip_left), "0px");
    snprintf(hud.tip_top,  sizeof(hud.tip_top),  "0px");
    if (!s->editor.edit_mode_active) {
        i32 mx = 0, my = 0;
        input_get_mouse_position(&mx, &my);
        i32 tx = 0, ty = 0;
        if (galaxy_map_hover_tooltip(s, mx, my, hud.tip_text, (i32)sizeof(hud.tip_text), &tx, &ty)) {
            hud.tip_visible = TRUE;
            snprintf(hud.tip_left, sizeof(hud.tip_left), "%dpx", tx);
            snprintf(hud.tip_top,  sizeof(hud.tip_top),  "%dpx", ty);
        }
    }

    // ---- Ship hover nameplate (above the hull: vessel name + hull class) -----------------
    // np_left/np_top carry the plate's anchor: horizontally the hull's screen-space center,
    // vertically the TOP of its bounding circle (rotation-invariant, so the plate clears the
    // hull at any heading) minus a gap -- the plate floats over the ship's upper edge rather
    // than covering the art. Clamped so a hull taller than the screen still shows the plate
    // near the top edge instead of pushing it off-screen. Both strings must ALWAYS hold a
    // valid CSS length (same rule as tip_left/tip_top above). The class line is the registry
    // card's display name (ship->def->name); a hull whose vessel name IS its class name --
    // the card-name fallback for unnamed NPC traffic -- sends "" and the plate collapses to
    // one line. RtsControls owns WHICH hull qualifies (fleet always, hostiles only once
    // identified) via hovered_nameplate_ship().
    snprintf(hud.np_left, sizeof(hud.np_left), "0px");
    snprintf(hud.np_top,  sizeof(hud.np_top),  "0px");
    if (!s->editor.edit_mode_active) {
        const Ship* np = s->rts_controls.hovered_nameplate_ship();
        if (np) {
            Vec2 npc_screen = camera2d_world_to_screen(&s->camera_state.camera,
                                                       s->fb_width, s->fb_height,
                                                       render_from_hierpos(s, &np->origin));
            // Hull's on-screen half-extent: bounding radius (world units) x zoom (px/unit).
            f32 zoom      = s->camera_state.camera.zoom;
            f32 radius_px = ship_bounding_radius(np) * (zoom > 0.0f ? zoom : 0.0f);
            // 12 px gap + 22 px half plate height (the RCSS margin-top centers the plate on
            // the anchor), so the plate's bottom edge sits 12 px above the hull's top.
            f32 anchor_y = npc_screen.y - radius_px - 34.0f;
            if (anchor_y < 40.0f) anchor_y = 40.0f;   // keep it on-screen at close zoom
            const char* np_name = np->vessel_name ? np->vessel_name : "Unnamed vessel";
            const char* np_cls  = (np->def && np->def->name[0])
                                      ? np->def->name
                                      : ship_size_class_name(np->size_class);
            hud.np_visible = TRUE;
            snprintf(hud.np_left, sizeof(hud.np_left), "%dpx", (i32)npc_screen.x);
            snprintf(hud.np_top,  sizeof(hud.np_top),  "%dpx", (i32)anchor_y);
            snprintf(hud.np_name, sizeof(hud.np_name), "%s", np_name);
            if (strcmp(np_name, np_cls) != 0)
                snprintf(hud.np_class, sizeof(hud.np_class), "%s", np_cls);
        }
    }

    // ---- Space-station interaction: cursor-anchored right-click menu + fullscreen Inspect window --
    hud.station_menu_visible = s->station_menu_visible ? TRUE : FALSE;
    snprintf(hud.station_menu_left, sizeof(hud.station_menu_left), "%dpx", s->station_menu_x);
    snprintf(hud.station_menu_top,  sizeof(hud.station_menu_top),  "%dpx", s->station_menu_y);
    // Fleet-ship context menu: validate the member each frame (the editor's "multiple ship
    // command" toggle can shrink the roster under an open menu), then mirror. Exactly one
    // action row shows: "Auto-pilot" for the hull the player is flying right now, "Pilot"
    // for everything else (including re-attach to the last-piloted hull while detached).
    if (s->ship_menu_visible &&
        (s->ship_menu_member < 0 || s->ship_menu_member >= s->fleet_state.fleet.count()))
        s->ship_menu_visible = false;
    hud.ship_menu_visible = s->ship_menu_visible ? TRUE : FALSE;
    snprintf(hud.ship_menu_left, sizeof(hud.ship_menu_left), "%dpx", s->ship_menu_x);
    snprintf(hud.ship_menu_top,  sizeof(hud.ship_menu_top),  "%dpx", s->ship_menu_y);
    if (s->ship_menu_visible) {
        const Ship& menu_ship = s->fleet_state.fleet.at(s->ship_menu_member).ship;
        snprintf(hud.ship_menu_name, sizeof(hud.ship_menu_name), "%s",
                 menu_ship.vessel_name ? menu_ship.vessel_name : "Ship");
        b8 flying_it = (s->ship_menu_member == s->fleet_state.fleet.piloted_index()) &&
                       !s->camera_state.free_camera_active;
        hud.ship_menu_can_release = flying_it;
        hud.ship_menu_can_pilot   = flying_it ? FALSE : TRUE;
        // Escort absorbed the old overlay's RMB-escort gesture: order the SELECTED ships onto
        // this hull. Shown whenever a selection exists (order_escort skips self-escort, so a
        // selection containing the target stays coherent).
        hud.ship_menu_can_escort  = s->fleet_state.fleet.any_selected() ? TRUE : FALSE;
    } else {
        hud.ship_menu_name[0]     = '\0';
        hud.ship_menu_can_pilot   = FALSE;
        hud.ship_menu_can_release = FALSE;
        hud.ship_menu_can_escort  = FALSE;
    }
    hud.station_inspector_visible = s->show_station_inspector ? TRUE : FALSE;
    {
        const char* sysname = "Unknown System";
        i32 sid = s->inspect_station_id;
        if (sid >= 0) {
            i32 node = sid >> 8;   // station_id packs (node << 8) | index
            if (node >= 0 && node < s->galaxy.node_count) sysname = s->galaxy.nodes[node].name;
        }
        snprintf(hud.station_insp_title, sizeof(hud.station_insp_title), "SPACE STATION \xE2\x80\x94 %s", sysname);

        // Subtitle: market specialization + controlling civilization (planet-inspector style).
        hud.station_insp_subtitle[0] = '\0';
        hud.station_insp_spec_agri = FALSE;
        hud.station_insp_spec_mine = FALSE;
        hud.station_insp_spec_vola = FALSE;
        hud.station_insp_spec_indu = FALSE;
        if (sid >= 0) {
            i32 node = sid >> 8;
            const char* owner_name = "Independent";
            if (node >= 0 && node < s->galaxy.node_count && s->galaxy.node_owner) {
                i16 owner = s->galaxy.node_owner[node];
                if (owner >= 0 && owner < s->galaxy.civ_count && s->galaxy.civs[owner].status == 0)
                    owner_name = s->galaxy.civs[owner].name;
            }
            i32 spec = station_specialization(s, sid);
            const char* spec_name = (spec >= 0 && spec < CAT_COUNT) ? TRADE_CATEGORY_NAMES[spec] : "General";
            hud.station_insp_spec_agri = (spec == CAT_AGRICULTURE) ? TRUE : FALSE;
            hud.station_insp_spec_mine = (spec == CAT_MINERALS)    ? TRUE : FALSE;
            hud.station_insp_spec_vola = (spec == CAT_VOLATILES)   ? TRUE : FALSE;
            hud.station_insp_spec_indu = (spec == CAT_INDUSTRIAL)  ? TRUE : FALSE;
            snprintf(hud.station_insp_subtitle, sizeof(hud.station_insp_subtitle),
                     "%s Hub  \xE2\x80\x94  %s", spec_name, owner_name);
        }

        // Tab visibility: which tabs exist for this station.
        b8 has_contracts = FALSE;
        if (sid >= 0) {
            u8 rooms[4];
            i32 rc = station_rooms(s, sid, rooms, 4);
            for (i32 r = 0; r < rc; ++r) if (rooms[r] == ROOM_CONTRACTS) { has_contracts = TRUE; break; }
        }
        hud.station_insp_tab2_visible = has_contracts;

        // Active tab (clamp to valid range).
        i32 tab = s->station_insp_tab;
        if (tab < 0) tab = 0;
        if (tab > 2) tab = 2;
        if (tab == 2 && !has_contracts) tab = 1;   // contracts tab doesn't exist: fall back
        hud.station_insp_show_dock      = (tab == 0) ? TRUE : FALSE;
        hud.station_insp_show_market    = (tab == 1) ? TRUE : FALSE;
        hud.station_insp_show_contracts = (tab == 2) ? TRUE : FALSE;

        // ---- Tab content strings (\n-separated, pre-formatted; white-space:pre in RCSS) ----
        hud.station_insp_dock[0]        = '\0';
        hud.station_insp_market_head[0] = '\0';
        hud.station_insp_market_agri[0] = '\0';
        hud.station_insp_market_mine[0] = '\0';
        hud.station_insp_market_vola[0] = '\0';
        hud.station_insp_market_indu[0] = '\0';
        hud.station_insp_market_note[0] = '\0';
        hud.station_insp_contracts[0]   = '\0';

        if (sid >= 0) {
            // DOCK tab: list missions whose current dock stage is at this station.
            // A mission docks at its origin station (MISSION_STAGE_ORIGIN_DOCK) or destination
            // station (MISSION_STAGE_MARKET_DOCK). Cooldown missions are at the origin station too.
            if (tab == 0) {
                i32 off = 0;
                #define DOCK_SNPRINTF(fmt, ...) \
                    off += snprintf(hud.station_insp_dock + off, sizeof(hud.station_insp_dock) - off, fmt, __VA_ARGS__)
                if (s->galaxy.missions) {
                    for (i32 mi = 0; mi < s->galaxy.mission_count; ++mi) {
                        const ShipMission& m = s->galaxy.missions[mi];
                        if (!m.active) continue;
                        b8 at_origin = (m.stage == MISSION_STAGE_ORIGIN_DOCK || m.stage == MISSION_STAGE_COOLDOWN)
                                       && m.station_id == sid;
                        b8 at_dest = (m.stage == MISSION_STAGE_MARKET_DOCK) && m.dest_station_id == sid;
                        if (!at_origin && !at_dest) continue;
                        const char* stage_lbl = (m.stage == MISSION_STAGE_ORIGIN_DOCK) ? "Loading"
                                              : (m.stage == MISSION_STAGE_MARKET_DOCK) ? "Unloading"
                                              : "Idle";
                        i32 other_node = at_origin ? m.dest_node : m.home_node;
                        const char* other_name = "?";
                        if (other_node >= 0 && other_node < s->galaxy.node_count)
                            other_name = s->galaxy.nodes[other_node].name;
                        if (off > 0 && off < (i32)sizeof(hud.station_insp_dock))
                            off += snprintf(hud.station_insp_dock + off, sizeof(hud.station_insp_dock) - off, "\n");
                        DOCK_SNPRINTF("Trader #%d \xE2\x80\x94 %s (route: %s)", mi, stage_lbl, other_name);
                    }
                }
                if (off == 0)
                    snprintf(hud.station_insp_dock, sizeof(hud.station_insp_dock), "No ships docked.");
                #undef DOCK_SNPRINTF
            }

            // MARKET tab: one aligned line per resource, grouped into per-category strings
            // (the RML supplies the category section heads). The last column is the local price
            // deviation from the good's galactic-average price — the actual trade signal (buy
            // below average, sell above); it folds in both local abundance bias and live stock
            // swings from AI trade, since both feed the price rule.
            if (tab == 1) {
                MarketGood gm[GOOD_COUNT];
                station_market_get(s, sid, gm);
                snprintf(hud.station_insp_market_head, sizeof(hud.station_insp_market_head),
                         "%-12s%8s%10s%8s", "", "STOCK", "PRICE", "VS AVG");
                char* bufs[CAT_COUNT]  = { hud.station_insp_market_agri, hud.station_insp_market_mine,
                                           hud.station_insp_market_vola, hud.station_insp_market_indu };
                i32 sizes[CAT_COUNT]   = { (i32)sizeof(hud.station_insp_market_agri), (i32)sizeof(hud.station_insp_market_mine),
                                           (i32)sizeof(hud.station_insp_market_vola), (i32)sizeof(hud.station_insp_market_indu) };
                i32 offs[CAT_COUNT]    = { 0, 0, 0, 0 };
                for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {
                    i32 c = trade_good_category(gd);
                    if (c < 0 || c >= CAT_COUNT) continue;
                    f32 base_p = trade_good_base_price(gd);
                    f32 dev    = (base_p > 0.0f) ? (gm[gd].price / base_p - 1.0f) * 100.0f : 0.0f;
                    char devs[8];
                    if (dev > -5.0f && dev < 5.0f) snprintf(devs, sizeof(devs), "%s", "avg");
                    else                           snprintf(devs, sizeof(devs), "%+.0f%%", dev);
                    if (offs[c] < 0 || offs[c] >= sizes[c] - 1) continue;
                    offs[c] += snprintf(bufs[c] + offs[c], (size_t)(sizes[c] - offs[c]),
                                        "%s%-12s %5.0f u   %4.0f cr  %6s",
                                        offs[c] > 0 ? "\n" : "",
                                        TRADE_GOOD_NAMES[gd], gm[gd].stock, gm[gd].price, devs);
                }
                snprintf(hud.station_insp_market_note, sizeof(hud.station_insp_market_note),
                         "VS AVG \xE2\x80\x94 local price vs galactic average\nStation revenue: %.0f cr",
                         station_revenue_get(s, sid));
            }

            // CONTRACTS tab: list missions issued by this station (station_id == sid).
            if (tab == 2 && has_contracts) {
                i32 off = 0;
                #define CON_SNPRINTF(fmt, ...) \
                    off += snprintf(hud.station_insp_contracts + off, sizeof(hud.station_insp_contracts) - off, fmt, __VA_ARGS__)
                static const char* STAGE_LABELS[] = {
                    "Loading at origin",      // MISSION_STAGE_ORIGIN_DOCK
                    "Departing origin",       // MISSION_STAGE_ACQUIRE
                    "En route to jump point", // MISSION_STAGE_TO_JUMP
                    "In transit (jumping)",   // MISSION_STAGE_JUMP
                    "Crossing system",        // MISSION_STAGE_CROSS
                    "Final approach",         // MISSION_STAGE_FINAL_APPROACH
                    "Unloading at destination",// MISSION_STAGE_MARKET_DOCK
                    "Cooldown"                // MISSION_STAGE_COOLDOWN
                };
                // Station cumulative revenue header.
                f32 rev = station_revenue_get(s, sid);
                CON_SNPRINTF("Station revenue: %.0f credits\n", rev);
                if (s->galaxy.missions) {
                    for (i32 mi = 0; mi < s->galaxy.mission_count; ++mi) {
                        const ShipMission& m = s->galaxy.missions[mi];
                        if (m.station_id != sid) continue;
                        const char* dest_name = "?";
                        if (m.dest_node >= 0 && m.dest_node < s->galaxy.node_count)
                            dest_name = s->galaxy.nodes[m.dest_node].name;
                        const char* good_name = (m.cargo_good < GOOD_COUNT) ? TRADE_GOOD_NAMES[m.cargo_good] : "?";
                        const char* stage_lbl = (m.stage < 8) ? STAGE_LABELS[m.stage] : "Unknown";
                        if (off > 0 && off < (i32)sizeof(hud.station_insp_contracts))
                            off += snprintf(hud.station_insp_contracts + off, sizeof(hud.station_insp_contracts) - off, "\n\n");
                        if (m.active) {
                            CON_SNPRINTF("Contract #%d\n  Destination : %s\n  Cargo       : %s \xE2\x80\x94 %.0f units\n  Reward      : %.0f credits\n  Status      : %s",
                                         mi, dest_name, good_name, m.cargo_units, m.reward_credits, stage_lbl);
                            if (m.return_cargo_units > 0.0f && m.return_cargo_good < GOOD_COUNT) {
                                const char* ret_name = TRADE_GOOD_NAMES[m.return_cargo_good];
                                CON_SNPRINTF("\n  Return      : %s \xE2\x80\x94 %.0f units\n  Return reward: %.0f credits",
                                             ret_name, m.return_cargo_units, m.return_reward);
                            }
                        } else {
                            CON_SNPRINTF("Contract #%d\n  Destination : %s\n  Status      : Cooldown (%.1fh to new contract)",
                                         mi, dest_name, m.respawn_hours);
                        }
                    }
                }
                if (off == 0)
                    snprintf(hud.station_insp_contracts, sizeof(hud.station_insp_contracts), "No contracts available.");
                #undef CON_SNPRINTF
            }
        }
    }

    // ---- Planet inspector window (left-click a planet on the galaxy map) -----------------
    // Selection is (system galaxy_center, planet index): resolve the cached slot by identity
    // each frame and auto-close if the system left the materialisation cache. Every value gets
    // its qualitative label HERE (game-side) — the engine data model only displays strings.
    // The gauge width strings must ALWAYS hold valid CSS (same caveat as tip_left/tip_top).
    snprintf(hud.planet_insp_hab_w, sizeof(hud.planet_insp_hab_w), "0%%");
    snprintf(hud.planet_insp_haz_w, sizeof(hud.planet_insp_haz_w), "0%%");
    if (s->show_planet_inspector && s->planet_insp_planet >= 0) {
        i32 pslot = -1;
        for (i32 si = 0; si < s->galaxy.system_count; ++si) {
            const bs_math::HierPos2& gc = s->galaxy.systems[si].galaxy_center;
            if (gc.cell.x == s->planet_insp_center.cell.x && gc.cell.y == s->planet_insp_center.cell.y &&
                gc.local.x == s->planet_insp_center.local.x && gc.local.y == s->planet_insp_center.local.y) {
                pslot = si;
                break;
            }
        }
        if (pslot < 0 || s->planet_insp_planet >= s->galaxy.systems[pslot].planet_count) {
            s->show_planet_inspector = false;   // system evicted from the cache (player flew away)
        } else {
            const StarSystem& ss = s->galaxy.systems[pslot];
            i32 pi = s->planet_insp_planet;
            const PlanetProperties& pp = ss.planet_props[pi];
            const EvolvedBody& eb = ss.evo.bodies[1 + pi];
            b8 giant = eb.comp.gas > 0.35f;
            hud.planet_insp_visible = TRUE;

            static const char* ROMAN[MAX_SYSTEM_PLANETS] = { "I", "II", "III", "IV", "V", "VI" };
            snprintf(hud.planet_insp_title, sizeof(hud.planet_insp_title), "%s %s",
                     ss.name ? ss.name : "Unknown", ROMAN[pi]);

            // Classification + trait tags (skip a tag that just repeats the subtype name).
            {
                const char* subtype = planet_subtype_name(pp.type, pp.genome.subtype);
                i32 off = snprintf(hud.planet_insp_subtitle, sizeof(hud.planet_insp_subtitle), "%s %s",
                                   subtype, planet_type_name(pp.type));
                const char* tags[3];
                i32 nt = planet_trait_names(pp.genome.trait_bits, tags, 3);
                for (i32 t = 0; t < nt && off > 0 && off < (i32)sizeof(hud.planet_insp_subtitle) - 1; ++t) {
                    if (strcmp(tags[t], subtype) == 0) continue;
                    off += snprintf(hud.planet_insp_subtitle + off, sizeof(hud.planet_insp_subtitle) - (size_t)off,
                                    "  [%s]", tags[t]);
                }
            }

            // Gameplay gauges: habitability + hazard.
            i32 habp = (i32)(eb.habitability * 100.0f + 0.5f);
            i32 hazp = (i32)(eb.env_hazard * 100.0f + 0.5f);
            snprintf(hud.planet_insp_hab_w, sizeof(hud.planet_insp_hab_w), "%d%%", habp);
            snprintf(hud.planet_insp_haz_w, sizeof(hud.planet_insp_haz_w), "%d%%", hazp);
            snprintf(hud.planet_insp_hab_label, sizeof(hud.planet_insp_hab_label), "Habitability  %d%%", habp);
            const char* hazl = hazp < 25 ? "Low" : hazp < 50 ? "Moderate" : hazp < 75 ? "High" : "Extreme";
            snprintf(hud.planet_insp_haz_label, sizeof(hud.planet_insp_haz_label), "Hazard  %s (%d%%)", hazl, hazp);

            // Orbit / physical / environment stats (each with its qualitative label).
            {
                f32 grav = eb.radius_earth > 0.01f ? eb.mass_earth / (eb.radius_earth * eb.radius_earth) : 0.0f;
                const char* eccl = eb.eccentricity < 0.05f ? "Circular" : eb.eccentricity < 0.15f ? "Mild" : "Eccentric";
                const char* atml = giant ? "Bottomless"
                                 : eb.atmo_pressure < 0.01f ? "None"
                                 : eb.atmo_pressure < 0.1f  ? "Trace"
                                 : eb.atmo_pressure < 0.5f  ? "Thin"
                                 : eb.atmo_pressure < 2.0f  ? "Moderate"
                                 : eb.atmo_pressure < 10.0f ? "Dense" : "Crushing";
                const char* magl = eb.magnetic_field < 0.25f ? "Weak" : eb.magnetic_field < 0.6f ? "Moderate" : "Strong";
                f32 geo = eb.tectonics > eb.volcanism ? eb.tectonics : eb.volcanism;
                const char* geol = geo < 0.1f ? "Dead" : geo < 0.35f ? "Quiet" : geo < 0.7f ? "Active" : "Violent";
                const char* biol = eb.life < 0.05f ? "Sterile" : eb.life < 0.4f ? "Microbial" : eb.life < 0.8f ? "Complex" : "Flourishing";
                i32 off = snprintf(hud.planet_insp_stats, sizeof(hud.planet_insp_stats),
                    "Orbit        %.2f AU  (%s)\n"
                    "Mass         %.2f Me    Radius  %.2f Re\n"
                    "Temperature  %.0f K  (%.0f C)\n"
                    "Gravity      %.2f g\n"
                    "Atmosphere   %s (%.2f atm)",
                    eb.orbit_au, eccl, eb.mass_earth, eb.radius_earth,
                    eb.temperature_k, eb.temperature_k - 273.15f, grav,
                    atml, eb.atmo_pressure);
                if (!giant && off > 0 && off < (i32)sizeof(hud.planet_insp_stats) - 1)
                    off += snprintf(hud.planet_insp_stats + off, sizeof(hud.planet_insp_stats) - (size_t)off,
                        "\nWater        %.0f%% coverage", eb.water_frac * 100.0f);
                if (off > 0 && off < (i32)sizeof(hud.planet_insp_stats) - 1)
                    off += snprintf(hud.planet_insp_stats + off, sizeof(hud.planet_insp_stats) - (size_t)off,
                        "\nMagnetic     %s", magl);
                if (!giant && off > 0 && off < (i32)sizeof(hud.planet_insp_stats) - 1)
                    off += snprintf(hud.planet_insp_stats + off, sizeof(hud.planet_insp_stats) - (size_t)off,
                        "\nGeology      %s\nBiosphere    %s", geol, biol);
            }

            // Resources + bulk-composition survey line (mining gameplay).
            {
                const char* rml = eb.res_metal < 0.2f ? "Poor" : eb.res_metal < 0.45f ? "Moderate" : eb.res_metal < 0.7f ? "Rich" : "Abundant";
                const char* rvl = eb.res_volatiles < 0.2f ? "Poor" : eb.res_volatiles < 0.45f ? "Moderate" : eb.res_volatiles < 0.7f ? "Rich" : "Abundant";
                snprintf(hud.planet_insp_comp, sizeof(hud.planet_insp_comp),
                    "Metals       %s (%.0f%%)\n"
                    "Volatiles    %s (%.0f%%)\n"
                    "Survey       %.0f%% metal  %.0f%% rock  %.0f%% ice  %.0f%% gas",
                    rml, eb.res_metal * 100.0f, rvl, eb.res_volatiles * 100.0f,
                    eb.comp.metal * 100.0f, eb.comp.silicate * 100.0f,
                    eb.comp.ice * 100.0f, eb.comp.gas * 100.0f);
            }

            // Moons of this planet (evo parent links).
            {
                hud.planet_insp_moons[0] = '\0';
                i32 moon_base = 1 + ss.evo.planet_count;
                i32 moff = 0, letter = 0;
                for (i32 m = 0; m < ss.evo.moon_count; ++m) {
                    const EvolvedBody& mb = ss.evo.bodies[moon_base + m];
                    if (mb.parent != (i8)(1 + pi)) continue;
                    if (moff < 0 || moff >= (i32)sizeof(hud.planet_insp_moons) - 1) break;
                    moff += snprintf(hud.planet_insp_moons + moff, sizeof(hud.planet_insp_moons) - (size_t)moff,
                        "%s%s-%c   %s  %.0f K%s",
                        moff > 0 ? "\n" : "", ROMAN[pi], (char)('a' + letter),
                        planet_type_name(mb.type), mb.temperature_k,
                        mb.volcanism > 0.5f ? "  (tidal volcanism)" : "");
                    ++letter;
                }
            }

            // Chronicle: this body's evolution events + system-wide ones (disk dispersal).
            {
                hud.planet_insp_events[0] = '\0';
                i32 eoff = 0;
                for (i32 e = 0; e < ss.evo.event_count; ++e) {
                    const EvolutionEvent& ev = ss.evo.events[e];
                    if (ev.body != (i8)(1 + pi) && ev.other != (i8)(1 + pi) && ev.body != -1) continue;
                    if (eoff < 0 || eoff >= (i32)sizeof(hud.planet_insp_events) - 1) break;
                    eoff += snprintf(hud.planet_insp_events + eoff, sizeof(hud.planet_insp_events) - (size_t)eoff,
                        "%sEpoch %-2d  %s", eoff > 0 ? "\n" : "", (i32)ev.epoch, evo_event_name(ev.kind));
                }
            }
        }
    }

    bs_rml_hud_update(&hud);

    // ---- Drain the button clicks the document produced this frame -----------------------
    char action[BS_RML_ACTION_CAP];
    while (bs_rml_hud_poll_action(action, sizeof(action)) > 0) {
        if (strcmp(action, "close_discoveries") == 0) {
            s->show_discoveries = false;
            continue;
        }
        // Time control: "time:M" sets the speed multiplier directly (0=pause, 1/3/5/10 = tiers).
        // Parse the full integer after the prefix so multi-digit tiers (e.g. "time:10") work.
        if (strncmp(action, "time:", 5) == 0) {
            i32 mult = 0;
            for (const char* p = action + 5; *p >= '0' && *p <= '9'; ++p) mult = mult * 10 + (*p - '0');
            s->time_scale = (f32)mult;
            if (mult == 0) action_log_push(s, "Game paused.");
            else           action_log_push(s, "Speed set to %dx.", mult);
            continue;
        }
        // Fleet ship panel: release the piloted hull back to auto-pilot. RELEASE-ONLY now --
        // the button is hidden while detached; piloting is entered via the ship context menu.
        if (strcmp(action, "fleet_mode") == 0) {
            s->rts_controls.release_to_autopilot();
            continue;
        }
        // Fleet ship panel: fire-group chip click selects that weapon group (same feedback as
        // the number keys).
        if (strncmp(action, "group:", 6) == 0) {
            i32 g = atoi(action + 6);
            FleetShip* p = s->fleet_state.fleet.piloted();
            if (!p) p = &s->fleet_state.fleet.at(0);
            if (p && g >= 0 && g < SHIP_WEAPON_GROUPS) {
                ship_groups_sanitize(&p->ship);
                ship_select_weapon_group(&p->ship, g);
                i32 n = ship_group_size(&p->ship, g);
                if (n > 0) action_log_push(s, "Weapon group %d selected (%d weapon%s).", g + 1, n, n == 1 ? "" : "s");
                else       action_log_push(s, "Weapon group %d is empty.", g + 1);
            }
            continue;
        }
        // Fleet roster: row click selects that ship. Shift is read at drain time (the same
        // frame as the click) for additive toggle -- the engine carries no modifier state.
        if (strncmp(action, "fsel:", 5) == 0) {
            i32 i = atoi(action + 5);
            Fleet& fleet = s->fleet_state.fleet;
            if (i >= 0 && i < fleet.count()) {
                if (input_is_key_down(KEY_LSHIFT) || input_is_key_down(KEY_RSHIFT)) {
                    fleet.set_selected(i, fleet.is_selected(i) ? FALSE : TRUE);
                } else {
                    fleet.clear_selection();
                    fleet.set_selected(i, TRUE);
                }
            }
            continue;
        }
        // Fleet roster stance chip: acts on the whole selection when the clicked ship is part
        // of it, and on that ship alone otherwise. That keeps "set the wing defensive" one
        // click without a stray click on an unselected row silently retuning the whole fleet.
        if (strncmp(action, "fstance:", 8) == 0) {
            i32 i = atoi(action + 8);
            const char* colon = strchr(action + 8, ':');
            i32 st = colon ? atoi(colon + 1) : -1;
            Fleet& fleet = s->fleet_state.fleet;
            if (i >= 0 && i < fleet.count() && st >= 0 && st < 4) {
                static const char* const STANCE_NAME[4] =
                    { "AGGRESSIVE", "DEFENSIVE", "PASSIVE", "HOLD FIRE" };
                if (fleet.is_selected(i)) fleet.set_selected_stance((u8)st);
                else                      fleet.at(i).stance = (u8)st;
                action_log_push(s, "%s: %s",
                                fleet.at(i).ship.vessel_name ? fleet.at(i).ship.vessel_name : "Ship",
                                STANCE_NAME[st]);
            }
            continue;
        }
        // Fleet roster ROE cycling chip ("froe:R"): weapons free -> return fire -> hold ->
        // free. Same selection semantics as the stance chips: the whole selection when the
        // clicked ship is part of it, that ship alone otherwise.
        if (strncmp(action, "froe:", 5) == 0) {
            i32 i = atoi(action + 5);
            Fleet& fleet = s->fleet_state.fleet;
            if (i >= 0 && i < fleet.count()) {
                u8 next = (u8)((fleet.at(i).roe + 1) % ((u8)ROE_HOLD + 1));
                static const char* const ROE_NAME[3] =
                    { "Weapons free", "Return fire", "Hold (designate only)" };
                if (fleet.is_selected(i)) fleet.set_selected_roe(next);
                else                      fleet.at(i).roe = next;
                action_log_push(s, "%s ROE: %s",
                                fleet.at(i).ship.vessel_name ? fleet.at(i).ship.vessel_name : "Ship",
                                ROE_NAME[next]);
            }
            continue;
        }
        // Fire-group matrix checkbox: toggle INSPECTED-ship weapon (hardpoint H) in group G.
        // Refuses to orphan a weapon from all groups.
        if (strncmp(action, "gm:", 3) == 0) {
            i32 h = atoi(action + 3);
            const char* colon = strchr(action + 3, ':');
            i32 g = colon ? atoi(colon + 1) : -1;
            Ship& fsh = s->inspected_ship();
            if (h >= 0 && h < fsh.hardpoint_count && g >= 0 && g < SHIP_WEAPON_GROUPS &&
                fsh.mounts[h]) {
                Weapon* w = fsh.mounts[h];
                const char* wname = w->name ? w->name : "Weapon";
                u8 toggled = fsh.mount_groups[h] ^ (u8)(1u << g);
                if (!toggled) {
                    action_log_push(s, "%s must belong to at least one group.", wname);
                } else {
                    fsh.mount_groups[h] = toggled;
                    // Re-assert the mask invariants: dropping this weapon out of the ACTIVE
                    // group must also drop a micro-selection override aimed at it, or it would
                    // go on firing with no hub tile left to report it.
                    ship_groups_sanitize(&fsh);
                    action_log_push(s, "%s ('%s') %s group %d.", wname, fsh.hardpoints[h].id,
                                    (toggled >> g) & 1 ? "added to" : "removed from", g + 1);
                }
            }
            continue;
        }
        // Ship inspector: toggle/close the window. The bottom-center button inspects the
        // PILOTED ship (the hull the player is flying); the roster's per-row "i" buttons
        // arrive as "insp:N" below and pick any member.
        if (strcmp(action, "toggle_inspector") == 0) {
            s->show_flagship_inspector = !s->show_flagship_inspector;
            if (s->show_flagship_inspector) {
                s->inspected_ship_idx = s->fleet_state.fleet.piloted_index();
                inspector_view_reset(s);
            }
            continue;
        }
        if (strcmp(action, "close_inspector") == 0) {
            s->show_flagship_inspector = false;
            continue;
        }
        // Inspector middle-section tabs: 0 = Loadout (status), 1 = Doctrine (groups + PD).
        if (strncmp(action, "insp_tab:", 9) == 0) {
            s->insp_tab = (action[9] == '1') ? 1 : 0;
            continue;
        }
        // Roster inspect button: open the full-screen inspector on fleet member N. When the
        // free camera is up, snap it onto that hull so the inspected ship is the one on screen
        // (the center zone is the world -- phase 2's portrait replaces this with an offscreen
        // render and the snap can go).
        if (strncmp(action, "insp:", 5) == 0) {
            i32 i = atoi(action + 5);
            if (i >= 0 && i < s->fleet_state.fleet.count()) {
                if (i != s->inspected_ship_idx || !s->show_flagship_inspector)
                    inspector_view_reset(s);
                s->inspected_ship_idx      = i;
                s->show_flagship_inspector = true;
                if (s->camera_state.free_camera_active && !s->camera_state.recentering)
                    s->camera_state.free_camera_pos = s->fleet_state.fleet.at(i).ship.origin;
            }
            continue;
        }
        if (strcmp(action, "station_inspect") == 0) {
            s->inspect_station_id      = s->station_menu_station_id;
            s->show_station_inspector  = true;
            s->station_menu_visible    = false;
            s->station_insp_tab        = 0;   // default to Dock tab
            continue;
        }
        if (strcmp(action, "close_station_inspector") == 0) {
            s->show_station_inspector = false;
            continue;
        }
        // Ship context menu: the two directed piloting moves. The target is resolved from the
        // menu's own state (ship_menu_member), the same pattern the station menu uses -- the
        // action string stays FIXED so the RML rows are static markup.
        if (strcmp(action, "ship_pilot") == 0) {
            s->rts_controls.pilot_ship(s->ship_menu_member);
            s->ship_menu_visible = false;
            continue;
        }
        if (strcmp(action, "ship_autopilot") == 0) {
            s->rts_controls.release_to_autopilot();
            s->ship_menu_visible = false;
            continue;
        }
        if (strcmp(action, "ship_escort") == 0) {
            Fleet& esc_fleet = s->fleet_state.fleet;
            if (s->ship_menu_member >= 0 && s->ship_menu_member < esc_fleet.count()) {
                Ship* buddy = &esc_fleet.at(s->ship_menu_member).ship;
                esc_fleet.order_escort(buddy);
                action_log_push(s, "Escorting: %s", buddy->vessel_name ? buddy->vessel_name : "ship");
            }
            s->ship_menu_visible = false;
            continue;
        }
        if (strcmp(action, "close_planet_inspector") == 0) {
            s->show_planet_inspector = false;
            continue;
        }
        if (strncmp(action, "station_tab:", 12) == 0) {
            s->station_insp_tab = action[12] - '0';
            continue;
        }
        // Modules-bay drag source armed on dragstart. Bay sources: "inv:K" = an unmounted weapon
        // in the loadout stash, "defdrag" = the unmounted point-defense, "mod:K" = a ship module
        // in the rack. Kinds 0/3/5 (mounted weapon / mounted PD / installed module) are armed by
        // the ship-side pick-up in game_update instead. The armed source resolves on: "dragend"
        // (world hardpoint hit-test -> arsenal_drop_on_slot), "baydrop" (unmount into the bay),
        // or "uidrop" (window dead space -> disarm).
        if (strncmp(action, "inv:", 4) == 0) {
            s->pending_weapon_drag      = atoi(action + 4);
            s->pending_weapon_drag_kind = 1;   // unmounted offensive weapon (stash)
            continue;
        }
        if (strncmp(action, "mod:", 4) == 0) {
            s->pending_weapon_drag      = atoi(action + 4);
            s->pending_weapon_drag_kind = 4;   // unmounted ship module (rack)
            continue;
        }
        if (strcmp(action, "defdrag") == 0) {
            s->pending_weapon_drag      = 0;   // src unused for the point-defense
            s->pending_weapon_drag_kind = 2;   // unmounted point-defense (defensive inventory)
            continue;
        }
        // Point-defense doctrine chips (Phase C): "pd:stance:N" / "pd:pri:N" / "pd:gate:N".
        // Targets the INSPECTED ship -- each hull owns its own PD device and doctrine.
        if (strncmp(action, "pd:", 3) == 0) {
            DefenseLaser& pd = s->inspected_ship().point_defense;
            if (strncmp(action + 3, "stance:", 7) == 0) {
                i32 v = atoi(action + 10);
                if (v >= 0 && v <= 2) {
                    pd.stance = (u8)v;
                    static const char* PD_STANCE_NAMES[3] = { "HOLD", "STANDARD", "OVERDRIVE" };
                    action_log_push(s, "PD stance: %s", PD_STANCE_NAMES[v]);
                }
            } else if (strncmp(action + 3, "pri:", 4) == 0) {
                i32 v = atoi(action + 7);
                if (v >= 0 && v <= 2) {
                    pd.priority = (u8)v;
                    static const char* PD_PRI_NAMES[3] = { "impact time", "missiles first", "nearest" };
                    action_log_push(s, "PD priority: %s", PD_PRI_NAMES[v]);
                }
            } else if (strncmp(action + 3, "gate:", 5) == 0) {
                i32 v = atoi(action + 8);
                if (v >= 0 && v <= 2) {
                    pd.gate_tier = (u8)v;
                    static const char* PD_GATE_NAMES[3] = { "60%", "80%", "100%" };
                    action_log_push(s, "PD engagement gate: %s range", PD_GATE_NAMES[v]);
                }
            }
            continue;
        }
        // Engagement doctrine chips (Phase 3): "doc:roe:N" / "doc:flak:N" / "doc:mp:N" /
        // "doc:capfloor:N". Targets the INSPECTED ship, same convention as the PD trio --
        // the inspector is the per-hull doctrine surface; the roster chips act on selections.
        if (strncmp(action, "doc:", 4) == 0) {
            FleetShip& ifs = s->fleet_state.fleet.at(s->inspected_ship_idx);
            const char* ship_name = ifs.ship.vessel_name ? ifs.ship.vessel_name : "Ship";
            if (strncmp(action + 4, "roe:", 4) == 0) {
                i32 v = atoi(action + 8);
                if (v >= 0 && v <= (i32)ROE_HOLD) {
                    ifs.roe = (u8)v;
                    static const char* N[3] = { "Weapons free", "Return fire", "Hold" };
                    action_log_push(s, "%s ROE: %s", ship_name, N[v]);
                }
            } else if (strncmp(action + 4, "flak:", 5) == 0) {
                i32 v = atoi(action + 9);
                if (v >= 0 && v <= (i32)FLAK_MANUAL) {
                    ifs.flak_doctrine = (u8)v;
                    action_log_push(s, "%s flak screen: %s", ship_name, v == 0 ? "AUTO" : "MANUAL");
                }
            } else if (strncmp(action + 4, "mp:", 3) == 0) {
                i32 v = atoi(action + 7);
                if (v >= 0 && v <= (i32)MISSILE_HOLD) {
                    ifs.missile_policy = (u8)v;
                    static const char* N[3] = { "free", "conserve", "hold" };
                    action_log_push(s, "%s missiles: %s", ship_name, N[v]);
                }
            } else if (strncmp(action + 4, "capfloor:", 9) == 0) {
                i32 v = atoi(action + 13);
                if (v >= 0 && v <= 3) {
                    static const f32 TIER[4] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    ifs.cap_fire_floor = TIER[v];
                    action_log_push(s, "%s gun cap floor: %.0f%%", ship_name, TIER[v] * 100.0f);
                }
            }
            continue;
        }
        // Speed-limit gear chips ("spd:N"): set the chosen governor gear (ship_speed_cap).
        // Attached, it throttles the hull being flown (the panel's hull). Detached it
        // targets the SELECTION -- the same surface RMB orders use -- falling back to the
        // panel's hull when nothing is selected, so the chips never click into a void.
        if (strncmp(action, "spd:", 4) == 0) {
            i32 gear = atoi(action + 4);
            Fleet& fleet = s->fleet_state.fleet;
            FleetShip* panel_hull = fleet.piloted();
            if (!panel_hull && fleet.count() > 0) panel_hull = &fleet.at(0);
            b8 use_selection = s->camera_state.free_camera_active && fleet.any_selected();
            i32 applied = 0;
            f32 cap_val = 0.0f;
            const char* one_name = nullptr;
            for (i32 i = 0; i < fleet.count(); ++i) {
                FleetShip& fs = fleet.at(i);
                b8 hit = use_selection ? fleet.is_selected(i) : (&fs == panel_hull);
                if (!hit) continue;
                const ShipDef* d = fs.ship.def;
                if (!d || d->speed_limit_count <= 0) continue;
                i32 g = gear;
                if (g < 0) g = 0;
                if (g >= d->speed_limit_count) g = d->speed_limit_count - 1;
                fs.ship.speed_limit_sel = g;
                cap_val  = d->speed_limits[g];
                one_name = fs.ship.vessel_name;
                ++applied;
            }
            if (applied == 1 && one_name)
                action_log_push(s, "%s: speed limit %d - %.0f u/s", one_name, gear + 1, cap_val);
            else if (applied > 1)
                action_log_push(s, "Speed limit %d set - %d ships", gear + 1, applied);
            continue;
        }
        // Legacy per-tray drop actions removed: the unified Modules bay emits "baydrop" (below).
        // Unified Modules-bay drop: UNMOUNT whatever mounted item was dragged in, routed by the
        // armed drag's kind (0 = mounted weapon, 3 = mounted point-defense, 5 = mounted module).
        // Drops from bay sources onto the bay itself are no-ops (kind 1/2/4 fall through).
        if (strcmp(action, "baydrop") == 0) {
            Ship& fs   = s->inspected_ship();   // unmount FROM the inspected hull...
            Ship& pool = s->player_ship();      // ...INTO the fleet-wide pool
            i32 src  = s->pending_weapon_drag;
            i32 kind = s->pending_weapon_drag_kind;
            if (kind == 0 && src >= 0 && src < fs.hardpoint_count && fs.mounts[src]) {
                Weapon* w = fs.mounts[src];
                fs.mounts[src] = nullptr;
                ship_stash_append(pool, w);
                ship_rehome_weapons(fs);
                action_log_push(s, "%s returned to the module bay.", w->name ? w->name : "Weapon");
            } else if (kind == 3 && fs.point_defense_mount >= 0) {
                i32 slot = fs.point_defense_mount;
                fs.point_defense_mount   = -1;
                fs.point_defense.enabled = FALSE;
                ++s->fleet_pd_stock;                 // device back into the fleet pool
                ship_rehome_weapons(fs);
                action_log_push(s, "Point Defense Laser returned to the module bay (was '%s').",
                                fs.hardpoints[slot].id);
            } else if (kind == 5 && src >= 0 && src < fs.hardpoint_count && fs.module_mounts[src]) {
                const ModuleDef* m = fs.module_mounts[src];
                if (pool.module_stash_count >= SHIP_MAX_MODULES) {
                    action_log_push(s, "Module rack full - can't remove '%s'.", m->name);
                } else {
                    fs.module_mounts[src] = nullptr;
                    ship_module_stash_append(pool, m);
                    ship_recompute_stats(&fs);
                    action_log_push(s, "%s returned to the module bay.", m->name);
                }
            }
            s->pending_weapon_drag = -1;
            continue;
        }
        // Catch-all drop on the inspector panel itself (dead space between real targets): swallow
        // the drag so it can't fall through to a world hardpoint behind the window. Drops on real
        // targets bubble here AFTER their own handler ran - the extra disarm is harmless.
        if (strcmp(action, "uidrop") == 0) {
            s->pending_weapon_drag = -1;
            continue;
        }
        // Drag released (fires on the drag source after any dragdrop). If a window drop target
        // already consumed the armed drag, pending_weapon_drag is -1 and this is a no-op. Otherwise
        // hit-test the release point against the flagship's world hardpoint boxes so modules can be
        // dropped straight onto the cruiser. NOTE: bs_rml_wants_mouse() is unusable here - RmlUi
        // reports the cursor as interacting with UI for the whole drag (the clone rides under it),
        // so releases over the inspector's dead space are instead swallowed by the panel's own
        // catch-all "uidrop" dragdrop (queued before this dragend).
        if (strcmp(action, "dragend") == 0) {
            if (s->pending_weapon_drag >= 0 && !bs_imgui_wants_mouse()) {
                i32 hit = inspected_hardpoint_at_cursor(s);
                if (hit >= 0) arsenal_drop_on_slot(s, hit);
                else          action_log_push(s, "No hardpoint there - drag cancelled.");
            }
            s->pending_weapon_drag = -1;   // disarm on any unconsumed release
            continue;
        }
        const char* msg = nullptr;
        if      (strcmp(action, "engage")  == 0) msg = "Engage selected.";
        else if (strcmp(action, "avoid")   == 0) msg = "Avoid selected.";
        else if (strcmp(action, "hail")    == 0) msg = "Hail selected.";
        else if (strcmp(action, "observe") == 0) msg = "Observe selected.";
        if (msg) {
            s->encounter_active = FALSE;
            action_log_push(s, "%s", msg);
        }
    }
}

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
        f32 acq_ms = 0.0f, sub_ms = 0.0f;
        renderer_get_present_breakdown(&acq_ms, &sub_ms);
        s->profiler.set_present_breakdown(acq_ms, sub_ms);
    }

    BS_PROFILE(&s->profiler, PROF_UPDATE_TOTAL);

    // Phase A: while in the New Game setup screen or the staged generation, run only the generation
    // step (if any) and skip the entire gameplay update — the galaxy does not exist yet.
    if (s->app_phase != APP_PLAYING) {
        if (s->app_phase == APP_GENERATING) run_generation_stage(s);
        return TRUE;
    }

    if (dt > 0.05f) dt = 0.05f; // clamp hitches

    f32 sim_dt = dt * s->time_scale;

    if (sim_dt > 0.05f) sim_dt = 0.05f; // still clamp scaled hitches

    // Advance the shared in-game calendar: 1 real second = 1 in-game hour at 1x (scaled by time_scale
    // through sim_dt). This single clock drives both local gameplay and the galaxy history/news.
    s->sim_hours += (f64)sim_dt;

    // Age the cosmetic projectile-FX ring. Deliberately EARLY: the fire path below and the
    // collision pass further down both emit into it, and an event aged in the same frame it
    // was emitted would lose a whole frame of a 75 ms muzzle flash before it is ever drawn.
    // Runs on sim_dt, not dt, so effects slow with the time scale like the world they depict.
    s->projectile_fx.update(sim_dt);

    galaxy_history_live_tick(s, sim_dt);   // Phase C1: the galaxy lives on the shared calendar clock
    ship_missions_update(s, sim_dt);       // Cross-system Ship AI: advance macro travelers along lanes

    // Feature B: the local patrol serves the OWNER of the player's current system (the flagship's
    // nearest node). In wild / unclaimed space it defaults to pirates. Each frame we tag the patrol
    // hull with that faction, label it, and resolve hostility per-faction (folds transitive stance).
    {
        i32 pnode  = galaxy_nearest_node(s, &s->fleet_state.fleet.flagship().ship.origin);
        i32 powner = galaxy_history_owner_at_node(s, pnode);
        s->galaxy.current_owner_civ = (i16)powner;

        i16 pfac = (powner >= 0) ? (i16)powner : FACTION_PIRATE;
        s->fleet_state.enemy_ship.faction_id = pfac;
        galaxy_history_faction_label(s, pfac, s->fleet_state.patrol_name,
                                     (i32)sizeof(s->fleet_state.patrol_name));
        s->fleet_state.enemy_ship.vessel_name = s->fleet_state.patrol_name;
        s->galaxy.current_hostile = galaxy_history_faction_is_hostile(s, pfac);
    }

    s->profiler.begin(PROF_OUT_SENSOR_FX);

    s->out_sensor_fx.update(dt);

    s->profiler.end(PROF_OUT_SENSOR_FX);

    // ---- Encounter detection: blob merge (sim/combat_arena.cpp) --------------------------
    combat_arena_update_encounter(s);

    s->galaxy.galaxy_map_time += sim_dt;

    // ---- SHIFT key: toggle alternative mouse-follow movement system in global mode -----------

    // Mouse-follow turning is a PILOTING control, so it follows the camera being attached, not the
    // render look. The `view.mode == MODE_GLOBAL` term it used to carry was implied by
    // !free_camera_active back when the zoom crossing force-detached; now that piloting persists at
    // any zoom, keeping it would have made the toggle silently dead below ZOOM_MIN.
    //
    // DEFERRED TO RELEASE: shift is also the avoid-order MODIFIER (shift-RMB on a hostile), and
    // with the order grammar live in both control modes a press-edge toggle flipped the flight
    // scheme on every avoid issued mid-pilot -- the long-standing shift-RMB/LSHIFT collision.
    // So the press only ARMS the toggle, any RMB during the hold cancels it (that shift was a
    // modifier, not a command), and the release commits. A plain tap behaves as before, just on
    // the release edge.
    {
        static b8 lshift_toggle_pending = FALSE;
        if (input_is_key_down(KEY_LSHIFT) && !input_was_key_down(KEY_LSHIFT) &&
            !s->camera_state.free_camera_active && !s->show_flagship_inspector)
            lshift_toggle_pending = TRUE;
        if (input_is_button_down(BUTTON_RIGHT) && !input_was_button_down(BUTTON_RIGHT))
            lshift_toggle_pending = FALSE;
        if (!input_is_key_down(KEY_LSHIFT) && input_was_key_down(KEY_LSHIFT)) {
            if (lshift_toggle_pending && !s->camera_state.free_camera_active &&
                !s->show_flagship_inspector) {
                s->view.alt_movement_active = !s->view.alt_movement_active;
                if (s->view.alt_movement_active)
                    action_log_push(s, "Alternative movement system activated.");
                else
                    action_log_push(s, "Alternative movement system deactivated.");
            }
            lshift_toggle_pending = FALSE;
        }
    }

    // ---- R key: toggle radiation detector overlay in global mode -------------------------

    if (input_is_key_down(KEY_R) && !input_was_key_down(KEY_R) && s->view.mode == MODE_GLOBAL &&
        !s->show_flagship_inspector) {

        s->show_metaball_ui = !s->show_metaball_ui;

        action_log_push(s, "Radiation detector %s.", s->show_metaball_ui ? "ON" : "OFF");

    }

    // ---- V key: toggle the flagship's three-layer sensor rings in global mode -----------

    if (input_is_key_down(KEY_V) && !input_was_key_down(KEY_V) && s->view.mode == MODE_GLOBAL &&
        !s->show_flagship_inspector) {

        s->show_sensor_layers = !s->show_sensor_layers;

        action_log_push(s, "Sensor layers %s.", s->show_sensor_layers ? "ON" : "OFF");

    }

    // ---- ESC: collapse the ship inspector; otherwise quit the application. --------------
    // The engine's hardcoded ESC-quit moved HERE (core/application.cpp no longer fires it)
    // so the key can be modal-aware: the press that collapses the inspector is CONSUMED by
    // it, and only a separate, later press quits. Edge-triggered, so one physical press can
    // never both collapse and quit -- no debounce timer needed. First game-side use of the
    // engine's exported event bus (event_fire).
    if (input_is_key_down(KEY_ESCAPE) && !input_was_key_down(KEY_ESCAPE)) {
        if (s->show_flagship_inspector) {
            s->show_flagship_inspector = false;
        } else {
            event_context quit_ctx = {};
            event_fire(EVENT_CODE_APPLICATION_QUIT, 0, quit_ctx);
        }
    }

    // ---- SPACE key: RETIRED (was the command overlay toggle). Selection, the roster and the
    // full RMB order grammar are always live now, and the 0.25x tactical slow is gone with the
    // overlay -- deliberate pacing is the time-tier buttons (Pause / 1x / 3x / 5x / 10x). The
    // key is free for a future binding. ------------------------------------------------------

    // The fleet roster is an RmlUi HUD panel now: its clicks arrive as "fsel:"/"fstance:"
    // action strings in the drain above, and bs_rml_wants_mouse() reports the cursor as
    // UI-owned over it, which is what keeps a roster click from also reaching the world.

    // ---- TAB key: RETIRED. The pilot <-> auto-pilot/RTS mode TOGGLE is gone: auto-pilot/RTS
    // is the default mode, piloting is entered per-ship via the right-click context menu's
    // "Pilot" row, and release goes through the same menu's "Auto-pilot" row or the fleet
    // panel's release button (RtsControls::pilot_ship / release_to_autopilot). --------------

    // ---- M key: retired. The arena <-> galaxy-map "look" is now driven continuously by zoom, so
    // there is no discrete mode to toggle here anymore. ----------------------------------------

    // ---- P key: RETIRED. Pilot <-> auto-pilot/RTS is toggled by TAB (and the HUD pilot button). --

    // ---- Planet approach is now a PASSIVE, zoom-based feed-forward follow (see the capture block
    // in the camera update below): no click to engage and no key to release. Zoom toward a planet
    // and the camera captures + follows it; pan/zoom away and it eases off. (The old double-click
    // travel+lock and the X-release were removed in favour of this.) ------------------------------

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

    if (input_is_key_down(KEY_F10) && !input_was_key_down(KEY_F10)) {

        s->galaxy.map_draw_habitability = s->galaxy.map_draw_habitability ? FALSE : TRUE;

        action_log_push(s, s->galaxy.map_draw_habitability ? "[F10] Habitability overlay ON" : "[F10] Habitability overlay OFF");

    }

    if (input_is_key_down(KEY_F11) && !input_was_key_down(KEY_F11)) {

        s->galaxy.map_draw_civs = s->galaxy.map_draw_civs ? FALSE : TRUE;

        action_log_push(s, s->galaxy.map_draw_civs ? "[F11] Civilization markers ON" : "[F11] Civilization markers OFF");

    }

    if (input_is_key_down(KEY_F12) && !input_was_key_down(KEY_F12)) {

        bs_rml_debugger_toggle(); // dev tool: RmlUi element inspector / live RCSS (in-game UI)

        action_log_push(s, "[F12] RmlUi debugger toggled");

    }

    // Fire-mode toggle (Phase D): T flips every ballistic weapon in the ACTIVE fire group
    // between AP shells and FLAK screen (proximity anti-ordnance). Converge-then-toggle:
    // mixed groups become all-FLAK first.
    if (input_is_key_down(KEY_T) && !input_was_key_down(KEY_T)) {

        Ship& fsh = s->player_ship();

        b8 all_flak = TRUE;
        i32 nballistic = 0;

        for (i32 h = 0; h < fsh.hardpoint_count; ++h) {
            Weapon* w = fsh.mounts[h];
            if (!w || w->wkind != WEAPON_KIND_BALLISTIC) continue;
            if (!((fsh.mount_groups[h] >> fsh.active_group) & 1)) continue;
            ++nballistic;
            if (((BallisticWeapon*)w)->fire_mode != MODE_FLAK) all_flak = FALSE;
        }

        if (nballistic > 0) {

            u8 mode = all_flak ? MODE_AP : MODE_FLAK;

            for (i32 h = 0; h < fsh.hardpoint_count; ++h) {
                Weapon* w = fsh.mounts[h];
                if (!w || w->wkind != WEAPON_KIND_BALLISTIC) continue;
                if (!((fsh.mount_groups[h] >> fsh.active_group) & 1)) continue;
                ((BallisticWeapon*)w)->fire_mode = mode;
            }

            action_log_push(s, "[T] Group %d: %s", fsh.active_group + 1,
                            mode == MODE_FLAK ? "FLAK screen" : "AP shells");

        }

    }

    // Point-defense stance cycle (Phase C doctrine): HOLD -> STANDARD -> OVERDRIVE -> HOLD.
    // One combat-time control; priorities and the engagement gate live in the inspector.
    if (input_is_key_down(KEY_P) && !input_was_key_down(KEY_P) && !s->show_flagship_inspector) {

        DefenseLaser& pd = s->player_ship().point_defense;

        pd.stance = (u8)((pd.stance + 1) % 3);

        static const char* PD_STANCE_NAMES[3] = { "HOLD", "STANDARD", "OVERDRIVE" };

        action_log_push(s, "[P] PD stance: %s", PD_STANCE_NAMES[pd.stance]);

    }

    if (input_is_key_down(KEY_L) && !input_was_key_down(KEY_L)) {

        s->galaxy.show_legends = s->galaxy.show_legends ? FALSE : TRUE;

        action_log_push(s, s->galaxy.show_legends ? "[L] Galaxy legends ON" : "[L] Galaxy legends OFF");

    }

    if (input_is_key_down(KEY_N) && !input_was_key_down(KEY_N)) {

        s->galaxy.show_news = s->galaxy.show_news ? FALSE : TRUE;

        action_log_push(s, s->galaxy.show_news ? "[N] Galactic news ON" : "[N] Galactic news OFF");

    }

    if (input_is_key_down(KEY_I) && !input_was_key_down(KEY_I)) {

        s->galaxy.show_inspector = s->galaxy.show_inspector ? FALSE : TRUE;

        action_log_push(s, s->galaxy.show_inspector ? "[I] Civ inspector ON" : "[I] Civ inspector OFF");

    }

    if (input_is_key_down(KEY_H) && !input_was_key_down(KEY_H) && !s->show_flagship_inspector) {

        s->galaxy.show_houses = s->galaxy.show_houses ? FALSE : TRUE;

        action_log_push(s, s->galaxy.show_houses ? "[H] Dynastic houses ON" : "[H] Dynastic houses OFF");

    }

    if (input_is_key_down(KEY_O) && !input_was_key_down(KEY_O)) {

        s->show_discoveries = !s->show_discoveries;

        action_log_push(s, s->show_discoveries ? "[O] Discoveries browser ON" : "[O] Discoveries browser OFF");

    }

    // DEBUG (K): drop a hostile strike group next to the fleet to force an NPC-vs-NPC engagement
    // on demand -- raids only reach the player's system occasionally, which makes combat nearly
    // impossible to observe or test by waiting.
    if (input_is_key_down(KEY_K) && !input_was_key_down(KEY_K)) {

        i32 n = ai_ships_debug_spawn_strike(s, 3);

        char msg[96];

        snprintf(msg, sizeof(msg), "[K] Spawned %d hostile warship(s) -- expect combat", n);

        action_log_push(s, msg);

    }

    // DEBUG (G): war room overlay -- mission glyphs by objective (red chevron = raid, green arrow =
    // reinforcement column, blue ring = patrol, orange chevron = pirate sortie), red pulsing lanes
    // across borders at war, and garrison / lane-risk readouts at contested nodes. Pair with F3
    // (jump to a war frontier, forcing the war if needed) to watch a war unfold in real time.
    if (input_is_key_down(KEY_G) && !input_was_key_down(KEY_G) && !s->show_flagship_inspector) {

        s->galaxy.map_war_room = !s->galaxy.map_war_room;

        action_log_push(s, s->galaxy.map_war_room ? "[G] War room ON (F3 jumps to a war frontier)"
                                                  : "[G] War room OFF");

    }

    // Feature B: the F3 debug faction-pin has been retired. Real jump-travel now resolves the
    // current system's owner from the flagship's nearest node, so the patrol reflects the true
    // faction wherever you fly. Test transitive stance by aiding a civ (F5) then entering an enemy's
    // space. (s->galaxy.debug_force_civ is left unused for save-format stability.)

    // DEBUG (F3): teleport the fleet to a civ-vs-civ war frontier so the garrison grind is verifiable.
    // Repeated presses cycle through frontiers; a border that is not already at war is forced to war.
    if (input_is_key_down(KEY_F3) && !input_was_key_down(KEY_F3)) {
        static i32 s_last_frontier = -1;
        i32 ca = -1, cb = -1;
        i32 node = galaxy_history_debug_war_frontier(s, s_last_frontier, &ca, &cb);
        if (node >= 0) {
            s_last_frontier = node;
            HierPos2 dest = s->galaxy.nodes[node].galaxy_center;
            HierPos2 flag = s->fleet_state.fleet.flagship().ship.origin;
            Vec2 delta = hierpos_diff(&dest, &flag);
            for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
                FleetShip& fs = s->fleet_state.fleet.at(i);
                fs.ship.origin = hierpos_add_vec2(&fs.ship.origin, delta);
                fs.flight.velocity = bs_math::Vec2{ 0.0f, 0.0f };
                fs.clear_move_target();
                fs.clear_attack_target();
            }
            s->camera_state.free_camera_pos = dest;   // move the RTS free camera along too
            s->npc_spawned_node = -1;                 // force the garrison to re-materialize here
            char msg[128];
            snprintf(msg, sizeof(msg), "[F3] War frontier: %s vs %s (open I to watch the garrison)",
                     (ca >= 0 ? s->galaxy.civs[ca].name : "?"), (cb >= 0 ? s->galaxy.civs[cb].name : "?"));
            action_log_push(s, msg);
        } else {
            action_log_push(s, "[F3] No inter-civ frontier found");
        }
    }

    // Phase C2 debug: perturb the faction whose space the player is in (proxy for real actions).
    if (input_is_key_down(KEY_F4) && !input_was_key_down(KEY_F4)) {
        i32 pc = s->galaxy.current_owner_civ;
        if (pc >= 0) { galaxy_history_player_raid(s, pc, s->galaxy.civs[pc].power * 0.2f + 5.0f);
                       action_log_push(s, "[F4] Raided the current faction"); }
    }

    if (input_is_key_down(KEY_F5) && !input_was_key_down(KEY_F5)) {
        i32 pc = s->galaxy.current_owner_civ;
        if (pc >= 0) { galaxy_history_player_aid(s, pc, 20.0f);
                       action_log_push(s, "[F5] Aided the current faction"); }
    }

    if (input_is_key_down(KEY_F9) && !input_was_key_down(KEY_F9)) {

        b8 imm = renderer_is_present_immediate() ? FALSE : TRUE;

        if (renderer_set_present_mode(imm)) {
            s->profiler.present_immediate = imm;
            action_log_push(s, imm ? "[F9] Present: IMMEDIATE (uncapped)" : "[F9] Present: VSYNC");
        } else {
            action_log_push(s, "[F9] Present mode change REFUSED by driver (still VSYNC)");
        }

    }

    // Reconcile the materialised hot cache with the systems nearest the camera and cache which
    // system is current (galaxy_materialize_update sets s->galaxy.current_system).

    galaxy_materialize_update(s);

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

    // ---- Weapon micro-selection hub (hold MIDDLE MOUSE) -------------------------------------
    // Deliberately OUTSIDE the piloting branch below, and NOT gated on view.mode or the free
    // camera. Fire control is continuous across the arena <-> galaxy-map blend: the mode flip is
    // a label over one shared coordinate space (sim/camera_controller.cpp), and zooming out past
    // ZOOM_MIN force-detaches the camera, so gating on either would kill the hub exactly when
    // the player zooms out to engage something far away. That is also when it matters most --
    // the override drives the AUTOPILOT attack order, which is how long-range engagement is
    // actually fought. Only the editor (which owns middle-mouse for camera pan), the management
    // inspector, and a UI layer holding the cursor suppress it.
    if (!s->editor.edit_mode_active && !s->show_flagship_inspector &&
        !bs_imgui_wants_mouse() && !bs_rml_wants_mouse() &&
        s->fleet_state.fleet.count() > 0) {

        weapon_hub_update(s, &s->fleet_state.fleet.at(piloted_idx).ship);

    } else if (s->weapon_hub_open) {

        weapon_hub_close(s);   // a gate closed mid-hold: dismiss without committing

    }

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

        if (input_is_button_down(BUTTON_MIDDLE)) {

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

    } else {

        // Pilot the manually-controlled fleet member directly.
        //
        // NOT gated on view.mode: the arena <-> galaxy-map flip is a label over one shared
        // coordinate space (sim/camera_controller.cpp), so flying and shooting must not stop
        // at the boundary -- zoom is a camera choice, not a control mode. What actually
        // separates the two control schemes is whether the camera is DETACHED, and every
        // block below already gates on that:
        //   * control_ship_global returns FALSE immediately when free_camera_active
        //     (sim/ship_control.cpp:21), so WASDQE never fights the free camera's pan;
        //   * the fire-group number row is suppressed while detached, because there the row
        //     belongs to RTS unit selection (sim/rts_controls.cpp);
        //   * turret traverse and left-click firing are suppressed while detached, because
        //     there left-click belongs to RTS box/click selection (sim/rts_controls.cpp:246).
        // So: piloting at ANY zoom in EITHER look flies and fires; detached at any zoom is
        // RTS, where engagement runs through attack orders (which honour weapon_override).

        FleetShip* pf = &s->fleet_state.fleet.at(piloted_idx);

        Ship* psh = &pf->ship;

        // The inspector is MODAL: while it is open the player is a shipwright, not a pilot,
        // so flight input is fully suppressed and the hull coasts on its momentum. The
        // world-drag pickup block below stays live -- it IS inspector interaction.
        turn_commanded = s->show_flagship_inspector ? FALSE : control_ship_global(s, pf, sim_dt);

        // ---- Ship-side loadout drag (inspector open) --------------------------------------
        // Pick a mounted item straight off one of the flagship's hardpoint boxes: press on an
        // occupied box to lift it (arms the same pending_weapon_drag protocol as the window
        // drags, so the green/red fit boxes light up), release on another box to move/swap,
        // release anywhere else - empty space or the inspector window - to unmount it back to
        // its inventory. Safe alongside firing: firing is fully suppressed while the inspector
        // is open.
        if (s->show_flagship_inspector && !s->editor.edit_mode_active) {
            // Portrait view first: it arms the pan only when the press hits NO hardpoint box,
            // so the pickup below always wins the presses it cares about.
            update_portrait_view_input(s);
            Ship& fsh      = s->inspected_ship();  // the hull being refitted
            Ship& pool     = s->player_ship();     // fleet-wide inventory (stow target)
            b8    pressed  = input_is_button_down(BUTTON_LEFT) && !input_was_button_down(BUTTON_LEFT);
            b8    released = !input_is_button_down(BUTTON_LEFT) && input_was_button_down(BUTTON_LEFT);
            if (pressed && !s->world_module_drag && s->pending_weapon_drag < 0 &&
                !bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {
                i32 hp = inspected_hardpoint_at_cursor(s);
                if (hp >= 0) {
                    if (fsh.mounts[hp]) {
                        s->pending_weapon_drag = hp;  s->pending_weapon_drag_kind = 0;   // mounted weapon
                        s->world_module_drag = TRUE;
                    } else if (fsh.point_defense_mount == hp) {
                        s->pending_weapon_drag = hp;  s->pending_weapon_drag_kind = 3;   // mounted point-defense
                        s->world_module_drag = TRUE;
                    } else if (fsh.module_mounts[hp]) {
                        s->pending_weapon_drag = hp;  s->pending_weapon_drag_kind = 5;   // installed module
                        s->world_module_drag = TRUE;
                    }
                }
            } else if (released && s->world_module_drag) {
                i32 src  = s->pending_weapon_drag;
                i32 kind = s->pending_weapon_drag_kind;
                // Releasing over any UI window counts as "off the ship": stow, don't hit-test
                // boxes that may sit behind the panel.
                i32 hit  = (bs_imgui_wants_mouse() || bs_rml_wants_mouse())
                               ? -1 : inspected_hardpoint_at_cursor(s);
                if (hit >= 0 && hit != src) {
                    arsenal_drop_on_slot(s, hit);                 // move/swap; disarms the drag
                } else if (hit < 0 && src >= 0 && src < fsh.hardpoint_count) {
                    // Released off the ship: unmount back into the fleet-wide pool (weapons,
                    // the PD device and modules alike -- the PD returns to fleet_pd_stock).
                    if (kind == 0 && fsh.mounts[src]) {
                        Weapon* w = fsh.mounts[src];
                        fsh.mounts[src] = nullptr;
                        ship_stash_append(pool, w);
                        ship_rehome_weapons(fsh);
                        action_log_push(s, "%s returned to inventory.", w->name ? w->name : "Weapon");
                    } else if (kind == 3 && fsh.point_defense_mount == src) {
                        fsh.point_defense_mount   = -1;
                        fsh.point_defense.enabled = FALSE;
                        ++s->fleet_pd_stock;         // device back into the fleet pool
                        ship_rehome_weapons(fsh);
                        action_log_push(s, "Point Defense Laser returned to inventory.");
                    } else if (kind == 5 && fsh.module_mounts[src]) {
                        const ModuleDef* m = fsh.module_mounts[src];
                        if (pool.module_stash_count >= SHIP_MAX_MODULES) {
                            action_log_push(s, "Module rack full - can't remove '%s'.", m->name);
                        } else {
                            fsh.module_mounts[src] = nullptr;
                            ship_module_stash_append(pool, m);
                            ship_recompute_stats(&fsh);
                            action_log_push(s, "%s returned to the module rack.", m->name);
                        }
                    }
                }
                s->pending_weapon_drag = -1;
                s->world_module_drag   = FALSE;
            }
        } else if (s->world_module_drag) {
            // Inspector closed (or editor grabbed input) mid-drag: cancel cleanly.
            s->pending_weapon_drag = -1;
            s->world_module_drag   = FALSE;
        }

        // ---- Weapon-group selection (keys 1-5) ---------------------------------------------
        // Deliberately OUTSIDE the firing gate below so group selection works with the Arsenal
        // inspector open (assign in the matrix, then test).
        // Also suppressed by the hardpoint editor and any UI layer capturing the KEYBOARD.
        // (Group ASSIGNMENT lives in the inspector's Fire groups matrix - "gm:" hud_action.)
        //
        // NO LONGER piloting-only. The row used to be suppressed while the free camera was
        // detached because there it belonged to RTS unit selection -- but that was a binding
        // collision, not a design decision, and with one hull the RTS number row only ever
        // re-selected the ship that was already piloted (now retired in rts_controls.cpp).
        // Fire groups are combat input in BOTH control modes, and the weapon hub they drive was
        // never mode-gated, so suppressing the row left the hub showing a group the player had
        // no keyboard way to change while detached.
        if (!s->editor.edit_mode_active && !s->show_flagship_inspector &&
            !bs_imgui_wants_keyboard() && !bs_rml_wants_keyboard()) {

            ship_groups_sanitize(psh);

            static const keys group_keys[SHIP_WEAPON_GROUPS] =
                { KEY_NUM1, KEY_NUM2, KEY_NUM3, KEY_NUM4, KEY_NUM5 };

            for (i32 g = 0; g < SHIP_WEAPON_GROUPS; ++g) {

                if (!input_is_key_down(group_keys[g]) || input_was_key_down(group_keys[g])) continue;

                ship_select_weapon_group(psh, g);

                i32 n = ship_group_size(psh, g);

                if (n > 0) action_log_push(s, "Weapon group %d selected (%d weapon%s).", g + 1, n, n == 1 ? "" : "s");
                else       action_log_push(s, "Weapon group %d is empty.", g + 1);

            }

        }

        // ---- Weapon firing (left button HELD, gated on neither UI layer owning the cursor) ----

        // Also suppressed while the flagship inspector is open: it is a management window, so a
        // left-click anywhere (inside or outside its bounds) must never fire the weapons.
        //
        // ATTACHED-ONLY again -- for the opposite reason it once was. With the command overlay
        // retired, box/click selection owns the left button whenever the camera is DETACHED
        // (sim/rts_controls.cpp): detached is the command mode, attached is the gun. Manual
        // gunnery therefore lives in the pilot seat only, and cursor turret-traverse goes with
        // the trigger -- a barrel tracking a cursor that cannot fire would be a lie. While
        // detached the piloted hull fights like any other AI wingman under its orders.
        if (!s->editor.edit_mode_active && !s->show_flagship_inspector &&
            !s->camera_state.free_camera_active
            && !bs_imgui_wants_mouse() && !bs_rml_wants_mouse()) {

            // Turret traverse: every weapon in the CURRENT SELECTION tracks the cursor. Under a
            // micro-selection override that is the one chosen weapon, so the hull art shows at a
            // glance which mount is live; the rest slew back to their rest facing.

            {

                bs_math::HierPos2 mw_hp = mouse_true_hierpos(s);

                for (i32 i = 0; i < psh->hardpoint_count; ++i) {

                    if (!ship_hardpoint_in_selection(psh, i)) continue;

                    bs_math::HierPos2 fo = ship_hardpoint_fire_origin(psh, i);

                    Vec2 dir = hierpos_diff(&mw_hp, &fo, BS_HIERPOS_CELL_SIZE);

                    ship_turret_aim_at(psh, i, dir);

                }

            }

            // Trigger HELD -> every frame, fire every member of the current selection that passes
            // validation. This is a trigger change only: the loop below is unchanged, so each
            // weapon still rate-limits itself through WEAPON_FIRE_RELOADING and nothing bypasses
            // ship_weapon_fire_state. The press edge is kept solely to gate the feedback block,
            // which would otherwise push an action-log line every frame the trigger is held.

            b8 fire_held    = input_is_button_down(BUTTON_LEFT);

            b8 fire_pressed = fire_held && !input_was_button_down(BUTTON_LEFT);

            if (fire_held) {

                bs_math::HierPos2 mw_hp = mouse_true_hierpos(s);

                i32 members = 0, blocked = 0, starved = 0, ranged = 0, dead = 0, fired = 0;

                for (i32 i = 0; i < psh->hardpoint_count; ++i) {

                    if (!ship_hardpoint_in_selection(psh, i)) continue;

                    Weapon* w = psh->mounts[i];

                    ++members;

                    // Shots leave from each weapon's own hardpoint; aim and measure from there.
                    bs_math::HierPos2 fire_origin = ship_hardpoint_fire_origin(psh, i);

                    Vec2 dir  = hierpos_diff(&mw_hp, &fire_origin, BS_HIERPOS_CELL_SIZE);

                    f32  dist = vec2_length(dir);

                    // One validator, shared with the hub: arc, cooldown, reach, power, status.
                    // Whatever the hub showed for this weapon is exactly what happens here.
                    switch (ship_weapon_fire_state(psh, i, dir, dist)) {

                        case WEAPON_FIRE_NO_BEARING:   ++blocked; continue;   // outside its arc

                        case WEAPON_FIRE_RELOADING:              continue;   // cooling down: silent
                        // Barrel still training onto the target. Silent like RELOADING: it
                        // resolves in a fraction of a second and reporting it would fire an
                        // action-log line on every press that beat the turret.
                        case WEAPON_FIRE_SLEWING:                continue;

                        case WEAPON_FIRE_OUT_OF_RANGE: ++ranged;  continue;   // armed, holds fire

                        case WEAPON_FIRE_STARVED:      ++starved; continue;   // cannot afford the shot

                        case WEAPON_FIRE_DISABLED:     ++dead;    continue;   // knocked out

                        default: break;                                       // WEAPON_FIRE_READY

                    }

                    // Validated: commit the capacitor spend, then fire.
                    if (!ship_try_spend_cap(psh, w->cap_cost())) { ++starved; continue; }

                    w->owner_faction_id = psh->faction_id;   // stamp attacker faction for hit attribution

                    ship_hardpoint_fire(psh, i, dir, pf->flight.velocity, &s->projectiles);

                    ++fired;

                }

                // Feedback. Only ever reported when the trigger produced NOTHING, and only when
                // one cause accounts for the whole selection - a partial volley stays quiet.
                // Gated on the PRESS EDGE, not the hold: action_log_push neither dedups nor rate
                // limits, so reporting per frame would flood the buffer and pin the HUD fade open.
                // One press still yields at most one message, exactly as before.
                if (!fire_pressed) {

                    // held, not pressed - fire without reporting

                } else if (members == 0) {

                    if (psh->weapon_override >= 0)
                        action_log_push(s, "Selected weapon is no longer mounted.");
                    else
                        action_log_push(s, "Weapon group %d is empty.", psh->active_group + 1);

                } else if (fired == 0) {

                    const Weapon* sel = (psh->weapon_override >= 0) ? psh->mounts[psh->weapon_override] : nullptr;

                    const char* sel_name = (sel && sel->name) ? sel->name : "Selected weapon";

                    if (dead == members) {

                        if (sel) action_log_push(s, "%s is disabled.", sel_name);
                        else     action_log_push(s, "Every weapon in group %d is disabled.", psh->active_group + 1);

                    } else if (blocked == members) {

                        if (sel) action_log_push(s, "%s cannot bear on target.", sel_name);
                        else     action_log_push(s, "No weapon in group %d can bear on target.", psh->active_group + 1);

                    } else if (ranged > 0 && ranged + blocked == members) {

                        action_log_push(s, "Target out of range - holding fire.");

                    } else if (starved > 0 && starved + blocked + ranged == members) {

                        action_log_push(s, "Capacitor dry.");

                    }

                }

            }

        }

    }

    // ---- Update weapons (cooldowns) + capacitor regen -----------------------------------

    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {

        Ship& sh = s->fleet_state.fleet.at(i).ship;

        for (i32 w = 0; w < sh.hardpoint_count; ++w)

            if (sh.mounts[w]) sh.mounts[w]->update(sim_dt);

        ship_capacitor_update(&sh, sim_dt);

        ship_update_turrets(&sh, sim_dt);   // slew turret art toward this frame's aim goals

    }

    for (i32 i = 0; i < s->fleet_state.enemy_ship.hardpoint_count; ++i) {

        if (s->fleet_state.enemy_ship.mounts[i]) s->fleet_state.enemy_ship.mounts[i]->update(sim_dt);

    }

    ship_capacitor_update(&s->fleet_state.enemy_ship, sim_dt);

    ship_update_turrets(&s->fleet_state.enemy_ship, sim_dt);

    // ---- RTS controls update (orders, selection, hover) --------------------------------

    // Runs on REAL dt (not sim_dt): input, free-camera panning, and UI marquee animations must be

    // unaffected by the time scale -- they keep working while paused and never speed up at 3x/5x/10x.

    s->profiler.begin(PROF_RTS);

    // Gated while the ship inspector is open (modal): one gate kills orders, selection, the
    // J/X/F keys and free-camera movement together. The piloted index cannot change while
    // modal (TAB and the HUD pilot button are both gated/hidden), so skipping the update's
    // piloted-index sync cannot go stale in a way that matters.
    if (!s->show_flagship_inspector) s->rts_controls.update(dt);

    s->profiler.end(PROF_RTS);

    if (!s->editor.edit_mode_active) {

        // AUTOPILOT: drive ordered ships toward their targets. Only skip the piloted ship when it
        // is actually under manual control -- i.e. whenever the camera is ATTACHED. Detached,
        // nothing is flying it by hand, so it obeys its move/attack order like any other hull.
        //
        // The `view.mode == MODE_GLOBAL` term this used to carry is GONE, and removing it was
        // mandatory, not tidying. It was only ever redundant belt-and-braces: the zoom crossing
        // force-detached the camera, so `view.mode != MODE_GLOBAL` already implied
        // free_camera_active. Now that zoom no longer touches the control mode
        // (sim/camera_controller.cpp), piloting below ZOOM_MIN is reachable -- and with the old
        // term still here that state read as "not manually piloting", so auto_skip would be -1 and
        // the AUTOPILOT WOULD DRIVE THE SHIP THE PLAYER IS FLYING: control_ship_global adding
        // thrust while update_move/update_attack writes ship->angle directly and zeroes
        // angular_velocity. Two controllers, one hull, fighting every frame.
        b8  manually_piloting = !s->camera_state.free_camera_active;

        i32 auto_skip = manually_piloting ? piloted_idx : -1;

        s->profiler.begin(PROF_FLEET_AUTOPILOT);

        // auto_skip alone carries the whole player-vs-autopilot fire arbitration now: manual
        // gunnery is attached-only (detached, LMB is selection), so while detached the piloted
        // hull is just another wingman and fires its ballistics under attack orders like the
        // rest of the fleet.
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

        // Static enemy: track + fire on the flagship when it enters the detector radius.

        combat_arena_update_enemy_ai(s, sim_dt);

        // General Ship AI: population manager + per-agent behavior (move + fire).

        ai_ships_update(s, sim_dt);

    }

    // General Ship AI: (re)register active NPC agents into the combat-entity pool each frame so the

    // projectile sweep + RTS targeting see them. Runs every frame (incl. edit mode) before the sync.

    ai_ships_register_combat(s);

    // ---- Sync combat entity positions / velocities from their ships (sim/combat_arena.cpp) --
    combat_arena_sync_entities(s, sim_dt);

    // ---- Autonomous flak screen: MODE_FLAK mounts engage incoming ordnance on their own ------
    // (sim/flak_screen.cpp). The OUTER defense layer: runs after positions are synced, BEFORE
    // the PD laser and the projectile advance, and claims its mounts so the attack broadside
    // yields them this tick.
    flak_screen_update(s, sim_dt);

    // ---- Point-defense lasers: intercept incoming hostile projectiles (sim/point_defense.cpp) --
    // Runs after positions are synced and BEFORE projectiles advance, so destroyed threats
    // never move / collide this frame. Records active beams into s->defense_beams for the overlay.
    point_defense_update(s, sim_dt);

    // ---- Update projectiles + projectile/entity collision (sim/combat_arena.cpp) ------------
    combat_arena_update_projectiles(s, sim_dt);

    // ---- Sync world entities to galaxy map --------------------------------------------
    // Rebuild the generic map entity list every frame (sim/galaxy_map.cpp).
    galaxy_map_sync_entities(s);

    // ---- Orbital motion: advance planets BEFORE the camera rebase below, so a followed planet's
    // camera target matches where the planet is actually drawn this frame. Otherwise the 1-frame
    // lag is magnified at high zoom and the camera "zooms past" the planet. ----------------------
    galaxy_map_update_orbits(s, sim_dt);

    // Camera follows the piloted ship, or holds a detached free-camera / edit center -- the SAME

    // model at every zoom (arena and galaxy-map looks). The renderer draws each entity at

    // (world - camera_hierpos), then camera2d subtracts camera.position; we choose ONE

    // true-world center per frame, fold it into camera_hierpos (canonicalized, keeps f32 coords

    // tiny) and keep the sub-cell residual in camera.position. This IS the floating-origin rebase.

    {

        // Recenter glide (TAB re-pilot / HUD pilot button / galaxy on-screen re-entry): ease the
        // detached center onto the piloted ship, then hand control back to ship-follow (piloting).

        if (s->camera_state.recentering) {

            s->camera_state.recenter_t += dt / 0.80f; // ~0.8 second duration

            if (s->camera_state.recenter_t > 1.0f) s->camera_state.recenter_t = 1.0f;

            f32 eased = s->camera_state.recenter_t * s->camera_state.recenter_t * (3.0f - 2.0f * s->camera_state.recenter_t);

            HierPos2 ship = piloted_ship_origin(s);

            s->camera_state.free_camera_pos = hierpos_lerp(&s->camera_state.recenter_from_pos, &ship, eased,

                                              BS_HIERPOS_CELL_SIZE);

            if (s->camera_state.recenter_t >= 1.0f) {

                s->camera_state.recentering        = FALSE;

                s->camera_state.free_camera_active = FALSE; // hand control back to ship-follow

            }

        }

        // ---- Planet approach: passive zoom-triggered feed-forward follow (replaces the double-
        // click follow). Zoom toward an orbiting planet -> it is captured and followed. The camera
        // moves WITH the planet each frame (velocity feed-forward -> no catch-up lag however fast
        // the planet crosses the screen when zoomed in) and softly corrects the residual gap to
        // keep it centred. WASD is a small bounded pan (it fights the correction). Engage/release
        // ease via `weight`. PURE CAMERA: the simulation (planet positions) is untouched.
        // galaxy_map_update_orbits ran just above, so positions match this frame's render.
        //
        // DETACHED ONLY. This is a BROWSING affordance, and it used to force free_camera_active on
        // at the moment of capture -- the third place a zoom gesture silently took the helm, and
        // the one left over after the ZOOM_MIN hand-off was removed. Under TAB-only that has to go,
        // or the wheel still steals control, just at a different threshold. Gating capture on the
        // camera already being detached is the right cut rather than merely dropping the assignment:
        // while attached the camera is pinned to the ship, so a planet follow could not take effect
        // anyway -- the two would simply fight over the same centre every frame.
        if (!s->camera_state.free_camera_active) {
            // Attached: make sure a capture from a previous detached spell cannot persist. TAB
            // already clears this, but leaving the release to one input site is what let this state
            // survive a mode change in the first place.
            s->planet_approach.engaged   = FALSE;
            s->planet_approach.candidate = FALSE;
            s->planet_approach.weight    = 0.0f;
            s->planet_approach.leaving   = FALSE;
        }
        if (!s->camera_state.recentering && s->camera_state.free_camera_active) {
            static const f32 AP_SIZE_LO_PX   = 4.0f;    // apparent planet size where capture begins (also where the hold fades out)
            static const f32 AP_SIZE_HI_PX   = 16.0f;   // ...and where it is fully size-eligible
            static const f32 AP_BIG_PX       = 24.0f;   // already this big + centred -> engage w/o zooming
            static const f32 AP_ACQUIRE_FRAC = 0.30f;   // acquire radius / half-min screen dimension
            static const f32 AP_HOLD_FRAC    = 1.00f;   // HOLD prox edge: full follow across the clamp region, fades only when panned ~a screen-half away
            static const f32 AP_OFFSET_FRAC  = 0.35f;   // max WASD pan offset: planet stays within this * half-min of centre
            static const f32 AP_INNER_FRAC   = 0.35f;   // within this fraction of the edge -> full prox
            static const f32 AP_ENGAGE_RATE  = 4.0f;    // 1/s weight ease-in
            static const f32 AP_RELEASE_RATE = 2.0f;    // 1/s weight ease-out
            static const f32 AP_LEAVE_RATE   = 8.0f;    // 1/s faster ease-out when zooming OUT to leave
            static const f32 AP_SETTLE_DUR   = 0.40f;   // s: smooth ease-to-centre window on acquire (no hard snap)
            static const f32 AP_SETTLE_RATE  = 9.0f;    // 1/s exponential ease rate during the acquire settle

            auto& ap = s->planet_approach;
            const f32 zoom = s->camera_state.camera.zoom;
            const f32 eff_depth = s->render.depth_planet * celestial_parallax_fade(s);
            f32 denom = 1.0f - eff_depth; if (denom < 0.05f) denom = 0.05f;
            f32 screen_scale = zoom * denom; if (screen_scale < 1e-9f) screen_scale = 1e-9f;
            f32 half_min = 0.5f * (f32)((s->fb_width < s->fb_height) ? s->fb_width : s->fb_height);
            f32 poff_max = 0.85f * (f32)GALAXY_MATERIALIZE_RADIUS;

            HierPos2 cam_center = s->camera_state.free_camera_active
                                ? s->camera_state.free_camera_pos : piloted_ship_origin(s);

            // 1) Nearest planet (parallax-corrected abs pos) to the camera centre, across systems.
            // Also record the currently-HELD planet's own apparent size (independent of which planet
            // is nearest) so a WASD pan can never swap or drop the lock -- see the sticky hold below.
            i32 best_slot = -1, best_pi = -1; f32 best_dist = 3.4e38f, best_size_px = 0.0f;
            HierPos2 best_target{};
            b8 held_found = FALSE; f32 held_size_px = 0.0f, held_dist = 3.4e38f;
            for (i32 sl = 0; sl < s->galaxy.system_count; ++sl) {
                StarSystem& ss = s->galaxy.systems[sl];
                b8 sl_is_held_sys = ap.engaged &&
                    ss.galaxy_center.cell.x == ap.system_center.cell.x && ss.galaxy_center.cell.y == ap.system_center.cell.y &&
                    ss.galaxy_center.local.x == ap.system_center.local.x && ss.galaxy_center.local.y == ap.system_center.local.y;
                for (i32 i = 0; i < ss.planet_count; ++i) {
                    const CelestialBody& p = ss.planets[i];
                    if (p.semi_major_axis * zoom < 2.0f) continue; // orbit sub-2px -> ignore
                    Vec2 poff = vec2_scale(p.position, 1.0f / denom);
                    f32 pl = vec2_length(poff); if (pl > poff_max) poff = vec2_scale(poff, poff_max / pl);
                    HierPos2 target = hierpos_add_vec2(&ss.galaxy_center, poff);
                    f32 dist = vec2_length(hierpos_diff(&target, &cam_center, BS_HIERPOS_CELL_SIZE));
                    const PlanetTypeParams& pe = s->render.star_fx.planet_params[(i32)ss.planet_props[i].type];
                    f32 size_px = p.radius * pe.size_mul * zoom;
                    if (dist < best_dist) {
                        best_dist = dist; best_slot = sl; best_pi = i; best_target = target;
                        best_size_px = size_px;
                    }
                    if (sl_is_held_sys && i == ap.planet_index) { held_found = TRUE; held_size_px = size_px; held_dist = dist; }
                }
            }

            // Zoom-direction intent. Zooming OUT of a captured planet means "leave": latch ap.leaving
            // so the screen clamp lets go and WASD can freely navigate toward other planets, while the
            // (framing-based) follow below keeps the planet in view and fades out smoothly. Zooming
            // back IN clears it so the clamp re-frames the planet. ap_zoom_in reused by step 3 acquire.
            b8 ap_zoom_in  = s->camera_state.target_zoom > s->camera_state.camera.zoom * 1.005f;
            b8 ap_zoom_out = s->camera_state.target_zoom < s->camera_state.camera.zoom * 0.995f;
            if (ap.engaged) {
                if (ap_zoom_out)     ap.leaving = TRUE;
                else if (ap_zoom_in) ap.leaving = FALSE;
            }

            // 2) Target weight. STICKY HOLD: once a planet is captured it stays held no matter which
            // planet is now nearest, so WASD panning never swaps the lock to a neighbour. The follow
            // strength is FRAMING-based (apparent size * screen proximity): FULL while the planet is
            // large & near centre, fading GENTLY as it shrinks (zoom-out) or is panned ~a screen-half
            // away -- so zooming out keeps it in view and releases the lock smoothly instead of the
            // follow cutting out and the planet flying off. The hold prox radius (AP_HOLD_FRAC) is
            // generous so proximity stays 1 across the on-screen clamp region (no judder while panning).
            // Not yet engaged -> acquire needs apparent-size eligibility * screen-space proximity.
            f32 w_target = 0.0f; b8 same_as_held = FALSE;
            if (ap.engaged && held_found) {
                same_as_held = TRUE;
                f32 held_sdist_px = held_dist * screen_scale;
                f32 R_edge_px = AP_HOLD_FRAC * half_min;
                f32 R_in_px   = R_edge_px * AP_INNER_FRAC;
                f32 tp = clampf((R_edge_px - held_sdist_px) / fmaxf(R_edge_px - R_in_px, 1.0f), 0.0f, 1.0f);
                f32 w_prox = tp * tp * (3.0f - 2.0f * tp);
                f32 ts = clampf((held_size_px - AP_SIZE_LO_PX) / fmaxf(AP_SIZE_HI_PX - AP_SIZE_LO_PX, 1.0f), 0.0f, 1.0f);
                f32 w_size = ts * ts * (3.0f - 2.0f * ts);
                w_target = w_size * w_prox;
            } else if (best_slot >= 0) {
                f32 R_edge_px = AP_ACQUIRE_FRAC * half_min;
                f32 R_in_px   = R_edge_px * AP_INNER_FRAC;
                f32 sdist_px  = best_dist * screen_scale;
                f32 tp = clampf((R_edge_px - sdist_px) / fmaxf(R_edge_px - R_in_px, 1.0f), 0.0f, 1.0f);
                f32 w_prox = tp * tp * (3.0f - 2.0f * tp);
                f32 ts = clampf((best_size_px - AP_SIZE_LO_PX) / fmaxf(AP_SIZE_HI_PX - AP_SIZE_LO_PX, 1.0f), 0.0f, 1.0f);
                f32 w_size = ts * ts * (3.0f - 2.0f * ts);
                w_target = w_size * w_prox;
            }

            // 3) Acquire when the player zooms toward a centred, visible planet (or one already big
            // & centred). Engage at FULL follow strength IMMEDIATELY -- no weight ramp. A gradual
            // ramp leaves the camera un-centred while the pre-lock cursor-pin parallax drift shoots
            // the planet away (worst at a low parallax-fade threshold -> high eff_depth). Instant
            // full follow == the "already locked" state (stable); the correction eases the residual.
            // Pre-lock APPROACH signal: a visible planet near the view centre that we're zooming
            // toward. camera_controller reads this to disable the cursor-pin during the approach, so
            // the zoom homes on the planet instead of letting the parallax drift shoot it off (worst
            // at a low parallax-fade threshold). 'engaged' is handled by the same gate separately.
            f32 best_sdist_px = (best_slot >= 0) ? best_dist * screen_scale : 3.4e38f;
            s->planet_approach.candidate = (best_slot >= 0 && best_size_px > 2.5f && ap_zoom_in &&
                                            best_sdist_px < AP_ACQUIRE_FRAC * half_min);
            // Lock on the FIRST zoom-in frame toward a centred visible planet -- BEFORE the pre-lock
            // cursor-pin drift can accumulate (per-frame zoom is small/eased, so the planet is still
            // centred on frame 1; waiting for it to grow to ~12px let the drift shoot it off first).
            b8 ap_centered = best_sdist_px < AP_ACQUIRE_FRAC * half_min;
            if (best_slot >= 0 && !same_as_held && !ap.leaving && best_size_px > 2.5f && ap_centered &&
                (ap_zoom_in || best_size_px > AP_BIG_PX)) {
                ap.engaged = TRUE; ap.planet_index = best_pi; ap.leaving = FALSE;
                ap.system_center   = s->galaxy.systems[best_slot].galaxy_center;
                ap.planet_abs_prev = best_target;
                ap.weight          = 1.0f;                        // instant full follow -> no drift/lag (prevents shoot)
                ap.settle_t        = AP_SETTLE_DUR;               // smooth ease-to-centre instead of a hard snap
                // No free_camera_active = TRUE here any more: the block only runs while already
                // detached, so capture can no longer change the control mode behind the player.
                s->camera_state.free_camera_pos    = cam_center;  // start the ease from the CURRENT view (no jump);
                                                                  // step 4 glides it to the planet over AP_SETTLE_DUR
                same_as_held = TRUE;
            }

            // 4) Feed-forward follow of the held planet (kept through the whole release fade).
            if (ap.engaged) {
                i32 slot = -1;
                for (i32 sl = 0; sl < s->galaxy.system_count; ++sl) {
                    const HierPos2& gc = s->galaxy.systems[sl].galaxy_center;
                    if (gc.cell.x == ap.system_center.cell.x && gc.cell.y == ap.system_center.cell.y &&
                        gc.local.x == ap.system_center.local.x && gc.local.y == ap.system_center.local.y) { slot = sl; break; }
                }
                StarSystem* fs = (slot >= 0) ? &s->galaxy.systems[slot] : nullptr;
                if (!fs || ap.planet_index < 0 || ap.planet_index >= fs->planet_count) {
                    ap.weight += (0.0f - ap.weight) * (1.0f - expf(-AP_RELEASE_RATE * dt)); // system gone -> fade out
                    if (ap.weight < 0.003f) { ap.engaged = FALSE; ap.weight = 0.0f; ap.leaving = FALSE; }
                } else {
                    s->galaxy.current_system = slot; // keep the arena renderer drawing this system
                    Vec2 poff = vec2_scale(fs->planets[ap.planet_index].position, 1.0f / denom);
                    f32 pl = vec2_length(poff); if (pl > poff_max) poff = vec2_scale(poff, poff_max / pl);
                    HierPos2 target = hierpos_add_vec2(&fs->galaxy_center, poff);

                    f32 target_w = same_as_held ? w_target : 0.0f;
                    f32 rate = (target_w > ap.weight) ? AP_ENGAGE_RATE
                                                      : (ap.leaving ? AP_LEAVE_RATE : AP_RELEASE_RATE);
                    ap.weight += (target_w - ap.weight) * (1.0f - expf(-rate * dt));

                    // FULL-strength velocity FEED-FORWARD (NOT scaled by the release weight): move WITH
                    // the planet's motion AND its parallax-denom motion exactly each frame, so it never
                    // drifts out of frame -- while the camera KEEPS whatever offset WASD has set. Full
                    // velocity is CRITICAL for planets FAR from their star: the follow target
                    // (galaxy_center + planet.position/denom) moves a LARGE world distance as the
                    // zoom-fade denom changes on zoom-out, so any under-follow (weight<1) there would
                    // "shoot" a distant planet off screen. ap.weight is only the acquire/release timer.
                    // Centred by the transient acquire settle just below (a smooth ease, not a snap).
                    Vec2 vel = hierpos_diff(&target, &ap.planet_abs_prev, BS_HIERPOS_CELL_SIZE);
                    s->camera_state.free_camera_pos =
                        hierpos_add_vec2(&s->camera_state.free_camera_pos, vel);
                    ap.planet_abs_prev = target;

                    // Smooth ACQUIRE SETTLE: on lock the camera eases to centre the planet over a
                    // short window instead of snapping. weight is already 1 so the feed-forward above
                    // tracks the planet (no drift) while this closes the initial off-centre gap. It is
                    // time-boxed (AP_SETTLE_DUR) so it hands off to pure feed-forward + WASD free-pan
                    // and never fights panning afterwards. Skipped while leaving.
                    if (ap.settle_t > 0.0f && !ap.leaving) {
                        ap.settle_t -= dt;
                        Vec2 gap = hierpos_diff(&target, &s->camera_state.free_camera_pos, BS_HIERPOS_CELL_SIZE);
                        f32 a = 1.0f - expf(-AP_SETTLE_RATE * dt);
                        s->camera_state.free_camera_pos =
                            hierpos_add_vec2(&s->camera_state.free_camera_pos, vec2_scale(gap, a));
                        if (ap.settle_t < 0.0f) ap.settle_t = 0.0f;
                    }

                    // Soft screen-edge clamp: WASD (applied earlier by rts_controls) pans the camera
                    // freely around the locked planet, but never far enough to push it off screen. Only
                    // the OFFSET magnitude is bounded, and only AT the boundary -- the feed-forward
                    // above still tracks the planet, so this never re-centres or reintroduces the
                    // parallax-fade coupling (which would fight the WASD offset the player set).
                    // Skipped while LEAVING (zooming out) so WASD is free to navigate to other planets.
                    Vec2 off = hierpos_diff(&s->camera_state.free_camera_pos, &target, BS_HIERPOS_CELL_SIZE);
                    f32 off_px = vec2_length(off) * screen_scale;
                    f32 off_max_px = AP_OFFSET_FRAC * half_min;
                    if (!ap.leaving && off_px > off_max_px && off_px > 1e-6f) {
                        Vec2 off_clamped = vec2_scale(off, off_max_px / off_px);
                        s->camera_state.free_camera_pos = hierpos_add_vec2(&target, off_clamped);
                    }

                    // Zoom cap: tighten toward 0.32*fb_h apparent size as capture strengthens, so the
                    // player can't zoom infinitely into the planet. Loosened while weight is low.
                    const PlanetTypeParams& fpe = s->render.star_fx.planet_params[(i32)fs->planet_props[ap.planet_index].type];
                    f32 body_scale = fs->planets[ap.planet_index].radius * fpe.size_mul;
                    if (body_scale > 1.0f) {
                        f32 cap = (0.32f * (f32)s->fb_height) / body_scale;
                        // Approaching the planet -> ease zoom-IN speed down as the camera nears the
                        // framed size (deceleration), so it glides in gently instead of rushing.
                        f32 zt = clampf((s->camera_state.camera.zoom / cap - 0.4f) / 0.6f, 0.0f, 1.0f);
                        ap.zoom_damp = zt * zt * (3.0f - 2.0f * zt);
                        if (ap.weight > 0.05f) {
                            f32 eff_cap = cap + (1.0f - ap.weight) * 100.0f;
                            if (s->camera_state.target_zoom > eff_cap) s->camera_state.target_zoom = eff_cap;
                            if (s->camera_state.camera.zoom  > eff_cap) s->camera_state.camera.zoom  = eff_cap;
                        }
                    }

                    if (target_w <= 0.0f && ap.weight < 0.003f) { ap.engaged = FALSE; ap.weight = 0.0f; ap.leaving = FALSE; }
                }
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

    // ---- Discovery system: reveal NPC ships + system stations the player has approached.
    // Runs after NPC updates so this frame's positions are current.
    discovery_update(s, sim_dt);

    // ---- Coordinate diagnostics: dump/verify HierPos2 state now that the frame's positions

    // and the camera anchor are finalized (throttled; compiled out in release builds).

    coord_diag_update(s, dt);

    // ---- In-game UI (RmlUi): push the HUD snapshot + drain button clicks (APP_PLAYING only).
    game_push_hud(s, dt);

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

// view_arena_weight moved to core/view_transform.cpp (see game.h).

// PROFILER panel -- bottom-left, collapsible per-subsystem CPU timing readout (Profiler system).
// build_profiler_panel now lives in ui/editor_ui.cpp (declared via ui/editor_ui.h).

b8 game_render(Game* game_inst, f32 dt) {

    game_state* s = (game_state*)game_inst->state;

    if (!s) return TRUE;

    BS_PROFILE(&s->profiler, PROF_RENDER_TOTAL);

    s->elapsed_time += dt;

    // Phase A: the New Game setup screen + generation progress are the only things drawn until
    // gameplay begins (the galaxy is not generated yet, so skip all world rendering).
    if (s->app_phase == APP_SETUP)      { build_new_game_setup(s);      return TRUE; }
    if (s->app_phase == APP_GENERATING) { build_generation_progress(s); return TRUE; }

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

    // Action Log, Encounter modal, Nav/Ship HUD and the Discoveries browser are now RmlUi

    // documents driven from game_update (game_push_hud). ImGui remains for editor/dev tools.

    // Editor Panel (always visible: contains the "Edit mode active" checkbox)

    build_editor_panel(s);

    // Free-floating per-type Planet Editor window (toggled by the button in the editor panel)

    if (s->render.star_fx.show_planet_editor)

        s->render.star_fx.build_planet_editor();

    // Galaxy Legends browser (chronicle of civilizations; toggled with L)

    if (s->galaxy.show_legends)

        galaxy_history_build_legends(s);

    // Galactic News feed (live events from the ongoing simulation; toggled with N)

    if (s->galaxy.show_news)

        galaxy_history_build_news(s);

    // Live Civ Inspector (owner of the player's current system + reputation; toggled with I)

    if (s->galaxy.show_inspector)

        galaxy_history_build_inspector(s);

    // System Inspector (evolved bodies + chronicle of the current system; editor panel toggle)

    if (s->galaxy.show_system_inspector)

        build_system_inspector(s);

    // Discoveries browser (single-ship discovery log; toggled with O) is now an RmlUi document

    // driven from game_update (game_push_hud); s->show_discoveries still gates its visibility.

    // Dynastic Houses heredity tree (successor kingdoms rising and falling per lineage; toggled with H)

    if (s->galaxy.show_houses)

        galaxy_history_build_houses(s);

    // Government interaction window (Parliament / Royal Court / Sacred Synod / Charter Council),

    // launched from the Live Civ Inspector for the civ that owns the player's current system.

    if (s->galaxy.show_gov_window)

        galaxy_history_build_gov_interaction(s);

    // Transform panel: only in edit mode when an entity is selected

    if (s->editor.edit_mode_active)

        build_transform_panel(s);

    // Per-subsystem CPU profiler panel (collapsible)

    build_profiler_panel(s);

    // Navigation HUD + Ship HUD (arena-side affordance) are now RmlUi documents driven from

    // game_update (game_push_hud), rendered beneath the ImGui editor overlay.

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

