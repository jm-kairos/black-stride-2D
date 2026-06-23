#include "game.h"
#include "text.h"
#include "travel.h"
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
using namespace bs_math;
// =====================================================================================
// Tuning constants.
// =====================================================================================
// ---- Camera / zoom ----
static const f32 ZOOM_MIN          = 0.08f;  // most zoomed-out (global)
static const f32 ZOOM_MAX          = 12.00f; // most zoomed-in
static const f32 ZOOM_START        = 0.50f;  // begins in global mode
static const f32 ZOOM_STEP         = 1.12f;  // multiplicative per wheel notch
static const f32 ZOOM_SYSTEM       = 0.06f;  // system-view zoom (wide enough for 3-planet orbits)
// ---- Star zoom-distance scaling (MODE_SYSTEM) ----
const f32 STAR_MIN_SCREEN_RADIUS = 3.0f;   // px: minimum screen-space radius when zoomed out
static const f32 STAR_DIST_SCALE_FACTOR = 0.0003f;
static const f32 STAR_MAX_DIST_SCALE    = 4.0f;
static const f32 STAR_MAX_STREAK_LENGTH = 20.0f;
static const f32 GALAXY_PLANET_SCALE      = 0.00006f; // visually shrink planet orbits for galaxy view (~5-30 px orbits at default zoom)
static const f32 MIN_SYSTEM_SEPARATION    = 150000000.0f; // 150M: generous spacing so planets/orbits don't crowd neighbors
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
// ---- Engine exhaust tuning -------------------------------------------------------------
static const f32 EXHAUST_BASE_LENGTH = 16.0f;
static const f32 EXHAUST_MAX_EXTRA   = 48.0f;
static const f32 EXHAUST_FLICKER_HZ1 = 30.0f;
static const f32 EXHAUST_FLICKER_HZ2 = 47.3f;
static const f32 EXHAUST_JITTER_AMP  = 0.06f;
// ---- Procedural generation PRNG (splitmix64) ----
static u64 g_rng_state = 0x123456789ABCDEF0ull;
static u64 rng_next() {
    u64 z = (g_rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static f32 rng_f32() { return (f32)(rng_next() & 0xFFFFFF) / (f32)0xFFFFFF; }
static f32 rng_range(f32 min, f32 max) { return min + rng_f32() * (max - min); }
static i32 rng_int(i32 min, i32 max) { return min + (i32)(rng_next() % (u64)(max - min + 1)); }
static const char* SYSTEM_NAMES[] = {
    "Sol", "Alpha Centauri", "Proxima", "Vega", "Sirius",
    "Betelgeuse", "Rigel", "Antares", "Deneb", "Altair",
    "Fomalhaut", "Arcturus", "Aldebaran", "Spica", "Regulus", "Pollux",
    "Castor", "Mizar", "Capella", "Procyon", "Achernar",
    "Hadar", "Acrux", "Gacrux", "Mimosa", "Bellatrix",
    "Elnath", "Alioth", "Dubhe", "Merak", "Phecda",
    "Megrez", "Alkaid", "Alnitak", "Alnilam", "Mintaka",
    "Saiph", "Wezen", "Adhara", "Aludra", "Sargas",
    "Kaus Australis", "Nunki", "Ascella", "Shaula", "Girtab",
    "Marfik", "Izar", "Muphrid", "Caph", "Ruchbah"
};
static const i32 SYSTEM_NAME_COUNT = sizeof(SYSTEM_NAMES) / sizeof(SYSTEM_NAMES[0]);
// ---- Render layers (lower draws first) ----
static const u32 LAYER_STARFIELD_FAR = 0;  // procedural far stars (custom GPU)
static const u32 LAYER_STARFIELD_MID  = 2;  // procedural mid-distance stars (custom GPU)
static const u32 LAYER_MAPPED_SYSTEM  = 3;  // current system star + planets
static const u32 LAYER_SHIP           = 10; // ship sprite art
static const u32 LAYER_CELESTIAL      = 11; // stars, planets, orbit rings (below bloom threshold)
static const u32 LAYER_UI             = 50; // debug overlays above ship, below HUD text
static const u32 LAYER_HUD_TEXT       = 100; // screen-space HUD/UI text -- always on top
// Debug overlays bypass the bloom pipeline (drawn after composite).
static const u32 LAYER_DEBUG = BS_LAYER_BLOOM_THRESHOLD;
static const u32 LAYER_GIZMO = BS_LAYER_BLOOM_THRESHOLD + 1;
// Debug collider outline: a hot magenta loop traced over the exact polygon ships_collide tests.
static const bs_color COLLIDER_COLOR = bs_color{ 1.00f, 0.18f, 0.85f, 1.0f };
// =====================================================================================
// Action Log helpers -- push a formatted message into the rolling 30-entry HUD buffer.
// =====================================================================================
#define ACTION_LOG_MAX 30
#define ACTION_LOG_FADE_AFTER 3.0f // seconds of inactivity before fading begins
#define ACTION_LOG_FADE_OVER  2.0f // seconds to lerp from full opacity to idle
void action_log_push(game_state* s, const char* fmt, ...) {
    if (!s) return;
    // Shift oldest out if at capacity
    if (s->action_log.count == ACTION_LOG_MAX) {
        for (i32 i = 0; i < ACTION_LOG_MAX - 1; ++i)
            memcpy(s->action_log.entries[i], s->action_log.entries[i + 1], 128);
        s->action_log.count = ACTION_LOG_MAX - 1;
    }
    i32 slot = s->action_log.count;
    if (slot < 0) slot = 0;
    if (slot >= ACTION_LOG_MAX) slot = ACTION_LOG_MAX - 1;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->action_log.entries[slot], 128, fmt, args);
    va_end(args);
    s->action_log.entries[slot][127] = '\0';
    s->action_log.count = slot + 1;
    s->action_log.inactivity_timer = 0.0f;
}
// =====================================================================================
// Galaxy coordinate helpers.
// =====================================================================================
// Nearest-system lookup: brute-force distance check over the small cluster.
// Returns index of the closest system, or -1 if count == 0.
i32 find_nearest_system(const HierPos2* ship_pos,
                        const StarSystem* systems, i32 count)
{
    if (count <= 0) return -1;
    i32 best = -1;
    f64 best_dist = 1e300;
    for (i32 i = 0; i < count; ++i) {
        f64 sx, sy, cx, cy;
        hierpos_to_f64(ship_pos, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        hierpos_to_f64(&systems[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &cx, &cy);
        f64 dx = sx - cx;
        f64 dy = sy - cy;
        f64 d = dx * dx + dy * dy;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}
// Convert ship galaxy position to an offset within a specific system's local frame.
// Result is the same Vec2 the current MODE_SYSTEM render code expects.
Vec2 galaxy_to_system_local(const HierPos2* ship_galaxy,
                            const HierPos2* system_center)
{
    f64 sx, sy, cx, cy;
    hierpos_to_f64(ship_galaxy,   BS_HIERPOS_CELL_SIZE, &sx, &sy);
    hierpos_to_f64(system_center, BS_HIERPOS_CELL_SIZE, &cx, &cy);
    return Vec2{ (f32)(sx - cx), (f32)(sy - cy) };
}
// Convert a world-space Vec2 into the galaxy-map HierPos2 coordinate system.
static bs_math::HierPos2 world_to_galaxy_pos(bs_math::Vec2 world) {
    return hierpos_from_vec2(world, BS_HIERPOS_CELL_SIZE);
}
// Determine which zone the player ship occupies in a given star system.
// Zones are concentric rings bounded by planet orbit radii, numbered from
// outside in: Zone 0 = beyond outermost orbit, Zone 1 = between outermost
// and middle orbit, ... Zone N = inside innermost orbit.
static i32 get_system_zone(const game_state* s, i32 system_idx) {
    const StarSystem& sys = s->systems[system_idx];
    Vec2 diff = hierpos_diff(&s->map_entities[0].galaxy_pos, &sys.galaxy_center, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(diff);
    // Collect valid orbit radii
    f32 orbits[5];
    i32 orbit_count = 0;
    for (i32 i = 0; i < sys.planet_count; ++i) {
        if (sys.planets[i].semi_major_axis > 0.0f) {
            orbits[orbit_count++] = sys.planets[i].semi_major_axis;
        }
    }
    // Sort ascending (bubble sort, N <= 5)
    for (i32 i = 0; i < orbit_count; ++i) {
        for (i32 j = i + 1; j < orbit_count; ++j) {
            if (orbits[j] < orbits[i]) {
                f32 tmp = orbits[i]; orbits[i] = orbits[j]; orbits[j] = tmp;
            }
        }
    }
    // Zones are 0-indexed from outside in
    for (i32 i = orbit_count - 1; i >= 0; --i) {
        if (dist > orbits[i]) return (orbit_count - 1 - i);
    }
    return orbit_count; // inside innermost orbit
}
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
    s->lights           = {};
    s->light_ambient    = bs_color{ 0.18f, 0.19f, 0.24f, 1.0f }; // dim cool floor
    s->light_selected   = -1;
    // Default glow params (match hardcoded shader defaults).
    s->glow_params = bs_glow_params{
        1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,
        bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },
        bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },
        bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },
        bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }
    };
    // Bloom defaults (disabled by default until user opts in).
    s->bloom_enabled    = FALSE;
    s->bloom_threshold  = 1.2f;
    s->bloom_intensity  = 0.3f;
    s->dynamic_bloom    = TRUE;
    s->star_light_enabled     = TRUE;
    s->bg_layer0_enabled      = TRUE;
    s->bg_layer1_enabled      = TRUE;
    s->bg_layer2_enabled      = TRUE;
    s->starfield_lod_density    = 0.06f;
    s->starfield_lod_size       = 1.0f;
    s->starfield_lod_brightness = 1.0f;
    s->star_pos = bs_math::Vec2{0,0};
    s->star_dazzle_inner_radius = 5000.0f;
    s->star_dazzle_outer_radius = 15000.0f;
    s->star_dazzle_intensity    = 1.0f;
    s->star_light_intensity_mul = 1.0f;
    s->star_light_radius_mul    = 1.0f;
    // Per-entity glow defaults (all start identical to global defaults).
    s->exhaust_glow = s->glow_params;
    s->bullet_glow  = s->glow_params;
    s->edit_mode_active = FALSE;
    s->edit_selection   = EditSelection{ EDIT_NONE, -1 };
    s->edit_drag        = EditorDrag{ FALSE, EDIT_DRAG_NONE, Vec2{ 0.0f, 0.0f }, Vec2{ 0.0f, 0.0f }, 0.0f };
    s->action_log.count = 0;
    s->action_log.inactivity_timer = 0.0f;
    s->alt_movement_active = FALSE;
    // ---- Fleet: flagship (member 0) loaded from the player hull --------------------------
    s->fleet.init();
    {
        FleetShip& fs = s->fleet.add();
        if (!ship_load(&fs.ship, "assets/ships/mapped_ship.ship")) {
            BS_LOG_FATAL("game_init: failed to load player ship.");
            return FALSE;
        }
        fs.ship.origin = Vec2{ 25000.0f, 0.0f };
        fs.ship.glow   = s->glow_params;
        for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) fs.ship.weapons[i] = nullptr;
        fs.ship.weapons[0]       = weapon_create_ballistic_cannon(fs.ship.faction);
        fs.ship.weapon_count     = 1;
        fs.ship.active_weapon_idx = 0;
    }
    // ---- Fleet: demo escort ships (members 1..N) -----------------------------------------
    {
        const VesselFaction player_faction = s->fleet.flagship().ship.faction;
        const Vec2 escort_offsets[4] = {
            Vec2{ 24000.0f,  1500.0f },
            Vec2{ 24000.0f, -1500.0f },
            Vec2{ 22500.0f,  1500.0f },
            Vec2{ 22500.0f, -1500.0f },
        };
        for (i32 i = 0; i < 4; ++i) {
            FleetShip& fs = s->fleet.add();
            if (!ship_load(&fs.ship, "assets/ships/mapped_ship.ship")) {
                BS_LOG_ERROR("game_init: failed to load escort ship %d.", i);
                continue;
            }
            fs.ship.origin      = escort_offsets[i];
            fs.ship.faction     = player_faction;
            fs.ship.vessel_name = "Escort";
            fs.ship.glow        = s->glow_params;
            for (i32 w = 0; w < SHIP_MAX_WEAPONS; ++w) fs.ship.weapons[w] = nullptr;
            fs.ship.weapons[0]        = weapon_create_ballistic_cannon(fs.ship.faction);
            fs.ship.weapon_count      = 1;
            fs.ship.active_weapon_idx = 0;
        }
    }
    if (!ship_load(&s->enemy_ship, "assets/enemy_ship.ship")) {
        BS_LOG_FATAL("game_init: failed to load enemy ship.");
        return FALSE;
    }
    s->enemy_ship.origin     = Vec2{ 1e4, 0 };
    s->enemy_ship.angle      = 2.36f;
    s->enemy_ship.faction    = VESSEL_PIRATE;
    s->enemy_ship.vessel_name = "Raider-class Interceptor";
    s->enemy_ship.glow = s->glow_params;
    // ---- Enemy weapon inventory ----------------------------------------------------------
    s->enemy_ship.weapon_count = 0;
    s->enemy_ship.active_weapon_idx = -1;
    for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) s->enemy_ship.weapons[i] = nullptr;
    s->enemy_ship.weapons[0] = weapon_create_ballistic_cannon(s->enemy_ship.faction);
    s->enemy_ship.weapon_count = 1;
    s->enemy_ship.active_weapon_idx = 0;
    // ---- Projectile system -----------------------------------------------------------------
    s->projectiles.init();
    // ---- Combat entities -------------------------------------------------------------------
    s->combat_entity_count = 0;
    for (i32 i = 0; i < MAX_COMBAT_ENTITIES; ++i) {
        s->combat_entities[i].active = FALSE;
        s->combat_entities[i].velocity = Vec2{ 0.0f, 0.0f };
    }
    // Register enemy ship as a combat entity.
    {
        CombatEntity* ce = &s->combat_entities[0];
        ce->active   = TRUE;
        ce->position = s->enemy_ship.origin;
        ce->velocity = Vec2{ 0.0f, 0.0f };
        ce->radius   = ship_bounding_radius(&s->enemy_ship);
        ce->faction  = s->enemy_ship.faction;
        ce->hp       = 100.0f;
        ce->ship     = &s->enemy_ship;
        ce->tint     = bs_color{ 1.0f, 0.3f, 0.3f, 1.0f };
        s->combat_entity_count = 1;
    }
    // Register every fleet ship as a combat entity so enemy fire can hit them.
    for (i32 i = 0; i < s->fleet.count() && s->combat_entity_count < MAX_COMBAT_ENTITIES; ++i) {
        FleetShip& fs = s->fleet.at(i);
        CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];
        ce->active   = TRUE;
        ce->position = fs.ship.origin;
        ce->velocity = fs.flight.velocity;
        ce->radius   = ship_bounding_radius(&fs.ship);
        ce->faction  = fs.ship.faction;
        ce->hp       = 100.0f;
        ce->ship     = &fs.ship;
        ce->tint     = bs_color{ 0.3f, 0.8f, 1.0f, 1.0f };
    }
    // Camera starts zoomed out (global mode), centered on the flagship.
    s->camera          = camera2d_default();
    s->camera.zoom     = ZOOM_START;
    s->camera.position = s->player_ship().origin;
    s->mode            = MODE_GLOBAL;
    // Floating-origin camera starts at system 0's galaxy center.
    s->camera_hierpos = s->systems[0].galaxy_center;
    // Drag anchors zeroed so system-view middle-mouse panning starts clean.
    s->system_drag_cam    = Vec2{ 0.0f, 0.0f };
    s->system_drag_world  = Vec2{ 0.0f, 0.0f };
    s->system_zoom        = ZOOM_SYSTEM;
    s->galaxy_map_time    = 0.0f;
    s->map_anim_scale     = true;
    s->map_anim_rotate    = true;
    s->map_anim_alpha     = true;
    s->map_anim_thickness = true;
    s->map_draw_jump_range = FALSE;
    s->map_jump_range      = 5000000.0f;
    s->map_draw_sensor_range = FALSE;
    s->map_sensor_range      = 250000.0f;
    s->map_draw_lanes        = TRUE;
    s->show_metaball_ui       = FALSE;
    s->metaball_radius_factor = 2.0f;
    s->metaball_threshold     = 1.0f;
    s->metaball_grid_w        = 50;
    s->metaball_grid_h        = 40;
    s->time_scale             = 1.0f;
    s->elapsed_time           = 0.0f;
    s->encounter_active       = FALSE;
    s->encounter_was_active   = FALSE;
    s->encounter_can_retrigger = TRUE;
    s->map_recentering     = FALSE;
    s->map_recenter_t      = 0.0f;
    s->map_input_cooldown  = 0.0f;
    s->map_drag_needs_fresh_press = FALSE;
    // ---- Galaxy cluster (50 procedurally generated systems) ------------------------------
    s->system_count = 50;
    Vec2 cluster_center = Vec2{ 0.0f, 0.0f };
    i32 placed = 0;
    u64 seed_attempt = 0;
    while (placed < s->system_count && seed_attempt < 10000)
    {
        // Generate galaxy position candidate first, then check distance.
        g_rng_state = 0x123456789ABCDEF0ull + seed_attempt;
        f32 angle = rng_f32() * 2.0f * BS_PI;
        f32 dist  = rng_range(100000000.0f, 2000000000.0f);
        Vec2 world = vec2_add(cluster_center, Vec2{ cosf(angle) * dist, sinf(angle) * dist });
        HierPos2 galaxy_pos = hierpos_from_vec2(world, BS_HIERPOS_CELL_SIZE);
        // Check minimum separation from already-placed systems
        b8 too_close = FALSE;
        for (i32 j = 0; j < placed; ++j)
        {
            Vec2 diff = hierpos_diff(&galaxy_pos, &s->systems[j].galaxy_center, BS_HIERPOS_CELL_SIZE);
            if (vec2_length(diff) < MIN_SYSTEM_SEPARATION) { too_close = TRUE; break; }
        }
        if (!too_close)
        {
            generate_star_system(&s->systems[placed], seed_attempt, world);
            s->systems[placed].name = SYSTEM_NAMES[placed % SYSTEM_NAME_COUNT];
            ++placed;
        }
        ++seed_attempt;
    }
    if (placed < s->system_count) s->system_count = placed;
    // Home system stays at the cluster center so the ship start position remains valid.
    s->systems[0].galaxy_center = HierPos2{ GridCell{0, 0}, Vec2{0.0f, 0.0f} };
    s->systems[0].name = "Sol";
    // Generate Voronoi diagram for system territories and Delaunay lane connectivity.
    generate_galaxy_voronoi(s->systems, s->system_count, &s->galaxy_voronoi);
    s->current_system = 0;
    s->map_entity_count = 0;
    // Player ship starts in system 0; pre-seed map_entities[0] so the first-frame
    // M-key toggle and find_system_by_cell have valid data. Rebuilt each frame in game_update.
    s->map_entities[0] = MapEntity{ world_to_galaxy_pos(s->player_ship().origin),
                                    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, 12.0f, TRUE, "Player Ship" };
    s->map_entity_count = 1;
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
        s->exhaust_texture = renderer_create_texture(ex_pixels, EX_SIZE, EX_SIZE);
    }
    // Initialize star visual effect system (generates procedural textures).
    s->star_fx.init();
    // Initialize global-mode parallax background (layers + mapped system).
    s->global_background.init(s, &s->star_fx);
    // Resolve sprite textures now that the renderer is live.
    for (i32 i = 0; i < s->fleet.count(); ++i)
        ship_visual_resolve_textures(&s->fleet.at(i).ship.visual);
    ship_visual_resolve_textures(&s->enemy_ship.visual);
    return TRUE;
}
// =====================================================================================
// Update.
// =====================================================================================
static f32 compression_factor(f32 zoom); // defined near game_render
static void update_zoom_and_mode(game_state* s, f32 dt) {
    // In system view zoom is driven by mouse wheel (scroller), zooming toward the mouse cursor.
    if (s->mode == MODE_SYSTEM) {
        i32 wheel = input_get_mouse_wheel();
        if (wheel != 0) {
            f32 old_zoom = s->camera.zoom;
            s->camera.zoom *= powf(ZOOM_STEP, (f32)wheel);
            s->camera.zoom = clampf(s->camera.zoom, 0.000001f, 1.20f);
            // Zoom-to-mouse: shift camera so the world point under the cursor stays under it.
            // Because we apply cosmetic_compression to rendered positions, the visual point
            // under the mouse is compressed. The camera shift must account for the change in
            // compression factor so the same true galaxy point stays under the cursor.
            i32 mx, my;
            input_get_mouse_position(&mx, &my);
            f32 hw = (f32)s->fb_width  * 0.5f;
            f32 hh = (f32)s->fb_height * 0.5f;
            Vec2 mouse_off = Vec2{ (f32)mx - hw, hh - (f32)my };
            f32 k_old = compression_factor(old_zoom);
            f32 k_new = compression_factor(s->camera.zoom);
            // Derived from: screen = (k * true_world - camera.position) * zoom
            // We want the same true_world before and after zoom.
            // C_new = k_new * (C_old + mouse_off/old_zoom) / k_old - mouse_off / new_zoom
            Vec2 C_old = s->camera.position;
            Vec2 C_new = vec2_sub(
                vec2_scale(vec2_add(C_old, vec2_scale(mouse_off, 1.0f / old_zoom)), k_new / k_old),
                vec2_scale(mouse_off, 1.0f / s->camera.zoom));
            s->camera.position = C_new;
        }
        return;
    }
    // Mouse wheel -> multiplicative zoom.
    i32 wheel = input_get_mouse_wheel();
    if (wheel != 0) {
        s->camera.zoom *= powf(ZOOM_STEP, (f32)wheel);
        s->camera.zoom = clampf(s->camera.zoom, ZOOM_MIN, ZOOM_MAX);
    }
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
static Vec2 read_wasd_dir() {
    Vec2 d{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) d.y += 1.0f;
    if (input_is_key_down(KEY_S)) d.y -= 1.0f;
    if (input_is_key_down(KEY_D)) d.x += 1.0f;
    if (input_is_key_down(KEY_A)) d.x -= 1.0f;
    return d;
}
// World-space position under the mouse cursor.
static Vec2 mouse_world(const game_state* s) {
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    return camera2d_screen_to_world(&s->camera, s->fb_width, s->fb_height, Vec2{ (f32)mx, (f32)my });
}
// ---- Edit mode picking -----------------------------------------------------------------
// Even-odd ray-cast point-in-polygon test. `verts` is a closed polygon in world space.
static b8 point_in_polygon(Vec2 p, const Vec2* verts, i32 n) {
    b8 inside = FALSE;
    for (i32 i = 0, j = n - 1; i < n; j = i++) {
        b8 crosses = ((verts[i].y > p.y) != (verts[j].y > p.y)) &&
                     (p.x < (verts[j].x - verts[i].x) * (p.y - verts[i].y) /
                            (verts[j].y - verts[i].y) + verts[i].x);
        if (crosses) inside = !inside;
    }
    return inside;
}
// Return the world position of an edit selection, or {0,0} for EDIT_NONE.
static Vec2 edit_entity_position(const game_state* s, EditSelection sel) {
    switch (sel.kind) {
        case EDIT_LIGHT:
            if (sel.index >= 0 && sel.index < (i32)s->lights.size())
                return s->lights[sel.index].position;
            break;
        case EDIT_SHIP:
            return (sel.index == 0) ? s->player_ship().origin : s->enemy_ship.origin;
        default: break;
    }
    return Vec2{ 0.0f, 0.0f };
}
// Return the world angle (radians) of an edit selection, or 0 for non-ships.
static f32 edit_entity_angle(const game_state* s, EditSelection sel) {
    if (sel.kind == EDIT_SHIP)
        return (sel.index == 0) ? s->player_ship().angle : s->enemy_ship.angle;
    return 0.0f;
}
// Write a new world position back to the selected entity.
static void edit_entity_set_position(game_state* s, EditSelection sel, Vec2 pos) {
    switch (sel.kind) {
        case EDIT_LIGHT:
            if (sel.index >= 0 && sel.index < (i32)s->lights.size())
                s->lights[sel.index].position = pos;
            break;
        case EDIT_SHIP:
            if (sel.index == 0) s->player_ship().origin = pos;
            else                s->enemy_ship.origin     = pos;
            break;
        default: break;
    }
}
// Write a new angle (radians) back to the selected ship.
static void edit_entity_set_angle(game_state* s, EditSelection sel, f32 a) {
    if (sel.kind == EDIT_SHIP) {
        if (sel.index == 0) s->player_ship().angle = a;
        else                s->enemy_ship.angle     = a;
    }
}
// Hit-test the cursor against editable entities (lights first, then ships). Returns the
// selection under the cursor, or {EDIT_NONE, -1} if nothing was hit.
// Lights are picked by their CENTER only (small screen-space tolerance), not by radius.
static EditSelection edit_pick(const game_state* s, Vec2 cursor) {
    // Lights: nearest within a small screen-space tolerance around the center point.
    f32 tol = 20.0f / ((s->camera.zoom > 0.0001f) ? s->camera.zoom : 1.0f);
    f32 tol2 = tol * tol;
    i32 best_light = -1;
    f32 best_d2 = 0.0f;
    for (i32 i = 0; i < (i32)s->lights.size(); ++i) {
        Vec2 d = vec2_sub(cursor, s->lights[i].position);
        f32  d2 = d.x * d.x + d.y * d.y;
        if (d2 <= tol2 && (best_light < 0 || d2 < best_d2)) {
            best_light = i; best_d2 = d2;
        }
    }
    if (best_light >= 0) return EditSelection{ EDIT_LIGHT, best_light };
    // Ships: point-in-polygon against the world-space collider.
    Vec2 corners[SHIP_MAX_COLLIDER_VERTS];
    if (ship_collider_corners(&s->player_ship(), corners) &&
        point_in_polygon(cursor, corners, s->player_ship().collider_count)) {
        return EditSelection{ EDIT_SHIP, 0 };
    }
    if (ship_collider_corners(&s->enemy_ship, corners) &&
        point_in_polygon(cursor, corners, s->enemy_ship.collider_count)) {
        return EditSelection{ EDIT_SHIP, 1 };
    }
    return EditSelection{ EDIT_NONE, -1 };
}
// Gizmo geometry helpers (all in world space).
// All sizes are expressed as target screen pixels, then converted to world units via zoom_inv.
// This keeps gizmos visible at every zoom level.
static f32 gizmo_axis_len(f32 zoom_inv) { return 40.0f * zoom_inv; }
static f32 gizmo_ring_radius_ship(const Ship* ship, f32 zoom_inv) {
    f32 visual_half = vec2_length(vec2_scale(ship->visual.size_local, 0.5f));
    return visual_half + 30.0f * zoom_inv;  // entity bounds + 30 px screen padding
}
static f32 gizmo_ring_radius_light(f32 zoom_inv) { return 40.0f * zoom_inv; }
static f32 gizmo_arrow_size(f32 zoom_inv) { return 8.0f * zoom_inv; }
// Distance from a point to a line segment.
static f32 point_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = vec2_sub(b, a);
    Vec2 ap = vec2_sub(p, a);
    f32 ab2 = ab.x * ab.x + ab.y * ab.y;
    if (ab2 < 0.0001f) return sqrtf(ap.x * ap.x + ap.y * ap.y);
    f32 t = clampf((ap.x * ab.x + ap.y * ab.y) / ab2, 0.0f, 1.0f);
    Vec2 closest = vec2_add(a, vec2_scale(ab, t));
    Vec2 d = vec2_sub(p, closest);
    return sqrtf(d.x * d.x + d.y * d.y);
}
// Test which gizmo part (if any) is under the cursor for the current selection.
// Returns the drag mode that should be used, or EDIT_DRAG_FREE for the entity body.
static EditDragMode edit_pick_gizmo(const game_state* s, Vec2 cursor) {
    if (s->edit_selection.kind == EDIT_NONE) return EDIT_DRAG_NONE;
    Vec2 origin = edit_entity_position(s, s->edit_selection);
    f32 zoom_inv = 1.0f / ((s->camera.zoom > 0.0001f) ? s->camera.zoom : 1.0f);
    f32 axis_len = gizmo_axis_len(zoom_inv);
    f32 tol = 12.0f * zoom_inv;  // world-space tolerance around gizmo lines
    // Rotation ring: narrow band around the ring.
    f32 ring_r = 0.0f;
    if (s->edit_selection.kind == EDIT_SHIP) {
        const Ship* sh = (s->edit_selection.index == 0) ? &s->player_ship() : &s->enemy_ship;
        ring_r = gizmo_ring_radius_ship(sh, zoom_inv);
    } else {
        ring_r = gizmo_ring_radius_light(zoom_inv);
    }
    f32 d_ring = fabsf(vec2_length(vec2_sub(cursor, origin)) - ring_r);
    if (d_ring <= tol * 1.5f) return EDIT_DRAG_ROTATE;
    // Axis arrows.
    Vec2 x_end = vec2_add(origin, Vec2{ axis_len, 0.0f });
    Vec2 y_end = vec2_add(origin, Vec2{ 0.0f, axis_len });
    if (point_to_segment(cursor, origin, x_end) <= tol) return EDIT_DRAG_AXIS_X;
    if (point_to_segment(cursor, origin, y_end) <= tol) return EDIT_DRAG_AXIS_Y;
    return EDIT_DRAG_FREE;
}
// Edit-mode input: left-click selects an entity under the cursor and begins a drag; holding
// the button repositions it; releasing ends the drag. Clicking empty space deselects. Gated
// on bs_imgui_wants_mouse so clicks on the EDITOR PANEL never pick world entities.
static void update_edit_mode(game_state* s) {
    if (!s->edit_mode_active) {
        s->edit_drag.active = FALSE;
        s->edit_drag.mode   = EDIT_DRAG_NONE;
        return;
    }
    if (bs_imgui_wants_mouse()) return; // cursor over a panel; ignore world picks
    Vec2 cursor = mouse_world(s);
    b8 down     = input_is_button_down(BUTTON_LEFT);
    b8 was_down = input_was_button_down(BUTTON_LEFT);
    if (down && !was_down) {
        // Edge: mouse just pressed.
        // If we already have a selection, test gizmos first; otherwise pick a new entity.
        EditDragMode gizmo = EDIT_DRAG_NONE;
        if (s->edit_selection.kind != EDIT_NONE)
            gizmo = edit_pick_gizmo(s, cursor);
        if (gizmo != EDIT_DRAG_NONE) {
            // Gizmo drag started on the currently selected entity.
            s->edit_drag.active        = TRUE;
            s->edit_drag.mode          = gizmo;
            s->edit_drag.drag_anchor   = cursor;
            s->edit_drag.entity_anchor = edit_entity_position(s, s->edit_selection);
            s->edit_drag.entity_angle  = edit_entity_angle(s, s->edit_selection);
        } else {
            // No gizmo hit -> try picking a new entity (or deselect on empty space).
            EditSelection hit = edit_pick(s, cursor);
            s->edit_selection = hit;
            if (hit.kind != EDIT_NONE) {
                s->edit_drag.active        = TRUE;
                s->edit_drag.mode          = EDIT_DRAG_FREE;
                s->edit_drag.drag_anchor   = cursor;
                s->edit_drag.entity_anchor = edit_entity_position(s, hit);
                s->edit_drag.entity_angle  = edit_entity_angle(s, hit);
                if (hit.kind == EDIT_LIGHT) s->light_selected = hit.index;
            } else {
                s->edit_drag.active = FALSE;
                s->edit_drag.mode   = EDIT_DRAG_NONE;
            }
        }
    } else if (down && s->edit_drag.active) {
        // Hold: update position or angle based on the drag mode.
        switch (s->edit_drag.mode) {
            case EDIT_DRAG_FREE: {
                Vec2 delta = vec2_sub(cursor, s->edit_drag.drag_anchor);
                edit_entity_set_position(s, s->edit_selection,
                                         vec2_add(s->edit_drag.entity_anchor, delta));
                break;
            }
            case EDIT_DRAG_AXIS_X: {
                Vec2 pos = s->edit_drag.entity_anchor;
                pos.x += (cursor.x - s->edit_drag.drag_anchor.x);
                edit_entity_set_position(s, s->edit_selection, pos);
                break;
            }
            case EDIT_DRAG_AXIS_Y: {
                Vec2 pos = s->edit_drag.entity_anchor;
                pos.y += (cursor.y - s->edit_drag.drag_anchor.y);
                edit_entity_set_position(s, s->edit_selection, pos);
                break;
            }
            case EDIT_DRAG_ROTATE: {
                Vec2 origin = edit_entity_position(s, s->edit_selection);
                f32 start_a = atan2f(s->edit_drag.drag_anchor.y - origin.y,
                                     s->edit_drag.drag_anchor.x - origin.x);
                f32 cur_a   = atan2f(cursor.y - origin.y,
                                     cursor.x - origin.x);
                f32 new_a   = s->edit_drag.entity_angle + (cur_a - start_a);
                edit_entity_set_angle(s, s->edit_selection, new_a);
                break;
            }
            default: break;
        }
    } else if (!down) {
        s->edit_drag.active = FALSE;
        s->edit_drag.mode   = EDIT_DRAG_NONE;
    }
}
// ---- Ship CONTROL: pilot input -> forces.
// WASD here are NOT screen-relative; thrust is applied along the ship's heading
// (Starsector-style). The ship coasts (no passive drag); speed only changes via thrust
// (W/S/Q/E) or the brake (C). This function mutates only flight velocities, never the pose --
// integration is simulate_ship's job, so the ship keeps moving even when nobody is piloting.
// Returns TRUE if a turn is actively commanded this frame (A/D held) so simulate_ship knows to
// skip auto-stabilizing the spin; FALSE otherwise.
static b8 control_ship_global(game_state* s, FleetShip* pf, f32 dt) {
    if (!pf) return FALSE;
    Ship*       ship = &pf->ship;
    ShipFlight* fl   = &pf->flight;
    // Free camera mode: ship coasts; no pilot input this frame.
    if (s->free_camera_active)
        return FALSE;
    // Heading basis: angle 0 => nose points +Y (up), matching the tilemap's nose-at-top.
    Vec2 fwd   = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle); // forward (nose)
    Vec2 right = vec2_rotate(Vec2{ 1.0f, 0.0f }, ship->angle); // starboard
    f32 strafe = SHIP_ACCEL; // full strafe thrust
    // ---- Linear thrust (accumulate this frame's acceleration along the heading) ----
    Vec2 acc{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) acc = vec2_add(acc, vec2_scale(fwd,   SHIP_ACCEL)); // forward
    if (input_is_key_down(KEY_S)) acc = vec2_add(acc, vec2_scale(fwd,  -SHIP_DECEL)); // reverse
    if (input_is_key_down(KEY_E)) acc = vec2_add(acc, vec2_scale(right, strafe));     // strafe right
    if (input_is_key_down(KEY_Q)) acc = vec2_add(acc, vec2_scale(right,-strafe));     // strafe left
    fl->velocity = vec2_add(fl->velocity, vec2_scale(acc, dt));
    // ---- Brake (C): bleed the current velocity toward zero at the decel rate ----
    if (input_is_key_down(KEY_C)) {
        f32 spd = vec2_length(fl->velocity);
        if (spd > 0.0001f) {
            f32 ns = spd - SHIP_DECEL * dt;
            if (ns < 0.0f) ns = 0.0f;
            fl->velocity = vec2_scale(fl->velocity, ns / spd);
        }
    }
    // ---- Angular: A/D ramp angular velocity toward +/- max. The COMPLEMENTARY auto-stabilize
    // (no turn input -> bleed spin to zero) is deliberately NOT done here; it lives in
    // simulate_ship so it runs in BOTH modes. Reason: simulate_ship integrates angular_velocity
    // into ship->angle every frame regardless of mode, but this control fn only runs while
    // piloting (global). If the stabilizer lived here, spin built up in global mode and carried
    // into local mode would never bleed off and the integrator would rotate the ship forever.
    // Here we only ADD the commanded turn and report whether one was issued so the simulator
    // knows to skip stabilizing the spin this frame.
    f32 turn_in = 0.0f;
    if (s->alt_movement_active) {
        // ---- Mouse-follow mode: smoothly rotate toward the mouse cursor ----
        Vec2 mw = mouse_world(s);
        Vec2 to = vec2_sub(mw, ship->origin);
        if (vec2_length(to) > 0.001f) {
            f32 desired = atan2f(-to.x, to.y); // ship angle 0 = nose points +Y
            f32 diff  = atan2f(sinf(desired - ship->angle), cosf(desired - ship->angle));
            if (fabsf(diff) > 0.01f) {
                // PD controller: proportional pulls toward target;
                // derivative term (current spin) damps overshoot.
                turn_in = clampf(diff * 3.0f - fl->angular_velocity * 1.5f, -1.0f, 1.0f);
            } // else: no turn -> simulate_ship stabilizer bleeds residual spin smoothly
        }
    } else {
        if (input_is_key_down(KEY_A)) turn_in += 1.0f; // turn left (CCW)
        if (input_is_key_down(KEY_D)) turn_in -= 1.0f; // turn right (CW)
    }
    if (turn_in != 0.0f) {
        fl->angular_velocity += turn_in * SHIP_TURN_ACCEL * dt;
        return TRUE;  // a turn is actively commanded this frame
    }
    return FALSE;     // no turn commanded -> simulate_ship will auto-stabilize the spin
}
// ---- Ship SIMULATION: momentum -> motion. Now owned by Fleet::simulate_all (per-ship
// FleetShip::simulate). Every fleet member integrates its own pose every frame; the piloted
// ship uses the pilot's turn_commanded flag while the rest auto-stabilize residual spin.
// =====================================================================================
// Ship-ship collision response (combat). The enemy hull is an inoperative derelict -- an
// IMMOVABLE obstacle -- so no fleet ship can shove it; each fleet ship is pushed OUT of any
// penetration instead. Called AFTER the fleet has integrated its poses: test each fleet hull's
// oriented bounding box against the enemy (ships_collide -> SAT minimum-translation vector).
// On overlap, translate the ship clear by the FULL MTV and cancel the INWARD component of its
// linear velocity, so the ship slides along the hull instead of sticking or phasing through.
static void resolve_ship_collision(game_state* s) {
    for (i32 i = 0; i < s->fleet.count(); ++i) {
        FleetShip& fs = s->fleet.at(i);
        Vec2 mtv{ 0.0f, 0.0f };
        if (!ships_collide(&fs.ship, &s->enemy_ship, &mtv)) continue; // hulls clear
        // Push this ship fully out of the enemy hull (enemy immovable).
        fs.ship.origin = vec2_add(fs.ship.origin, mtv);
        f32 mlen = vec2_length(mtv);
        if (mlen > 1.0e-6f) {
            Vec2 n  = vec2_scale(mtv, 1.0f / mlen);       // unit outward normal (enemy -> ship)
            f32  vn = vec2_dot(fs.flight.velocity, n);    // signed speed along that normal
            if (vn < 0.0f)                                 // moving INTO the hull
                fs.flight.velocity = vec2_sub(fs.flight.velocity, vec2_scale(n, vn)); // kill inward part
        }
    }
}
// World-space origin of the ship the player is currently controlling (the manually-piloted
// ship, defaulting to the flagship). Used for camera follow.
static Vec2 piloted_ship_origin(game_state* s) {
    i32 idx = s->rts_controls.piloted_index();
    if (idx < 0 || idx >= s->fleet.count()) idx = 0;
    return s->fleet.at(idx).ship.origin;
}
// =====================================================================================
// Action Log Panel -- bottom-right HUD. Shows the last 3 messages when idle (fading to
// 15% opacity after 3s inactivity), expands to full 30-entry scrollable history on hover.
// Uses the Consola font via bs_ui_begin_hud_panel.
// =====================================================================================
static void build_action_log_panel(game_state* s, f32 dt) {
    // Update timer even if panel isn't hovered; fade is a global state.
    s->action_log.inactivity_timer += dt;
    f32 alpha = 1.0f;
    if (s->action_log.inactivity_timer > ACTION_LOG_FADE_AFTER) {
        f32 t = (s->action_log.inactivity_timer - ACTION_LOG_FADE_AFTER) / ACTION_LOG_FADE_OVER;
        if (t > 1.0f) t = 1.0f;
        alpha = 1.0f - t * 0.85f; // fade to 0.15 (15%)
    }
    b8 hovered = FALSE;
    // Set background alpha before opening the panel so the whole window fades together.
    bs_ui_push_alpha(alpha);
    if (bs_ui_begin_hud_panel("ACTION LOG", BS_UI_ANCHOR_BOTTOM_RIGHT, 12.0f)) {
        hovered = bs_ui_is_window_hovered();
        if (hovered) {
            alpha = 1.0f;
            s->action_log.inactivity_timer = 0.0f;
        }
        i32 visible = 3; // collapsed: show only newest 3
        if (hovered)
            visible = s->action_log.count; // expanded: show all
        const f32 TEXT_COL[4] = { 0.86f, 0.90f, 0.96f, 1.00f };
        // Draw newest entries at the bottom (natural log order: chronological).
        i32 start = s->action_log.count - visible;
        if (start < 0) start = 0;
        for (i32 i = start; i < s->action_log.count; ++i) {
            bs_ui_text_colored(TEXT_COL[0], TEXT_COL[1], TEXT_COL[2],
                               TEXT_COL[3] * alpha, s->action_log.entries[i]);
        }
    }
    bs_ui_end_hud_panel();
    bs_ui_pop_alpha(); // Alpha
}
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
    if (compression_factor(s->camera.zoom) < 1.0f) return; // extreme zoom-out: skip (see note)
    Vec2 delta = s->camera.position;
    if (delta.x == 0.0f && delta.y == 0.0f) return; // nothing accumulated this frame
    // Absorb the pan into the hierarchical reference (re-canonicalizes cell + local internally).
    s->camera_hierpos  = hierpos_add_f64(&s->camera_hierpos, (f64)delta.x, (f64)delta.y,
                                         BS_HIERPOS_CELL_SIZE);
    s->camera.position = Vec2{ 0.0f, 0.0f };
    // Persistent camera-space anchors must shift by the same -delta to stay valid across the
    // rebase. (system_drag_world is screen-space pixels and is intentionally left untouched.)
    s->system_drag_cam         = vec2_sub(s->system_drag_cam,         delta);
    s->map_recenter_from_pos   = vec2_sub(s->map_recenter_from_pos,   delta);
    s->map_recenter_target_pos = vec2_sub(s->map_recenter_target_pos, delta);
}
b8 game_update(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;
    if (dt > 0.05f) dt = 0.05f; // clamp hitches
    f32 sim_dt = dt * s->time_scale;
    if (sim_dt > 0.05f) sim_dt = 0.05f; // still clamp scaled hitches
    // ---- Encounter detection: blob merge ------------------------------------------------
    {
        f32 threshold = s->metaball_threshold;
        if (threshold < 1.0e-4f) threshold = 1.0e-4f;
        f32 radius_factor = s->metaball_radius_factor;
        f32 r_player = ship_bounding_radius(&s->player_ship()) * radius_factor;
        f32 r_enemy  = ship_bounding_radius(&s->enemy_ship) * radius_factor;
        f32 reach_p = r_player / sqrtf(threshold);
        f32 reach_e = r_enemy  / sqrtf(threshold);
        Vec2 delta = vec2_sub(s->player_ship().origin, s->enemy_ship.origin);
        f32 dist   = vec2_length(delta);
        b8 blobs_merged = (dist < reach_p + reach_e);
        if (blobs_merged && !s->encounter_active && s->encounter_can_retrigger) {
            s->encounter_active = TRUE;
            s->encounter_can_retrigger = FALSE;
            s->time_scale = 0.0f; // pause on encounter
            action_log_push(s, "Encounter detected! Enemy ship nearby.");
        }
        if (!blobs_merged && !s->encounter_active) {
            // Ships separated far enough -- allow re-trigger next approach.
            s->encounter_can_retrigger = TRUE;
        }
        s->encounter_was_active = s->encounter_active;
    }
    s->galaxy_map_time += sim_dt;
    // ---- SHIFT key: toggle alternative mouse-follow movement system in global mode -----------
    if (input_is_key_down(KEY_LSHIFT) && !input_was_key_down(KEY_LSHIFT) && s->mode == MODE_GLOBAL && !s->free_camera_active) {
        s->alt_movement_active = !s->alt_movement_active;
        if (s->alt_movement_active)
            action_log_push(s, "Alternative movement system activated.");
        else
            action_log_push(s, "Alternative movement system deactivated.");
    }
    // ---- M key: toggle between global mode and system view (Slice 1 prototype) ---------------
    if (input_is_key_down(KEY_M) && !input_was_key_down(KEY_M)) {
        if (s->mode != MODE_SYSTEM) {
            s->mode = MODE_SYSTEM;
            s->current_system = find_system_by_cell(&s->map_entities[0].galaxy_pos, &s->galaxy_voronoi, s->systems);
            // Floating-origin camera: anchor to the ship's galaxy position so the map starts centered on the player.
            s->camera_hierpos = s->map_entities[0].galaxy_pos;
            s->camera.position = Vec2{ 0.0f, 0.0f }; // ship at exact render-local center
            // Start at the same zoom the P-key recenter animation targets (0.20f).
            s->camera.zoom = 0.20f;
            action_log_push(s, "'M' key pressed - system mode entered, this is your map !");
        } else {
            s->mode = MODE_GLOBAL;
            s->camera.zoom     = ZOOM_START;
            s->camera.position = piloted_ship_origin(s);
            // Parallax layer tracks current system.
            {
                ParallaxLayer* pl = s->global_background.layers[2];
                if (pl) {
                    MappedSystemLayer* msl = (MappedSystemLayer*)pl;
                    msl->on_system_changed(s->current_system);
                }
            }
            action_log_push(s, "'M' key pressed - global mode entered");
        }
    }
    // ---- P key: free camera toggle in global mode ----------------------------------------
    if (input_is_key_down(KEY_P) && !input_was_key_down(KEY_P) && s->mode == MODE_GLOBAL) {
        if (!s->free_camera_active) {
            s->free_camera_active = TRUE;
            s->free_camera_pos    = s->camera.position;
            action_log_push(s, "Free camera active.");
        } else {
            s->free_camera_active = FALSE;
            s->camera.position    = piloted_ship_origin(s);
            action_log_push(s, "Free camera disabled.");
        }
    }
    // ---- P key: recenter camera on player ship in system view ----------------------------
    if (input_is_key_down(KEY_P) && !input_was_key_down(KEY_P) && s->mode == MODE_SYSTEM) {
        s->map_recentering        = TRUE;
        s->map_recenter_t           = 0.0f;
        action_log_push(s, "'P' key pressed - camera recentering on player ship");
        s->map_recenter_from_pos    = s->camera.position;
        s->map_recenter_from_zoom   = s->camera.zoom;
        Vec2 ship_rel = hierpos_diff(&s->map_entities[0].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
        s->map_recenter_target_pos  = ship_rel; // camera.position == ship_rel puts ship at screen center
    }
    // Cache which system the ship is in (always updated for gameplay logic).
    s->current_system = find_system_by_cell(&s->map_entities[0].galaxy_pos, &s->galaxy_voronoi, s->systems);
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
    if (piloted_idx < 0 || piloted_idx >= s->fleet.count()) piloted_idx = 0;
    if (s->edit_mode_active) {
        // Editing: freeze ALL fleet flight so dragged poses stay put. Kill residual velocity so
        // ships don't drift the instant edit mode is switched off.
        for (i32 i = 0; i < s->fleet.count(); ++i) {
            s->fleet.at(i).flight.velocity = Vec2{ 0.0f, 0.0f };
            s->fleet.at(i).flight.angular_velocity = 0.0f;
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
        s->camera.position = vec2_add(s->camera.position, pan);
        // Middle-mouse drag: screen-space delta -> world delta (no live-camera feedback loop).
        if (!input_is_button_down(BUTTON_MIDDLE)) {
            s->map_drag_needs_fresh_press = FALSE;
        }
        if (!s->map_drag_needs_fresh_press && input_is_button_down(BUTTON_MIDDLE)) {
            if (!input_was_button_down(BUTTON_MIDDLE)) {
                s->system_drag_cam = s->camera.position;
                i32 mx, my;
                input_get_mouse_position(&mx, &my);
                s->system_drag_world = Vec2{ (f32)mx, (f32)my };
            } else {
                i32 mx, my;
                input_get_mouse_position(&mx, &my);
                Vec2 screen_delta = Vec2{ (f32)mx - s->system_drag_world.x,
                                          (f32)my - s->system_drag_world.y };
                Vec2 world_delta = Vec2{ screen_delta.x / s->camera.zoom,
                                        -screen_delta.y / s->camera.zoom };
                s->camera.position = vec2_sub(s->system_drag_cam, world_delta);
            }
        }
    } else if (s->travel_enabled && s->travel.active) {
        // Travel mode: flagship is on rails. Flight controls are overridden.
        if (!s->travel_paused)
            travel_update(&s->travel, sim_dt);
        s->player_ship().origin    = hierpos_to_vec2(&s->travel.current, BS_HIERPOS_CELL_SIZE);
        s->player_flight().velocity = Vec2{ 0.0f, 0.0f };
    } else if (s->mode == MODE_GLOBAL) {
        // Pilot the manually-controlled fleet member directly.
        FleetShip* pf = &s->fleet.at(piloted_idx);
        Ship* psh = &pf->ship;
        turn_commanded = control_ship_global(s, pf, sim_dt);
        // ---- Weapon firing (left click, gated on ImGui not owning the cursor) ------------
        if (!s->edit_mode_active && !s->free_camera_active && !bs_imgui_wants_mouse()) {
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
                        Vec2 mw = mouse_world(s);
                        Vec2 dir = vec2_sub(mw, psh->origin);
                        w->fire(psh->origin, dir, pf->flight.velocity, &s->projectiles);
                    }
                }
            }
        }
    }
    // ---- Update weapons (cooldowns) -----------------------------------------------------
    for (i32 i = 0; i < s->fleet.count(); ++i) {
        Ship& sh = s->fleet.at(i).ship;
        for (i32 w = 0; w < sh.weapon_count; ++w)
            if (sh.weapons[w]) sh.weapons[w]->update(sim_dt);
    }
    for (i32 i = 0; i < s->enemy_ship.weapon_count; ++i) {
        if (s->enemy_ship.weapons[i]) s->enemy_ship.weapons[i]->update(sim_dt);
    }
    // ---- RTS controls update (orders, selection, hover) --------------------------------
    s->rts_controls.update(sim_dt);
    if (!s->edit_mode_active) {
        // AUTOPILOT: drive ordered ships toward their targets. Skip the manually-piloted ship
        // unless the camera is detached (free camera), in which case every ship obeys orders.
        i32 auto_skip = s->free_camera_active ? -1 : piloted_idx;
        s->fleet.update_autopilot(s, sim_dt, auto_skip);
        // SIMULATION: integrate every fleet ship's pose. The piloted ship uses turn_commanded;
        // the rest auto-stabilize residual spin.
        s->fleet.simulate_all(sim_dt, turn_commanded, piloted_idx);
        // Ship-ship collision response runs immediately AFTER the poses are integrated.
        resolve_ship_collision(s);
    }
    // ---- Sync combat entity positions / velocities from their ships --------------------
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active || !ce->ship) continue;
        Vec2 prev_pos = ce->position;
        ce->position = ce->ship->origin;
        ShipFlight* fl = s->fleet.flight_for_ship(ce->ship);
        if (fl) {
            ce->velocity = fl->velocity;
        } else if (sim_dt > 0.0001f) {
            // Derive velocity from position change for ships that don't have a flight state.
            ce->velocity = vec2_scale(vec2_sub(ce->position, prev_pos), 1.0f / sim_dt);
        }
    }
    // ---- Update projectiles -------------------------------------------------------------
    s->projectiles.update(sim_dt);
    // ---- Projectile vs combat entity collision ------------------------------------------
    for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
        Projectile* p = &s->projectiles.pool[pi];
        if (!p->active) continue;
        for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
            CombatEntity* ce = &s->combat_entities[ci];
            if (!ce->active) continue;
            if (ce->faction == p->owner) continue; // don't hit own faction
            f32 dx = ce->position.x - p->position.x;
            f32 dy = ce->position.y - p->position.y;
            f32 dist2 = dx * dx + dy * dy;
            f32 rr = ce->radius + p->radius;
            if (dist2 < rr * rr) {
                b8 hit = TRUE;
                if (ce->ship) {
                    Vec2 corners[SHIP_MAX_COLLIDER_VERTS];
                    if (ship_collider_corners(ce->ship, corners)) {
                        hit = point_in_polygon(p->position, corners, ce->ship->collider_count);
                    }
                }
                if (hit) {
                    p->active = FALSE;
                    --s->projectiles.count;
                    break; // projectile can only hit one entity
                }
            }
        }
    }
    // ---- Sync world entities to galaxy map --------------------------------------------
    // Rebuild the generic map entity list every frame so any world object with a Vec2 position
    // can appear on the galaxy map. Future entities (stations, asteroids, resources) add here.
    s->map_entity_count = 0;
    // Player ship (index 0 -- animated quad is drawn around this entry)
    if (s->map_entity_count < MAX_MAP_ENTITIES) {
        s->map_entities[s->map_entity_count++] = MapEntity{
            world_to_galaxy_pos(s->player_ship().origin),
            bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, 12.0f, TRUE, "Player Ship" };
    }
    // Enemy ship
    if (s->map_entity_count < MAX_MAP_ENTITIES) {
        s->map_entities[s->map_entity_count++] = MapEntity{
            world_to_galaxy_pos(s->enemy_ship.origin),
            bs_color{ 1.0f, 0.3f, 0.3f, 1.0f }, 10.0f, FALSE, "Enemy Ship" };
    }
    // Camera follows the ship origin (global) or the galaxy map (system).
    if (s->mode == MODE_SYSTEM) {
        // ---- Recenter animation (P key) --------------------------------------------------
        if (s->map_recentering) {
            s->map_recenter_t += dt / 0.80f; // ~0.8 second duration
            if (s->map_recenter_t > 1.0f) s->map_recenter_t = 1.0f;
            f32 t = s->map_recenter_t;
            f32 eased = t * t * (3.0f - 2.0f * t); // smoothstep
            s->camera.position = vec2_add(s->map_recenter_from_pos,
                                           vec2_scale(vec2_sub(s->map_recenter_target_pos, s->map_recenter_from_pos), eased));
            s->camera.zoom = s->map_recenter_from_zoom + (0.20f - s->map_recenter_from_zoom) * eased;
            if (t >= 1.0f) {
                s->camera_hierpos  = s->map_entities[0].galaxy_pos;
                s->camera.position = Vec2{ 0.0f, 0.0f };
                s->map_recentering = FALSE;
                s->map_input_cooldown = 1.5f; // freeze pan/drag for 1.5s after re-anchor
                s->map_drag_needs_fresh_press = TRUE; // require fresh middle-mouse press before next drag
            }
        } else {
            // Decrement input cooldown each frame.
            if (s->map_input_cooldown > 0.0f) {
                s->map_input_cooldown -= dt;
                if (s->map_input_cooldown < 0.0f) s->map_input_cooldown = 0.0f;
            }
            // System view: WASD pans the camera in world space (no ship tracking).
            if (s->map_input_cooldown <= 0.0f) {
                Vec2 pan = read_wasd_dir();
                if (pan.x != 0.0f || pan.y != 0.0f) {
                    s->camera.position = vec2_add(s->camera.position, vec2_scale(pan, 1200.0f * dt));
                }
            }
            // Middle-mouse drag panning: click-hold scroll button and move mouse to pan.
            // Uses SCREEN-SPACE delta (constant zoom) to avoid the feedback loop that
            // mouse_world(s) creates: it reads the live camera, which changes every frame.
            //
            // After a recenter (P key), the user must release and re-press middle mouse
            // before a new drag starts; this prevents stale drag anchors from snapping the
            // camera after the floating-origin re-anchors to the ship.
            if (!input_is_button_down(BUTTON_MIDDLE)) {
                s->map_drag_needs_fresh_press = FALSE; // user released -- next press is fresh
            }
            if (s->map_input_cooldown <= 0.0f &&
                !s->map_drag_needs_fresh_press &&
                input_is_button_down(BUTTON_MIDDLE)) {
                if (!input_was_button_down(BUTTON_MIDDLE)) {
                    // Start drag: record camera position and the screen pixel position.
                    s->system_drag_cam = s->camera.position;
                    i32 mx, my;
                    input_get_mouse_position(&mx, &my);
                    s->system_drag_world = Vec2{ (f32)mx, (f32)my }; // screen anchor
                } else {
                    // Continue drag: screen delta -> world delta (zoom only, no live camera).
                    i32 mx, my;
                    input_get_mouse_position(&mx, &my);
                    Vec2 screen_delta = Vec2{ (f32)mx - s->system_drag_world.x,
                                              (f32)my - s->system_drag_world.y };
                    Vec2 world_delta = Vec2{ screen_delta.x / s->camera.zoom,
                                            -screen_delta.y / s->camera.zoom };
                    s->camera.position = vec2_sub(s->system_drag_cam, world_delta);
                }
            }
        }
        // Floating-origin: fold this frame's pan into the hierarchical reference so on-screen
        // geometry is always rendered near the origin in f32 (prevents dashed far-system circles).
        galaxy_camera_rebase(s);
        s->camera.rotation = 0.0f;
    } else {
        // Global mode: camera tracks the ship origin, world-up orientation.
        // While edit mode is active, the camera stays fixed so the user can freely
        // reposition the ship by dragging it across the screen.
        if (!s->free_camera_active && !s->edit_mode_active) {
            s->camera.position = piloted_ship_origin(s);
        }
        s->camera.rotation = 0.0f;
    }
    // ---- Orbital motion (always simulated, only visible in system view) --------------------
    for (i32 sys = 0; sys < s->system_count; ++sys) {
        update_planet_positions(&s->systems[sys], sim_dt);
    }
    return TRUE;
}
// =====================================================================================
// Render.
// =====================================================================================
static void draw_ship_visual(const Ship* ship, f32 alpha, bs_math::Vec3 light_dir) {
    if (!ship || alpha <= 0.001f) return;
    for (i32 i = 0; i < ship->visual.layer_count; ++i) {
        const VisualLayer& vl = ship->visual.layers[i];
        if (vl.kind == VIS_LAYER_SPRITE) {
            if (!vl.texture.id) continue;
            bs_sprite sp{};
            sp.position = ship_local_to_world(ship, vl.offset_local);
            sp.size     = ship->visual.size_local;
            sp.origin   = Vec2{ 0.5f, 0.5f };
            sp.rotation = ship->angle;
            sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            sp.tint          = bs_color{ 1.0f, 1.0f, 1.0f, alpha };
            sp.texture       = vl.texture;
            sp.blend         = BLEND_ALPHA;
            sp.layer         = LAYER_SHIP + vl.z;
            sp.custom        = bs_color{ 1.0f, 0.0f, 0.0f, 0.0f };
            sp.glow_override = &ship->glow;
            renderer_draw_sprite(&sp);
        } else if (vl.kind == VIS_LAYER_MAPPED) {
            if (!vl.texture.id || !vl.normal_map.id || !vl.depth_map.id || !vl.position_map.id) continue;
            bs_mapped_sprite mp{};
            mp.position = ship_local_to_world(ship, vl.offset_local);
            mp.size     = ship->visual.size_local;
            mp.origin   = Vec2{ 0.5f, 0.5f };
            mp.rotation = ship->angle;
            mp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            mp.tint     = bs_color{ 1.0f, 1.0f, 1.0f, alpha };
            mp.diffuse_map  = vl.texture;
            mp.normal_map   = vl.normal_map;
            mp.depth_map    = vl.depth_map;
            mp.position_map = vl.position_map;
            mp.light_dir = light_dir;
            mp.layer    = LAYER_SHIP + vl.z;
            renderer_draw_mapped_sprite(&mp);
        }
    }
}
// Draw a multi-layer additive engine-exhaust jet behind the ship.
// Layers: white-hot core, orange body, red halo.  Per-frame flicker makes it feel turbulent.
// custom.x gates heat distortion, colour temp, and radial glow in sprite.frag.hlsl.
static void draw_engine_exhaust(const Ship* ship, bs_texture exhaust_tex,
                                const bs_glow_params* glow,
                                f32 speed_ratio, f32 alpha, f32 time) {
    if (!ship || alpha <= 0.001f) return;
    // Flicker: two out-of-phase sines for organic turbulence.
    f32 flicker = sinf(time * EXHAUST_FLICKER_HZ1) * 0.5f
                + sinf(time * EXHAUST_FLICKER_HZ2) * 0.25f;
    f32 jitter = 1.0f + flicker * EXHAUST_JITTER_AMP;
    Vec2 fwd = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle);
    f32 rear_offset = ship->visual.size_local.y * 0.5f + 2.0f;
    Vec2 rear = vec2_sub(ship->origin, vec2_scale(fwd, rear_offset * jitter));
    f32 base_h = EXHAUST_BASE_LENGTH + speed_ratio * EXHAUST_MAX_EXTRA;
    f32 base_w = ship->visual.size_local.x * 0.35f;
    // Three layers: core (white, smallest, highest glow), body (orange), halo (red, largest).
    // The soft radial-gradient texture (exhaust_tex) provides natural tapered falloff.
    struct ExhaustLayer { f32 size_mul; f32 glow_mul; bs_color tint; };
    static const ExhaustLayer layers[3] = {
        { 0.35f, 1.5f, bs_color{ 1.00f, 0.95f, 0.85f, 0.90f } }, // core
        { 0.60f, 1.0f, bs_color{ 1.00f, 0.55f, 0.15f, 0.80f } }, // body
        { 0.90f, 0.6f, bs_color{ 1.00f, 0.25f, 0.05f, 0.60f } }, // halo
    };
    for (i32 i = 0; i < 3; ++i) {
        const ExhaustLayer& L = layers[i];
        bs_sprite sp{};
        sp.position = rear;
        sp.size     = Vec2{
            base_w * L.size_mul * jitter * 0.6f,  // taller-than-wide jet
            base_h * L.size_mul * jitter
        };
        sp.origin   = Vec2{ 0.5f, 0.5f };
        sp.rotation = ship->angle;
        sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        sp.tint     = L.tint;
        sp.tint.a  *= alpha * speed_ratio;      // invisible when stationary
        sp.custom   = bs_color{ speed_ratio * L.glow_mul, 0.0f, 0.0f, 0.0f };
        sp.texture  = exhaust_tex;
        sp.blend         = BLEND_ADDITIVE;
        sp.layer           = LAYER_SHIP;
        sp.glow_override   = glow;
        renderer_draw_sprite(&sp);
    }
}
static void draw_collider_outline(const Ship* ship, bs_color color, f32 thickness) {
    if (!ship || ship->collider_count <= 0) return;
    Vec2 k[SHIP_MAX_COLLIDER_VERTS];
    ship_collider_corners(ship, k);
    for (i32 i = 0; i < ship->collider_count; ++i) {
        Vec2 a = k[i];
        Vec2 b = k[(i + 1) % ship->collider_count];
        renderer_draw_line(a, b, thickness, color, LAYER_DEBUG);
    }
}
static void draw_glow_editor(bs_glow_params* gp, const char* id_suffix) {
    char lbl[64];
    auto mk = [&](const char* base) { snprintf(lbl, sizeof(lbl), "%s##%s", base, id_suffix); return lbl; };
    bs_ui_slider_float(mk("Intensity"), &gp->intensity, 0.0f, 3.0f);
    bs_ui_slider_float(mk("Falloff"),   &gp->falloff,   1.0f, 20.0f);
    f32 gt[3] = { gp->glow_tint.r, gp->glow_tint.g, gp->glow_tint.b };
    if (bs_ui_color_edit3(mk("Glow Tint"), gt)) {
        gp->glow_tint.r = gt[0]; gp->glow_tint.g = gt[1]; gp->glow_tint.b = gt[2];
    }
    bs_ui_slider_float(mk("Head Mult"),    &gp->head_mult,     0.0f, 8.0f);
    bs_ui_slider_float(mk("Head Falloff"), &gp->head_falloff,  0.5f, 10.0f);
    bs_ui_slider_float(mk("Head Range"),   &gp->head_range,    0.0f, 1.0f);
    bs_ui_slider_float(mk("Distort Amp"),  &gp->distort_amp,   0.0f, 0.3f);
    bs_ui_slider_float(mk("Wave Speed"),   &gp->wave_speed,    0.0f, 50.0f);
    bs_ui_slider_float(mk("Wave Freq"),    &gp->wave_freq,     0.0f, 30.0f);
    bs_ui_slider_float(mk("Jitter Speed"), &gp->jitter_speed, 0.0f, 100.0f);
    bs_ui_slider_float(mk("Jitter Freq"),  &gp->jitter_freq,   0.0f, 60.0f);
    f32 tc[3] = { gp->temp_cool.r, gp->temp_cool.g, gp->temp_cool.b };
    if (bs_ui_color_edit3(mk("Cool (tail)"), tc)) {
        gp->temp_cool.r = tc[0]; gp->temp_cool.g = tc[1]; gp->temp_cool.b = tc[2];
    }
    f32 tw[3] = { gp->temp_warm.r, gp->temp_warm.g, gp->temp_warm.b };
    if (bs_ui_color_edit3(mk("Warm (mid)"), tw)) {
        gp->temp_warm.r = tw[0]; gp->temp_warm.g = tw[1]; gp->temp_warm.b = tw[2];
    }
    f32 th[3] = { gp->temp_hot.r, gp->temp_hot.g, gp->temp_hot.b };
    if (bs_ui_color_edit3(mk("Hot (head)"), th)) {
        gp->temp_hot.r = th[0]; gp->temp_hot.g = th[1]; gp->temp_hot.b = th[2];
    }
    if (bs_ui_button(mk("Reset Defaults"), TRUE)) {
        *gp = bs_glow_params{
            1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,
            bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },
            bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },
            bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },
            bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }
        };
    }
}
static void build_editor_panel(game_state* s) {
    if (bs_ui_begin_panel("EDITOR PANEL", BS_UI_ANCHOR_TOP_LEFT, 12.0f, BsUiType::BS_UI_TYPE_EDITOR)) {
        // ---- EDIT MODE -------------------------------------------------------------------------
        // When active, left-click selects a ship or light in the world and drag repositions it.
        // Flight simulation is suspended while active so dragged poses stay put.
        const f32 EM[4] = { 0.55f, 0.85f, 0.95f, 1.0f };
        bs_ui_text_colored(EM[0], EM[1], EM[2], EM[3], "EDIT MODE");
        bool edit_on = s->edit_mode_active ? true : false;
        bs_ui_checkbox("Edit mode active", &edit_on);
        s->edit_mode_active = edit_on ? TRUE : FALSE;
        // ---- LIGHTS ----------------------------------------------------------------------------
        // Spawn / remove / edit the editor-managed 2D point lights.
        bs_ui_separator();
        const f32 LT[4] = { 0.95f, 0.85f, 0.55f, 1.0f };
        bs_ui_text_colored(LT[0], LT[1], LT[2], LT[3], "LIGHTS");
        // Scene-global ambient floor.
        f32 amb[3] = { s->light_ambient.r, s->light_ambient.g, s->light_ambient.b };
        if (bs_ui_color_edit3("Ambient##light_amb", amb)) {
            s->light_ambient.r = amb[0]; s->light_ambient.g = amb[1]; s->light_ambient.b = amb[2];
        }
        // Spawn a new light at the camera center (the visible world center), select it.
        if (bs_ui_button("Add Light", TRUE)) {
            bs_light2d nl{};
            nl.position  = s->camera.position;
            nl.radius    = 320.0f;
            nl.intensity = 1.6f;
            nl.color     = bs_color{ 1.00f, 0.92f, 0.78f, 1.0f }; // warm default
            nl.enabled   = TRUE;
            s->lights.push_back(nl);
            s->light_selected = (i32)s->lights.size() - 1;
        }
        // Per-light rows. Collect a remove request and apply it AFTER the loop so we never erase
        // while iterating. Button/widget ids are suffixed "##<i>" to stay unique per row.
        i32 remove_idx = -1;
        for (size_t i = 0; i < s->lights.size(); ++i) {
            bs_light2d& L = s->lights[i];
            char id[32], label[48];
            snprintf(label, sizeof(label), "Light %zu%s", i, ((i32)i == s->light_selected) ? " *" : "");
            bs_ui_text_colored(LT[0], LT[1], LT[2], LT[3], label);
            snprintf(id, sizeof(id), "Select##%zu", i);
            if (bs_ui_button_sized(id, 60.0f, TRUE)) s->light_selected = (i32)i;
            bs_ui_same_line();
            snprintf(id, sizeof(id), "Remove##%zu", i);
            if (bs_ui_button_sized(id, 60.0f, TRUE)) remove_idx = (i32)i;
            bool on = L.enabled ? true : false;
            snprintf(id, sizeof(id), "Enabled##%zu", i);
            bs_ui_checkbox(id, &on);
            L.enabled = on ? TRUE : FALSE;
            snprintf(id, sizeof(id), "Radius##%zu", i);
            bs_ui_slider_float(id, &L.radius, 16.0f, 1200.0f);
            snprintf(id, sizeof(id), "Intensity##%zu", i);
            bs_ui_slider_float(id, &L.intensity, 0.0f, 4.0f);
            f32 col[3] = { L.color.r, L.color.g, L.color.b };
            snprintf(id, sizeof(id), "Color##%zu", i);
            if (bs_ui_color_edit3(id, col)) { L.color.r = col[0]; L.color.g = col[1]; L.color.b = col[2]; }
            bs_ui_separator();
        }
        if (remove_idx >= 0 && remove_idx < (i32)s->lights.size()) {
            s->lights.erase(s->lights.begin() + remove_idx);
            // Keep the selection valid after the shift.
            if (s->light_selected == remove_idx)      s->light_selected = -1;
            else if (s->light_selected > remove_idx)  s->light_selected -= 1;
        }
        // ---- Per-Entity Glow Controls --------------------------------------------------------
        bs_ui_separator();
        const f32 GL[4] = { 1.0f, 0.75f, 0.35f, 1.0f };
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "SHIP GLOW");
        draw_glow_editor(&s->player_ship().glow, "ship");
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "EXHAUST GLOW");
        draw_glow_editor(&s->exhaust_glow, "exhaust");
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "BULLET GLOW");
        draw_glow_editor(&s->bullet_glow, "bullet");
        // Also keep the global fallback editable for entities that don't set an override.
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "GLOBAL GLOW (fallback)");
        draw_glow_editor(&s->glow_params, "global");
        // ---- HDR BLOOM -------------------------------------------------------------------------
        bs_ui_separator();
        const f32 BL[4] = { 0.75f, 0.55f, 0.95f, 1.0f };
        bs_ui_text_colored(BL[0], BL[1], BL[2], BL[3], "HDR BLOOM");
        bool bloom_on = s->bloom_enabled ? true : false;
        bs_ui_checkbox("Enabled##bloom", &bloom_on);
        s->bloom_enabled = bloom_on ? TRUE : FALSE;
        bs_ui_slider_float("Threshold##bloom", &s->bloom_threshold, 0.0f, 2.0f);
        bs_ui_slider_float("Intensity##bloom", &s->bloom_intensity, 0.0f, 2.0f);
        bool dyn_bloom = s->dynamic_bloom ? true : false;
        bs_ui_checkbox("Dynamic (speed-driven)", &dyn_bloom);
        s->dynamic_bloom = dyn_bloom ? TRUE : FALSE;
        // ---- BACKGROUND LAYERS (debug toggles) -----------------------------------------------
        const f32 BG[4] = { 0.75f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(BG[0], BG[1], BG[2], BG[3], "BACKGROUND LAYERS");
        bool layer0_on = s->bg_layer0_enabled ? true : false;
        bool layer1_on = s->bg_layer1_enabled ? true : false;
        bool layer2_on = s->bg_layer2_enabled ? true : false;
        bs_ui_checkbox("Far starfield (p=0.01)", &layer0_on);
        bs_ui_checkbox("Mid starfield (p=0.05)", &layer1_on);
        bs_ui_checkbox("Mapped system (p=0.30)", &layer2_on);
        s->bg_layer0_enabled = layer0_on ? TRUE : FALSE;
        s->bg_layer1_enabled = layer1_on ? TRUE : FALSE;
        s->bg_layer2_enabled = layer2_on ? TRUE : FALSE;
        bs_ui_separator();
        const f32 SF[4] = { 0.55f, 0.85f, 0.95f, 1.0f };
        bs_ui_text_colored(SF[0], SF[1], SF[2], SF[3], "STARFIELD LOD");
        bs_ui_slider_float("Density", &s->starfield_lod_density, 0.0f, 0.5f);
        bs_ui_slider_float("Size", &s->starfield_lod_size, 0.25f, 3.0f);
        bs_ui_slider_float("Brightness", &s->starfield_lod_brightness, 0.0f, 3.0f);
        bs_ui_separator();
        bs_ui_text_colored(SF[0], SF[1], SF[2], SF[3], "STAR DAZZLE");
        bs_ui_slider_float("Inner radius", &s->star_dazzle_inner_radius, 0.0f, 50000.0f);
        bs_ui_slider_float("Outer radius", &s->star_dazzle_outer_radius, 0.0f, 100000.0f);
        bs_ui_slider_float("Intensity", &s->star_dazzle_intensity, 0.0f, 1.0f);
        bool star_light_on = s->star_light_enabled ? true : false;
        bs_ui_checkbox("Star volumetric light", &star_light_on);
        s->star_light_enabled = star_light_on ? TRUE : FALSE;
        if (s->star_light_enabled) {
            bs_ui_slider_float("Star light intensity", &s->star_light_intensity_mul, 0.0f, 4.0f);
            bs_ui_slider_float("Star light radius",    &s->star_light_radius_mul, 0.1f, 4.0f);
        }
        s->star_fx.build_ui();
        // ---- TRAVEL ----------------------------------------------------------------------------
        bs_ui_separator();
        const f32 TR[4] = { 0.55f, 0.95f, 0.75f, 1.0f };
        bs_ui_text_colored(TR[0], TR[1], TR[2], TR[3], "TRAVEL");
        b8 was_travel = s->travel_enabled;
        bool travel_on = s->travel_enabled ? true : false;
        bs_ui_checkbox("Enable Continuous Travel", &travel_on);
        s->travel_enabled = travel_on ? TRUE : FALSE;
        if (s->travel_enabled != was_travel)
            action_log_push(s, "Continuous travel %s", s->travel_enabled ? "enabled" : "disabled");
        if (s->travel_enabled) {
            // Auto-init travel on first enable if not already active.
            if (!s->travel.active && s->travel.progress == 0.0f) {
                travel_init(&s->travel, s->player_ship().origin, Vec2{ 50000.0f, 0.0f });
            }
            bool paused = s->travel_paused ? true : false;
            bs_ui_checkbox("Pause Travel", &paused);
            s->travel_paused = paused ? TRUE : FALSE;
            bs_ui_slider_float("Speed", &s->travel.speed, 0.01f, 2.0f);
            i32 ease_idx = (i32)s->travel.ease_mode;
            if (bs_ui_combo("Ease Mode", &ease_idx, "Linear\0Smoothstep\0Quad In/Out\0")) {
                if (ease_idx >= 0 && ease_idx < TRAVEL_EASE_COUNT)
                    s->travel.ease_mode = (TravelEaseMode)ease_idx;
            }
            bs_ui_separator();
            char prog_buf[48];
            snprintf(prog_buf, sizeof(prog_buf), "Progress: %.1f%%", s->travel.progress * 100.0f);
            bs_ui_text(prog_buf);
            bs_ui_text("Hierarchical Position:");
            char cell_buf[48];
            snprintf(cell_buf, sizeof(cell_buf), "  Cell: (%lld, %lld)",
                     s->travel.current.cell.x, s->travel.current.cell.y);
            bs_ui_text(cell_buf);
            char local_buf[48];
            snprintf(local_buf, sizeof(local_buf), "  Local: (%.1f, %.1f)",
                     s->travel.current.local.x, s->travel.current.local.y);
            bs_ui_text(local_buf);
            char world_buf[64];
            snprintf(world_buf, sizeof(world_buf), "  World: (%.1f, %.1f)",
                     (f32)s->travel.world_x, (f32)s->travel.world_y);
            bs_ui_text(world_buf);
            // Distance to final destination.
            f64 dest_x, dest_y;
            bs_math::hierpos_to_f64(&s->travel.destination, BS_HIERPOS_CELL_SIZE, &dest_x, &dest_y);
            f64 dx = dest_x - s->travel.world_x;
            f64 dy = dest_y - s->travel.world_y;
            f64 dist = sqrt(dx * dx + dy * dy);
            char dist_buf[48];
            snprintf(dist_buf, sizeof(dist_buf), "  Dist to dest: %.1f", (f32)dist);
            bs_ui_text(dist_buf);
            if (bs_ui_button("Reset Travel", s->travel.progress > 0.0f ? TRUE : FALSE)) {
                travel_reset(&s->travel);
            }
        }
        // ---- SYSTEM VIEW -----------------------------------------------------------------------
        bs_ui_separator();
        const f32 SV[4] = { 0.95f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(SV[0], SV[1], SV[2], SV[3], "SYSTEM VIEW");
        bs_ui_checkbox("Animate scale",     &s->map_anim_scale);
        bs_ui_checkbox("Animate rotation",  &s->map_anim_rotate);
        bs_ui_checkbox("Animate alpha",     &s->map_anim_alpha);
        bs_ui_checkbox("Animate thickness", &s->map_anim_thickness);
        bs_ui_checkbox("Draw hyperjump range", &s->map_draw_jump_range);
        if (s->map_draw_jump_range) {
            bs_ui_slider_float("Range (units)", &s->map_jump_range, 0.0f, 10000000.0f);
        }
        bs_ui_checkbox("Draw sensor range", &s->map_draw_sensor_range);
        if (s->map_draw_sensor_range) {
            bs_ui_slider_float("Sensor range (units)", &s->map_sensor_range, 10000.0f, 500000.0f);
        }
        bs_ui_checkbox("Draw system lanes", &s->map_draw_lanes);
        bool show_mb = (bool)s->show_metaball_ui;
        bs_ui_checkbox("Show metaball UI", &show_mb);
        s->show_metaball_ui = (b8)show_mb;
        if (s->show_metaball_ui) {
            bs_ui_slider_float("Radius factor", &s->metaball_radius_factor, 0.5f, 500.0f);
            bs_ui_slider_float("Threshold",     &s->metaball_threshold,     0.1f, 5.0f);
            f32 grid_w_f = (f32)s->metaball_grid_w;
            f32 grid_h_f = (f32)s->metaball_grid_h;
            bs_ui_slider_float("Grid width",  &grid_w_f, 10.0f, 120.0f);
            bs_ui_slider_float("Grid height", &grid_h_f, 10.0f, 90.0f);
            s->metaball_grid_w = (i32)grid_w_f;
            s->metaball_grid_h = (i32)grid_h_f;
        }
        // Zoom is controlled by mouse wheel (scroller) in system view.
    }
    bs_ui_end_panel();
}
// =====================================================================================
// Transform panel: standalone window showing the selected entity's name, world position,
// angle, and HierPos2 galaxy coordinates. Appears in edit mode when an entity is selected.
// =====================================================================================
static void build_transform_panel(game_state* s) {
    if (!s->edit_mode_active || s->edit_selection.kind == EDIT_NONE) return;
    if (bs_ui_begin_panel("TRANSFORM", BS_UI_ANCHOR_TOP_RIGHT, 12.0f, BsUiType::BS_UI_TYPE_EDITOR)) {
        const f32 TF[4] = { 0.95f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(TF[0], TF[1], TF[2], TF[3], "TRANSFORM");
        Vec2  world_pos = Vec2{ 0.0f, 0.0f };
        f32   angle_deg = 0.0f;
        const char* name = "?";
        if (s->edit_selection.kind == EDIT_SHIP) {
            const Ship* sh = (s->edit_selection.index == 0) ? &s->player_ship() : &s->enemy_ship;
            world_pos = sh->origin;
            angle_deg = sh->angle * (180.0f / 3.14159265f);
            name = (sh->vessel_name && sh->vessel_name[0]) ? sh->vessel_name
                 : (s->edit_selection.index == 0) ? "Player Ship" : "Enemy Ship";
        } else if (s->edit_selection.kind == EDIT_LIGHT) {
            const bs_light2d& L = s->lights[s->edit_selection.index];
            world_pos = L.position;
            name = "Light";
        }
        // Entity name
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "Name: %s", name);
        bs_ui_text(name_buf);
        // World position (read-only text)
        char pos_buf[64];
        snprintf(pos_buf, sizeof(pos_buf), "Position: %.1f, %.1f", world_pos.x, world_pos.y);
        bs_ui_text(pos_buf);
        // Angle (ships only)
        if (s->edit_selection.kind == EDIT_SHIP) {
            char ang_buf[48];
            snprintf(ang_buf, sizeof(ang_buf), "Angle: %.1f deg", angle_deg);
            bs_ui_text(ang_buf);
        }
        // HierPos2 galaxy coordinates
        bs_math::HierPos2 gal = world_to_galaxy_pos(world_pos);
        char cell_buf[64];
        snprintf(cell_buf, sizeof(cell_buf), "Sector: %lld, %lld", gal.cell.x, gal.cell.y);
        bs_ui_text(cell_buf);
        char local_buf[64];
        snprintf(local_buf, sizeof(local_buf), "Local: %.1f, %.1f", gal.local.x, gal.local.y);
        bs_ui_text(local_buf);
        bs_ui_separator();
        if (bs_ui_button("Deselect", TRUE)) {
            s->edit_selection = EditSelection{ EDIT_NONE, -1 };
            s->edit_drag.active = FALSE;
            s->edit_drag.mode   = EDIT_DRAG_NONE;
        }
    }
    bs_ui_end_panel();
}
// Distance-based sensor visibility (0..1). Range and dist must be in the SAME units.
f32 sensor_visibility_from_dist(f32 dist, f32 range) {
    if (range <= 0.0f) return 1.0f;
    if (dist >= range) return 0.0f;
    f32 t = dist / range;
    return 1.0f - t * t * t;
}
// Compute sensor visibility (0..1) for an entity at render-local position `pos`.
// 1.0 = fully visible (inside strong sensor zone), 0.0 = outside range.
static f32 get_sensor_visibility(const game_state* s, Vec2 pos) {
    if (!s->map_draw_sensor_range || s->map_sensor_range <= 0.0f || s->map_entity_count == 0)
        return 1.0f;
    Vec2 ship_rel = hierpos_diff(&s->map_entities[0].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(vec2_sub(pos, ship_rel));
    return sensor_visibility_from_dist(dist, s->map_sensor_range);
}
// ---- Metaball movement UI (marching squares) -------------------------------------------
// Each ship emits a scalar field; the contour at threshold=1.0 is drawn via marching squares.
// When ships get close their fields merge into organic blob shapes.
static void draw_ship_metaballs(game_state* s) {
    if (s->mode != MODE_GLOBAL || !s->show_metaball_ui) return;
    i32 gw = s->metaball_grid_w;
    i32 gh = s->metaball_grid_h;
    if (gw < 2 || gh < 2) return;
    f32 threshold = s->metaball_threshold;
    if (threshold < 1.0e-4f) threshold = 1.0e-4f;
    f32 radius_factor = s->metaball_radius_factor;
    // ---- Gather ALL sources unconditionally. ------------------------------------------
    // Every source contributes to the summed field at every sample point; culling a
    // source because it is off-screen changes the field values and makes the contour
    // shape zoom-dependent. renderer_draw_line clips the output segments to the viewport.
    struct MBSource { Vec2 origin; f32 r2; bool enemy; };
    MBSource srcs[2];
    i32 src_count = 0;
    {
        const Ship* ships[2]  = { &s->player_ship(), &s->enemy_ship };
        bool        is_enemy[2] = { false, true };
        for (i32 i = 0; i < 2; ++i) {
            f32 r = ship_bounding_radius(ships[i]) * radius_factor;
            if (r <= 0.0f) continue;
            srcs[src_count++] = MBSource{ ships[i]->origin, r * r, is_enemy[i] };
        }
    }
    if (src_count == 0) return;
    // ---- Grid AABB = camera viewport, padded. ----------------------------------------
    // The grid samples the scalar field over the visible region at a fixed cell count,
    // so it is always well-resolved on screen. The field itself is the SUM of all sources
    // (none culled), so the contour at any given world point is invariant to zoom — we
    // simply sample a different window of that same field.
    f32 vis_w = (f32)s->fb_width  / s->camera.zoom;
    f32 vis_h = (f32)s->fb_height / s->camera.zoom;
    const f32 MARGIN = 0.15f;
    f32 pad_x = vis_w * MARGIN;
    f32 pad_y = vis_h * MARGIN;
    f32 min_x = s->camera.position.x - vis_w * 0.5f - pad_x;
    f32 max_x = s->camera.position.x + vis_w * 0.5f + pad_x;
    f32 min_y = s->camera.position.y - vis_h * 0.5f - pad_y;
    f32 max_y = s->camera.position.y + vis_h * 0.5f + pad_y;
    f32 cell_w = (max_x - min_x) / (f32)gw;
    f32 cell_h = (max_y - min_y) / (f32)gh;
    static const i32 MAX_GW = 120;
    static const i32 MAX_GH = 90;
    if (gw > MAX_GW) gw = MAX_GW;
    if (gh > MAX_GH) gh = MAX_GH;
    static const i32 MAX_GRID_CORNERS = (MAX_GW + 1) * (MAX_GH + 1);
    static f32 samples[MAX_GRID_CORNERS];
    // Evaluate scalar field at each grid corner (sum of contributing ships only).
    auto sample_idx = [&](i32 gx, i32 gy) -> i32 { return gy * (gw + 1) + gx; };
    for (i32 gy = 0; gy <= gh; ++gy) {
        for (i32 gx = 0; gx <= gw; ++gx) {
            Vec2 pos{ min_x + (f32)gx * cell_w, min_y + (f32)gy * cell_h };
            f32 val = 0.0f;
            for (i32 i = 0; i < src_count; ++i) {
                Vec2 d = vec2_sub(pos, srcs[i].origin);
                val += srcs[i].r2 / (d.x * d.x + d.y * d.y + 0.0001f);
            }
            samples[sample_idx(gx, gy)] = val;
        }
    }
    // Marching-squares edge pairs per case (corrected). Edge indices: 0=left, 1=right,
    // 2=top, 3=bottom. Edges connect: left=TL-BL, right=TR-BR, top=TL-TR, bottom=BL-BR.
    // Case mask: TL=1, TR=2, BR=4, BL=8.
    static const i32 MS_EDGE_PAIRS[16][5] = {
        /* 0  ....  */ {0, -1,-1, -1,-1},
        /* 1  TL    */ {1,  0, 2, -1,-1},
        /* 2  TR    */ {1,  2, 1, -1,-1},
        /* 3  TL TR */ {1,  0, 1, -1,-1},
        /* 4  BR    */ {1,  1, 3, -1,-1},
        /* 5  TL BR */ {2,  0, 2,  1, 3},
        /* 6  TR BR */ {1,  2, 3, -1,-1},
        /* 7  ~BL   */ {1,  0, 3, -1,-1},
        /* 8  BL    */ {1,  0, 3, -1,-1},
        /* 9  TL BL */ {1,  2, 3, -1,-1},
        /* 10 TR BL */ {2,  2, 1,  0, 3},
        /* 11 ~BR   */ {1,  1, 3, -1,-1},
        /* 12 BR BL */ {1,  0, 1, -1,-1},
        /* 13 ~TR   */ {1,  2, 1, -1,-1},
        /* 14 ~TL   */ {1,  0, 2, -1,-1},
        /* 15 ####  */ {0, -1,-1, -1,-1},
    };
    bs_color player_col = {0.2f, 0.85f, 1.0f, 0.6f};
    bs_color enemy_col  = {1.0f, 0.4f,  0.2f, 0.6f};
    bool has_enemy = false;
    Vec2 enemy_origin{0.0f, 0.0f};
    Vec2 player_origin = s->player_ship().origin;
    for (i32 i = 0; i < src_count; ++i) {
        if (srcs[i].enemy) { has_enemy = true; enemy_origin = srcs[i].origin; }
        else               { player_origin = srcs[i].origin; }
    }
    for (i32 gy = 0; gy < gh; ++gy) {
        for (i32 gx = 0; gx < gw; ++gx) {
            f32 bl = samples[sample_idx(gx,   gy  )];
            f32 br = samples[sample_idx(gx+1, gy  )];
            f32 tl = samples[sample_idx(gx,   gy+1)];
            f32 tr = samples[sample_idx(gx+1, gy+1)];
            i32 mask = ((tl > threshold) ? 1 : 0)
                     | ((tr > threshold) ? 2 : 0)
                     | ((br > threshold) ? 4 : 0)
                     | ((bl > threshold) ? 8 : 0);
            const i32* ep = MS_EDGE_PAIRS[mask];
            i32 seg_count = ep[0];
            if (seg_count == 0) continue;
            f32 x0 = min_x + (f32)gx * cell_w;
            f32 y0 = min_y + (f32)gy * cell_h;
            f32 x1 = x0 + cell_w;
            f32 y1 = y0 + cell_h;
            // Linear interpolation along each edge to find exact threshold crossing.
            auto lerp_edge = [&](f32 v0, f32 v1) -> f32 {
                f32 denom = v1 - v0;
                if (fabsf(denom) < 0.0001f) return 0.5f;
                f32 t = (threshold - v0) / denom;
                return clampf(t, 0.0f, 1.0f);
            };
            f32 left_t   = lerp_edge(bl, tl);
            f32 right_t  = lerp_edge(br, tr);
            f32 bottom_t = lerp_edge(bl, br);
            f32 top_t    = lerp_edge(tl, tr);
            Vec2 v_left   = { x0, y0 + left_t   * cell_h };
            Vec2 v_right  = { x1, y0 + right_t  * cell_h };
            Vec2 v_bottom = { x0 + bottom_t * cell_w, y0 };
            Vec2 v_top    = { x0 + top_t    * cell_w, y1 };
            Vec2 edge_verts[4] = { v_left, v_right, v_top, v_bottom };
            // Color this cell by the nearer ship (cell center), so merged blobs read with the
            // right team color on each side of the bridge.
            bs_color seg_col = player_col;
            if (has_enemy) {
                Vec2 cc{ x0 + cell_w * 0.5f, y0 + cell_h * 0.5f };
                Vec2 dp = vec2_sub(cc, player_origin);
                Vec2 de = vec2_sub(cc, enemy_origin);
                if (de.x*de.x + de.y*de.y < dp.x*dp.x + dp.y*dp.y) seg_col = enemy_col;
            }
            for (i32 si = 0; si < seg_count; ++si) {
                i32 ea = ep[1 + si*2];
                i32 eb = ep[1 + si*2 + 1];
                if (ea >= 0 && eb >= 0)
                    renderer_draw_line(edge_verts[ea], edge_verts[eb], 1.5f, seg_col, LAYER_UI);
            }
        }
    }
}
// ---- Time-control panel: top-center toggle for pause/resume (visible in ALL modes) ----
static void draw_time_control_panel(game_state* s) {
    if (bs_ui_begin_panel("TIME", BS_UI_ANCHOR_TOP_CENTER, 12.0f, BsUiType::BS_UI_TYPE_GAME)) {
        const char* label = (s->time_scale == 0.0f) ? "Resume" : "Pause";
        if (bs_ui_button(label, TRUE)) {
            s->time_scale = (s->time_scale == 0.0f) ? 1.0f : 0.0f;
            action_log_push(s, (s->time_scale == 0.0f) ? "Game paused." : "Game resumed.");
        }
    }
    bs_ui_end_panel();
}
// Scalar compression factor: 1.0 at normal zoom, shrinking toward 0.15 at min zoom.
static f32 compression_factor(f32 zoom) {
    const f32 threshold = 0.02f;
    const f32 min_zoom  = 0.000004f;
    if (zoom >= threshold) return 1.0f;
    f32 t = (zoom - min_zoom) / (threshold - min_zoom);
    t = t * t * (3.0f - 2.0f * t);         // smoothstep
    return 0.15f + 0.85f * t;
}
// Compress camera-relative positions toward the origin when zoomed out.
// This is purely cosmetic: true galaxy positions remain unchanged.
static Vec2 cosmetic_compress(Vec2 pos, f32 zoom) {
    return vec2_scale(pos, compression_factor(zoom));
}
b8 game_render(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;
    s->elapsed_time += dt;
    // Volumetric star light accumulator (filled in MODE_SYSTEM, consumed at end of frame)
    bs_light2d star_light{};
    b8 has_star_light = FALSE;
    renderer_set_camera(s->camera);
    // ---- System view render (MODE_SYSTEM) -- all star systems visible at once ----------------
    if (s->mode == MODE_SYSTEM) {
        // Update hovered cell (skip when cursor is over UI panels).
        if (!bs_imgui_wants_mouse()) {
            Vec2 mw = mouse_world(s);
            update_cell_hover_effect(&s->galaxy_voronoi, dt, mw, &s->camera_hierpos, s->camera.zoom, s->systems);
        }
        // Draw Delaunay dual lanes (natural connectivity from Voronoi diagram).
        if (s->map_draw_lanes) {
            bs_color lane_col = bs_color{ 0.25f, 0.40f, 0.55f, 0.35f };
            draw_delaunay_lanes(&s->galaxy_voronoi, s->systems, s, lane_col, 1.0f);
        }
        // Draw Voronoi cell wireframe edges (territory boundaries).
        bs_color vedge_col = bs_color{ 0.45f, 0.55f, 0.70f, 0.12f };
        draw_voronoi_edges(&s->galaxy_voronoi, s, vedge_col, 1.0f);
        // Overlay hovered cell with rotating neon-purple trail.
        bs_color hover_col = bs_color{ 0.55f, 0.20f, 1.00f, 1.00f };
        draw_cell_hover_effect(&s->galaxy_voronoi, s, hover_col);
        // Draw every star system at its camera-relative galaxy position.
        // ---- Pass 1: Stars only (aux bloom eligible for streaks) ----
        renderer_set_aux_bloom_mode(s->star_fx.streak_enabled);
        // Star-responsive streak params: derive multipliers from the current/nearest system.
        if (s->current_system >= 0 && s->current_system < s->system_count)
        {
            StarSystem& ss = s->systems[s->current_system];
            f32 length_mul = clampf(0.5f + ss.star.radius / 1500.0f, 0.5f, 2.0f);
            bs_color c = ss.star.color;
            f32 luminance = 0.3f * c.r + 0.6f * c.g + 0.1f * c.b;
            f32 intensity_mul = 0.4f + 0.6f * luminance;
            s->star_fx.streak_length_mul = length_mul;
            s->star_fx.streak_intensity_mul = intensity_mul;
        }
        else
        {
            s->star_fx.streak_length_mul = 1.0f;
            s->star_fx.streak_intensity_mul = 1.0f;
        }
        for (i32 sys = 0; sys < s->system_count; ++sys) {
            StarSystem& ss = s->systems[sys];
            Vec2 sys_pos_raw = hierpos_diff(&ss.galaxy_center, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            f32 vis = get_sensor_visibility(s, sys_pos_raw);
            Vec2 sys_pos = cosmetic_compress(sys_pos_raw, s->camera.zoom);
            Vec2 star_pos = vec2_add(sys_pos, ss.star.position);
            f32 base_r = ss.star.radius * (0.3f + 0.7f * vis);
            f32 screen_r = base_r * s->camera.zoom;
            f32 zoom_scale = (screen_r < STAR_MIN_SCREEN_RADIUS)
                ? (STAR_MIN_SCREEN_RADIUS / screen_r) : 1.0f;
            Vec2 star_screen = camera2d_world_to_screen(&s->camera, s->fb_width, s->fb_height, star_pos);
            Vec2 screen_center = Vec2{ (f32)s->fb_width * 0.5f, (f32)s->fb_height * 0.5f };
            f32 dist_from_center = vec2_length(vec2_sub(star_screen, screen_center));
            f32 dist_scale = 1.0f + dist_from_center * STAR_DIST_SCALE_FACTOR;
            dist_scale = fminf(dist_scale, STAR_MAX_DIST_SCALE);
            f32 total_scale = zoom_scale * dist_scale;
            f32 scaled_base_r = base_r * total_scale;
            f32 screen_radius = scaled_base_r * s->camera.zoom;
            renderer_set_streak_source(star_screen);
            s->star_fx.draw_star(ss, star_pos, star_screen, scaled_base_r, screen_radius, vis,
                                 s->galaxy_map_time, LAYER_CELESTIAL,
                                 s->fb_width, s->fb_height, total_scale);
        }
        renderer_set_aux_bloom_mode(FALSE);
        // ---- Pass 2: Labels, planets, orbit rings, and star light ----
        for (i32 sys = 0; sys < s->system_count; ++sys) {
            StarSystem& ss = s->systems[sys];
            Vec2 sys_pos_raw = hierpos_diff(&ss.galaxy_center, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            f32 vis = get_sensor_visibility(s, sys_pos_raw);
            Vec2 sys_pos = cosmetic_compress(sys_pos_raw, s->camera.zoom);
            Vec2 star_pos = vec2_add(sys_pos, ss.star.position);
            // System name label above the star (only when clearly visible)
            if (ss.name && ss.name[0] && vis > 0.5f) {
                Vec2 screen = camera2d_world_to_screen(&s->camera, s->fb_width, s->fb_height, star_pos);
                f32 zoom_factor = 0.006f / s->camera.zoom;
                f32 font_scale = 1.0f + 0.25f * (zoom_factor - 1.0f);
                font_scale = clampf(font_scale, 0.8f, 1.6f);
                bs_color label_col = bs_color{ 0.90f, 0.92f, 0.96f, 0.85f * vis };
                bs_ui_label_at(ss.name, screen.x, screen.y - ss.star.radius * s->camera.zoom - 4.0f,
                               font_scale, label_col, ss.name);
            }
            // Planets + orbit rings — draw true elliptical orbit paths.
            f32 comp = compression_factor(s->camera.zoom);
            f32 inv_zoom = 1.0f / s->camera.zoom;
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
                    Vec2 pt = vec2_add(sys_pos, vec2_scale(rot, comp));
                    if (!first) {
                        renderer_draw_line(prev, pt, 1.0f, ring_col, LAYER_CELESTIAL);
                    }
                    prev = pt;
                    first = FALSE;
                }
                Vec2 planet_off = vec2_scale(p.position, comp);
                Vec2 planet_vis = vec2_add(sys_pos, planet_off);
                renderer_draw_circle(planet_vis, 2.0f * inv_zoom, 16, 1.0f, planet_col, LAYER_CELESTIAL);
                max_orbit = fmaxf(max_orbit, p.semi_major_axis);
            }
            // ---- Test sprites: colored dots orbiting the CURRENT star (volumetric light demo)
            if (sys == s->current_system) {
                const i32 TEST_COUNT = 8;
                for (i32 ti = 0; ti < TEST_COUNT; ++ti) {
                    f32 t_angle = (f32)ti / (f32)TEST_COUNT * 2.0f * BS_PI + s->galaxy_map_time * 0.3f;
                    f32 t_orbit = max_orbit * 0.3f + max_orbit * 0.7f * ((f32)ti / (f32)TEST_COUNT);
                    Vec2 tpos = Vec2{
                        star_pos.x + cosf(t_angle) * t_orbit * comp,
                        star_pos.y + sinf(t_angle) * t_orbit * comp
                    };
                    bs_color tcol = ss.star.color;
                    tcol.a = vis * 0.9f;
                    renderer_draw_circle(tpos, 3.0f * inv_zoom, 8, 2.0f, tcol, LAYER_CELESTIAL);
                }
            }
            // Register current system's star as a point light for volumetric illumination
            if (s->star_light_enabled && sys == s->current_system) {
                star_light = make_star_light(star_pos, ss.star.color, max_orbit, comp, vis,
                                              s->star_light_intensity_mul, s->star_light_radius_mul);
                has_star_light = TRUE;
            }
        }
        // ---- Galaxy map entities ---------------------------------------------------------
        for (i32 i = 0; i < s->map_entity_count; ++i) {
            Vec2 pos = hierpos_diff(&s->map_entities[i].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            pos = cosmetic_compress(pos, s->camera.zoom);
            // Player ship (index 0) is always fully visible; others fade with sensor range
            f32 vis = (i == 0) ? 1.0f : get_sensor_visibility(s, pos);
            bs_color ent_col = s->map_entities[i].color;
            ent_col.a *= vis;
            renderer_draw_circle(pos, s->map_entities[i].radius * (0.3f + 0.7f * vis), 8, 2.0f,
                                 ent_col, LAYER_UI);
        }
        // Animated quad around the player ship (index 0, has_outline == TRUE)
        if (s->map_entity_count > 0 && s->map_entities[0].has_outline) {
            Vec2 player_gal = hierpos_diff(&s->map_entities[0].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            player_gal = cosmetic_compress(player_gal, s->camera.zoom);
            // Animation parameters
            f32 t = s->galaxy_map_time;
            f32 scale_mul  = s->map_anim_scale     ? (1.0f + 0.30f * sinf(t * 2.0f)) : 1.0f;
            f32 angle      = s->map_anim_rotate    ? (t * 1.5f) : 0.0f;
            f32 fill_alpha = s->map_anim_alpha     ? (0.45f + 0.35f * sinf(t * 3.0f)) : 0.80f;
            f32 out_alpha  = s->map_anim_alpha     ? (0.70f + 0.25f * sinf(t * 3.0f)) : 0.90f;
            f32 thick_mul  = s->map_anim_thickness ? (1.0f + 0.50f * sinf(t * 4.0f)) : 1.0f;
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
        if (s->map_draw_jump_range) {
            Vec2 ship_rel = hierpos_diff(&s->map_entities[0].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            ship_rel = cosmetic_compress(ship_rel, s->camera.zoom);
            f32 r = s->map_jump_range;
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
        if (s->map_draw_sensor_range && s->map_entity_count > 0) {
            Vec2 ship_rel = hierpos_diff(&s->map_entities[0].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            ship_rel = cosmetic_compress(ship_rel, s->camera.zoom);
            constexpr u32 SENSOR_RING_COUNT = 20;
            constexpr f32 SENSOR_BASE_ALPHA = 0.45f;
            bs_color base_col = bs_color{ 0.45f, 0.90f, 0.40f, 0.0f };
            for (u32 ring = 1; ring <= SENSOR_RING_COUNT; ++ring) {
                f32 t = (f32)ring / (f32)SENSOR_RING_COUNT;
                f32 r = s->map_sensor_range * t;
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
        for (i32 i = 0; i < s->map_entity_count; ++i) {
            Vec2 rel = hierpos_diff(&s->map_entities[i].galaxy_pos, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            rel = cosmetic_compress(rel, s->camera.zoom);
            Vec2 screen = camera2d_world_to_screen(&s->camera, s->fb_width, s->fb_height, rel);
            f32 dx = (f32)mx - screen.x;
            f32 dy = (f32)my - screen.y;
            f32 hit_r = s->map_entities[i].radius * s->camera.zoom + 8.0f;
            if (dx * dx + dy * dy <= hit_r * hit_r) {
                hovered = &s->map_entities[i];
                break; // first match wins
            }
        }
        if (hovered) {
            f64 ax, ay, bx, by;
            hierpos_to_f64(&hovered->galaxy_pos, BS_HIERPOS_CELL_SIZE, &ax, &ay);
            hierpos_to_f64(&s->map_entities[0].galaxy_pos, BS_HIERPOS_CELL_SIZE, &bx, &by);
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
    }
    // ---- Global mode parallax background (layers back-to-front) ------------------------
    if (s->mode == MODE_GLOBAL) {
        // Update current star world position for the dazzle effect.
        if (s->current_system >= 0 && s->current_system < s->system_count) {
            StarSystem& ss = s->systems[s->current_system];
            Vec2 sys_pos_raw = hierpos_diff(&ss.galaxy_center, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
            Vec2 sys_pos = cosmetic_compress(sys_pos_raw, s->camera.zoom);
            s->star_pos = vec2_add(sys_pos, ss.star.position);
        } else {
            s->star_pos = bs_math::Vec2{0,0};
        }
        s->global_background.draw(s->camera, s->fb_width, s->fb_height, dt, s->elapsed_time);
    }
    // ---- Movable 2D point lights (editor-managed). Submit the whole list each frame; the backend
    // accumulates them per-pixel over light_ambient. Sprite layers >= LAYER_UI (nav overlay + HUD
    // text + the light markers below) render fullbright so the UI stays readable. An empty list
    // renders the scene fullbright (lighting is opt-in via the EDITOR PANEL's "Add Light").
    // ---- Compute dynamic bloom / glow from ship speed -------------------
    f32 speed_ratio = 0.0f;
    if (s->mode != MODE_SYSTEM) {
        f32 speed = vec2_length(s->player_flight().velocity);
        speed_ratio = clampf(speed / SHIP_MAX_SPEED, 0.0f, 1.0f);
    }
    bs_glow_params render_glow = s->glow_params;
    f32 render_bloom_intensity = s->bloom_intensity;
    if (s->dynamic_bloom) {
        render_glow.intensity += speed_ratio * 1.0f;
        render_bloom_intensity += speed_ratio * 0.5f;
    }
    // Build frame-local light array: star light first (when in system view), then editor lights
    bs_light2d frame_lights[16];
    u32 frame_light_count = 0;
    if (has_star_light) {
        frame_lights[0] = star_light;
        frame_light_count = 1;
    }
    for (u32 i = 0; i < s->lights.size() && frame_light_count < 16; ++i) {
        frame_lights[frame_light_count++] = s->lights[i];
    }
    // When a star light is active, boost ambient so the galaxy map stays visible
    // (the default ambient is ~0.2, which makes the map 80% darker than fullbright).
    bs_color frame_ambient = s->light_ambient;
    if (has_star_light) {
        frame_ambient = bs_color{ 0.85f, 0.88f, 0.95f, 1.0f };
    }
    renderer_set_lights(frame_light_count > 0 ? frame_lights : nullptr,
                        frame_light_count, frame_ambient, LAYER_UI);
    renderer_set_glow_params(&render_glow);
    renderer_set_bloom_enabled(s->bloom_enabled);
    renderer_set_bloom_params(s->bloom_threshold, render_bloom_intensity);
    // ---- Ship rendering (skipped in system view) -------------------------
    if (s->mode != MODE_SYSTEM) {
        // Directional star light: from each ship toward the current star. For a distant star
        // all ships get roughly the same direction, but we compute it per ship to stay correct.
        bs_math::Vec3 default_light_dir = bs_math::Vec3{ 0.0f, -1.0f, 0.2f };
        // Compute a fleet-wide light direction from the player ship to the star as a fallback.
        bs_math::Vec2 to_star = vec2_sub(s->star_pos, s->player_ship().origin);
        f32 star_dist = vec2_length(to_star);
        bs_math::Vec3 fleet_light_dir = default_light_dir;
        if (star_dist > 0.001f) {
            bs_math::Vec2 d = vec2_scale(to_star, 1.0f / star_dist);
            fleet_light_dir = bs_math::Vec3{ d.x, d.y, 0.2f };
        }

        // Render each fleet ship with its own per-ship speed ratio.
        for (i32 i = 0; i < s->fleet.count(); ++i) {
            const Ship* ship = &s->fleet.at(i).ship;
            f32 ship_speed = vec2_length(s->fleet.at(i).flight.velocity);
            f32 ship_speed_ratio = clampf(ship_speed / SHIP_MAX_SPEED, 0.0f, 1.0f);

            bs_math::Vec2 ship_to_star = vec2_sub(s->star_pos, ship->origin);
            f32 dist = vec2_length(ship_to_star);
            bs_math::Vec3 light_dir = fleet_light_dir;
            if (dist > 0.001f) {
                bs_math::Vec2 d = vec2_scale(ship_to_star, 1.0f / dist);
                light_dir = bs_math::Vec3{ d.x, d.y, 0.2f };
            }

            draw_ship_visual(ship, 1.0f, light_dir);
            draw_engine_exhaust(ship, s->exhaust_texture, &s->exhaust_glow,
                                ship_speed_ratio, 1.0f, s->elapsed_time);
            // ---- DEBUG collider overlay.
            draw_collider_outline(ship, COLLIDER_COLOR, 1.5f);
        }
        f32 enemy_alpha = 1.0f;
        if (s->mode == MODE_GLOBAL && s->map_draw_sensor_range) {
            f32 enemy_dist = vec2_length(vec2_sub(s->enemy_ship.origin, s->player_ship().origin));
            f32 enemy_vis  = sensor_visibility_from_dist(enemy_dist, s->map_sensor_range);
            enemy_alpha *= enemy_vis;
        }
        draw_ship_visual(&s->enemy_ship, enemy_alpha, fleet_light_dir);
        draw_collider_outline(&s->enemy_ship, COLLIDER_COLOR, 1.5f);
        // ---- RTS controls overlay (hover selection, etc.) ------------------------------------
        s->rts_controls.draw();
        // ---- Combat entities (non-ship targets draw as quads) -------------------------------
        for (i32 i = 0; i < s->combat_entity_count; ++i) {
            const CombatEntity* ce = &s->combat_entities[i];
            if (!ce->active || ce->ship) continue; // ships are drawn above
            renderer_draw_quad(ce->position,
                               Vec2{ ce->radius * 2.0f, ce->radius * 2.0f },
                               ce->tint, LAYER_UI);
        }
        // ---- Projectiles ---------------------------------------------------------------------
        s->projectiles.glow_override = &s->bullet_glow;
        s->projectiles.render(LAYER_UI);
        // ---- EDIT MODE selection highlight ----------------------------------------------------
        // When active, every editor light gets a tinted marker and a faint radius circle so they
        // are easy to locate. The selected entity (ship or light) gets a bright highlight on top.
        if (s->edit_mode_active) {
            f32 zoom_inv = 1.0f / ((s->camera.zoom > 0.0001f) ? s->camera.zoom : 1.0f);
            // All lights: small tinted marker + faint radius circle.
            for (size_t i = 0; i < s->lights.size(); ++i) {
                const bs_light2d& L = s->lights[i];
                if (!L.enabled) continue;
                bs_color col = L.color;
                bs_color mkr = bs_color{ col.r, col.g, col.b, 0.40f };
                bs_color rad = bs_color{ col.r, col.g, col.b, 0.15f };
                f32 r_mkr = 12.0f * zoom_inv;
                renderer_draw_circle(L.position, r_mkr, 8, 2.0f, mkr, LAYER_GIZMO);
                renderer_draw_circle(L.position, L.radius, 32, 1.5f, rad, LAYER_GIZMO);
            }
            // Selected entity: bright highlight on top.
            const bs_color SEL = bs_color{ 0.30f, 0.95f, 1.00f, 1.0f };
            if (s->edit_selection.kind == EDIT_SHIP) {
                const Ship* sel = (s->edit_selection.index == 0) ? &s->player_ship() : &s->enemy_ship;
                draw_collider_outline(sel, SEL, 3.0f);
            } else if (s->edit_selection.kind == EDIT_LIGHT &&
                       s->edit_selection.index >= 0 &&
                       s->edit_selection.index < (i32)s->lights.size()) {
                Vec2 p = s->lights[s->edit_selection.index].position;
                f32  r = 24.0f * zoom_inv;
                renderer_draw_circle(p, r, 24, 2.0f, SEL, LAYER_GIZMO);
                renderer_draw_line(Vec2{ p.x - r, p.y }, Vec2{ p.x + r, p.y }, 2.0f, SEL, LAYER_GIZMO);
                renderer_draw_line(Vec2{ p.x, p.y - r }, Vec2{ p.x, p.y + r }, 2.0f, SEL, LAYER_GIZMO);
            }
            // ---- GIZMOS (translation arrows + rotation ring) ----------------------------------
            if (s->edit_selection.kind != EDIT_NONE) {
                Vec2 origin = edit_entity_position(s, s->edit_selection);
                f32 axis_len = gizmo_axis_len(zoom_inv);
                f32 arrow_sz = gizmo_arrow_size(zoom_inv);
                // Which gizmo part is currently under the cursor? Visual feedback must match the
                // hit-test logic in edit_pick_gizmo so the user knows what will activate on click.
                EditDragMode hover = edit_pick_gizmo(s, mouse_world(s));
                // Rotation ring — sized to extend past the entity bounds + 30 px screen padding.
                f32 ring_r = 0.0f;
                if (s->edit_selection.kind == EDIT_SHIP) {
                    const Ship* sh = (s->edit_selection.index == 0) ? &s->player_ship() : &s->enemy_ship;
                    ring_r = gizmo_ring_radius_ship(sh, zoom_inv);
                } else {
                    ring_r = gizmo_ring_radius_light(zoom_inv);
                }
                bs_color ring_col = (hover == EDIT_DRAG_ROTATE)
                    ? bs_color{ 0.30f, 0.95f, 1.00f, 1.0f }   // bright cyan on hover
                    : bs_color{ 0.90f, 0.90f, 0.90f, 0.60f }; // gray normally
                f32 ring_thick = (hover == EDIT_DRAG_ROTATE) ? 2.5f : 1.5f;
                renderer_draw_circle(origin, ring_r, 32, ring_thick, ring_col, LAYER_GIZMO);
                // X axis (red) and Y axis (green) — brighten and thicken on hover.
                bs_color x_col = (hover == EDIT_DRAG_AXIS_X)
                    ? bs_color{ 1.00f, 1.00f, 1.00f, 1.0f }   // white on hover
                    : bs_color{ 1.00f, 0.20f, 0.20f, 1.0f };  // red normally
                bs_color y_col = (hover == EDIT_DRAG_AXIS_Y)
                    ? bs_color{ 1.00f, 1.00f, 1.00f, 1.0f }   // white on hover
                    : bs_color{ 0.20f, 1.00f, 0.30f, 1.0f };  // green normally
                f32 axis_thick = 2.5f;
                if (hover == EDIT_DRAG_AXIS_X || hover == EDIT_DRAG_AXIS_Y)
                    axis_thick = 4.0f;
                Vec2 x_end = vec2_add(origin, Vec2{ axis_len, 0.0f });
                Vec2 y_end = vec2_add(origin, Vec2{ 0.0f, axis_len });
                renderer_draw_line(origin, x_end, axis_thick, x_col, LAYER_GIZMO);
                renderer_draw_line(origin, y_end, axis_thick, y_col, LAYER_GIZMO);
                // Arrow heads follow their parent axis color.
                renderer_draw_line(x_end, Vec2{ x_end.x - arrow_sz, x_end.y - arrow_sz * 0.5f }, 2.0f, x_col, LAYER_GIZMO);
                renderer_draw_line(x_end, Vec2{ x_end.x - arrow_sz, x_end.y + arrow_sz * 0.5f }, 2.0f, x_col, LAYER_GIZMO);
                renderer_draw_line(y_end, Vec2{ y_end.x - arrow_sz * 0.5f, y_end.y - arrow_sz }, 2.0f, y_col, LAYER_GIZMO);
                renderer_draw_line(y_end, Vec2{ y_end.x + arrow_sz * 0.5f, y_end.y - arrow_sz }, 2.0f, y_col, LAYER_GIZMO);
            }
        }
        // ---- Travel debug overlay (editor-gated) -----------------------------------------------
        if (s->travel_enabled) {
            Vec2 origin_world = hierpos_to_vec2(&s->travel.origin,      BS_HIERPOS_CELL_SIZE);
            Vec2 dest_world   = hierpos_to_vec2(&s->travel.destination, BS_HIERPOS_CELL_SIZE);
            Vec2 ship_world   = hierpos_to_vec2(&s->travel.current,    BS_HIERPOS_CELL_SIZE);
            // Path line: faint cyan.
            bs_color path_col = bs_color{ 0.35f, 0.90f, 0.95f, 0.6f };
            renderer_draw_line(origin_world, dest_world, 1.5f, path_col, LAYER_UI);
            // Origin marker: green circle.
            renderer_draw_circle(origin_world, 12.0f, 16, 2.0f, bs_color{ 0.35f, 0.95f, 0.45f, 0.9f }, LAYER_UI);
            // Destination marker: red circle.
            renderer_draw_circle(dest_world,   12.0f, 16, 2.0f, bs_color{ 0.95f, 0.35f, 0.35f, 0.9f }, LAYER_UI);
            // Current ship marker: yellow circle.
            renderer_draw_circle(ship_world,    8.0f, 16, 2.0f, bs_color{ 1.00f, 0.95f, 0.35f, 0.9f }, LAYER_UI);
        }
        // ---- Sensor range circle (global mode only) -----------------------------------------
        if (s->mode == MODE_GLOBAL && s->map_draw_sensor_range) {
            bs_color sensor_col = bs_color{ 0.45f, 0.90f, 0.40f, 0.30f };
            renderer_draw_circle(s->player_ship().origin, s->map_sensor_range, 64, 2.0f, sensor_col, LAYER_UI);
        }
        // ---- Metaball movement UI (global mode only) ------------------------------------------
        draw_ship_metaballs(s);
    }
    // Action Log Panel -- bottom-right HUD. Shows last 3 messages (fades after 3s), expands to
    // full 30-entry history on hover. Logs significant player actions.
    if (!s->edit_mode_active)
        build_action_log_panel(s, dt);
    // ---- Encounter panel (centered, modal) -----------------------------------------------
    if (s->encounter_active && !s->edit_mode_active) {
        if (bs_ui_begin_panel("ENCOUNTER", BS_UI_ANCHOR_CENTER, 12.0f, BsUiType::BS_UI_TYPE_GAME)) {
            bs_ui_text_colored(1.0f, 1.0f, 1.0f, 1.0f, s->enemy_ship.vessel_name);
            bs_ui_separator();
            Vec2 delta = vec2_sub(s->player_ship().origin, s->enemy_ship.origin);
            f32 dist   = vec2_length(delta);
            char info[128];
            snprintf(info, sizeof(info), "Distance: %.1f m", dist);
            bs_ui_text_colored(0.8f, 0.8f, 0.8f, 1.0f, info);
            char faction_line[128];
            snprintf(faction_line, sizeof(faction_line), "Faction: %s",
                     vessel_faction_name(s->enemy_ship.faction));
            bs_ui_text_colored(0.8f, 0.8f, 0.8f, 1.0f, faction_line);
            bs_ui_text_colored(0.7f, 0.7f, 0.7f, 1.0f,
                                vessel_faction_desc(s->enemy_ship.faction));
            bs_ui_separator();
            if (bs_ui_button("Engage",  TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Engage selected.");
            }
            if (bs_ui_button("Avoid",   TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Avoid selected.");
            }
            if (bs_ui_button("Hail",    TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Hail selected.");
            }
            if (bs_ui_button("Observe", TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Observe selected.");
            }
        }
        bs_ui_end_panel();
    }
    // Editor Panel (always visible: contains the "Edit mode active" checkbox)
    build_editor_panel(s);
    // Transform panel: only in edit mode when an entity is selected
    if (s->edit_mode_active)
        build_transform_panel(s);
    // Time-control panel (visible in ALL modes, even edit mode)
    draw_time_control_panel(s);
    // ---- Navigation HUD + Ship HUD (global mode only, hidden in edit mode) -----------------
    if (s->mode == MODE_GLOBAL && !s->edit_mode_active) {
        i32 nearest = find_system_by_cell(&s->map_entities[0].galaxy_pos, &s->galaxy_voronoi, s->systems);
        f64 sx, sy, nx, ny;
        hierpos_to_f64(&s->map_entities[0].galaxy_pos, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        hierpos_to_f64(&s->systems[nearest].galaxy_center, BS_HIERPOS_CELL_SIZE, &nx, &ny);
        f64 dist = sqrt((sx - nx) * (sx - nx) + (sy - ny) * (sy - ny));
        char dist_buf[64];
        if (dist >= 1000000.0) {
            snprintf(dist_buf, sizeof(dist_buf), "%.2f M u", dist / 1000000.0);
        } else if (dist >= 1000.0) {
            snprintf(dist_buf, sizeof(dist_buf), "%.2f k u", dist / 1000.0);
        } else {
            snprintf(dist_buf, sizeof(dist_buf), "%.0f u", dist);
        }
        if (bs_ui_begin_hud_panel("NAV HUD", BS_UI_ANCHOR_TOP_RIGHT, 16.0f)) {
            const f32 label_x = 60.0f;
            const f32 dim_a   = 0.60f;
            const f32 bright_a= 1.0f;
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SECTOR");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, "Alpha");
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SYS");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a,
                               s->systems[s->current_system].name ? s->systems[s->current_system].name : "?");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x + 110.0f);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "|");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x + 128.0f);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, dist_buf);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "ZONE");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            char zone_buf[16];
            snprintf(zone_buf, sizeof(zone_buf), "%d", get_system_zone(s, s->current_system));
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, zone_buf);
        }
        bs_ui_end_hud_panel();
        // ---- Ship properties HUD (global mode only) -------------------------------------------
        if (!s->free_camera_active) {
        f32 speed = vec2_length(s->player_flight().velocity);
        char speed_buf[64];
        snprintf(speed_buf, sizeof(speed_buf), "%.1f u/s", speed);
        if (bs_ui_begin_hud_panel("SHIP HUD", BS_UI_ANCHOR_TOP_RIGHT, 110.0f)) {
            const f32 label_x = 60.0f;
            const f32 dim_a   = 0.60f;
            const f32 bright_a= 1.0f;
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SPEED");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, speed_buf);
        }
        bs_ui_end_hud_panel();
        }
    }
    // Periodic stats to the log (the only on-screen text is the diagnostic helm-status HUD above).
    {
        // static f32 acc = 0.0f;
    }
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
