#include "sim/galaxy_map.h"
#include "game.h"              // full game_state
#include "ss_generation.h"     // generate_star_system / update_planet_positions
#include "voronoi_galaxy.h"    // generate_galaxy_voronoi
#include <math.h>              // cosf, sinf

using namespace bs_math;

// ---- Procedural generation PRNG (splitmix64) ---- (moved from game.cpp; used only here) ----
static u64 g_rng_state = 0x123456789ABCDEF0ull;

static u64 rng_next() {
    u64 z = (g_rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static f32 rng_f32() { return (f32)(rng_next() & 0xFFFFFF) / (f32)0xFFFFFF; }
static f32 rng_range(f32 min, f32 max) { return min + rng_f32() * (max - min); }

// 150M: generous spacing so planets/orbits don't crowd neighbors.
static const f32 MIN_SYSTEM_SEPARATION = 150000000.0f;

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

void galaxy_map_init(game_state* s) {
    s->galaxy.galaxy_map_time    = 0.0f;
    s->galaxy.map_anim_scale     = true;
    s->galaxy.map_anim_rotate    = true;
    s->galaxy.map_anim_alpha     = true;
    s->galaxy.map_anim_thickness = true;

    s->galaxy.map_draw_jump_range   = FALSE;
    s->galaxy.map_jump_range        = 5000000.0f;
    s->galaxy.map_draw_sensor_range = FALSE;
    s->galaxy.map_sensor_range      = 250000.0f;
    s->galaxy.map_draw_lanes        = TRUE;

    s->galaxy.map_recentering     = FALSE;
    s->galaxy.map_recenter_t      = 0.0f;
    s->galaxy.map_input_cooldown  = 0.0f;
    s->galaxy.map_drag_needs_fresh_press = FALSE;

    // ---- Galaxy cluster (50 procedurally generated systems) ------------------------------
    s->galaxy.system_count = 50;

    Vec2 cluster_center = Vec2{ 0.0f, 0.0f };
    i32 placed = 0;
    u64 seed_attempt = 0;
    while (placed < s->galaxy.system_count && seed_attempt < 10000)
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
            Vec2 diff = hierpos_diff(&galaxy_pos, &s->galaxy.systems[j].galaxy_center, BS_HIERPOS_CELL_SIZE);
            if (vec2_length(diff) < MIN_SYSTEM_SEPARATION) { too_close = TRUE; break; }
        }
        if (!too_close)
        {
            generate_star_system(&s->galaxy.systems[placed], seed_attempt, world);
            s->galaxy.systems[placed].name = SYSTEM_NAMES[placed % SYSTEM_NAME_COUNT];
            ++placed;
        }
        ++seed_attempt;
    }
    if (placed < s->galaxy.system_count) s->galaxy.system_count = placed;

    // Home system stays at the cluster center so the ship start position remains valid.
    s->galaxy.systems[0].galaxy_center = HierPos2{ GridCell{0, 0}, Vec2{0.0f, 0.0f} };
    s->galaxy.systems[0].name = "Sol";

    // Generate Voronoi diagram for system territories and Delaunay lane connectivity.
    generate_galaxy_voronoi(s->galaxy.systems, s->galaxy.system_count, &s->galaxy.galaxy_voronoi);

    s->galaxy.current_system = 0;
    s->galaxy.map_entity_count = 0;

    // Player ship starts in system 0; pre-seed map_entities[0] so the first-frame
    // find_system_by_cell has valid data. Rebuilt each frame in game_update.
    s->galaxy.map_entities[0] = MapEntity{ s->player_ship().origin,
                                    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, 12.0f, TRUE, "Player Ship" };
    s->galaxy.map_entity_count = 1;
}

void galaxy_map_sync_entities(game_state* s) {
    // Rebuild the generic map entity list every frame so any world object with a Vec2 position
    // can appear on the galaxy map. Future entities (stations, asteroids, resources) add here.
    s->galaxy.map_entity_count = 0;

    // Player ship (index 0 -- animated quad is drawn around this entry)
    if (s->galaxy.map_entity_count < MAX_MAP_ENTITIES) {
        s->galaxy.map_entities[s->galaxy.map_entity_count++] = MapEntity{
            s->player_ship().origin,
            bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, 12.0f, TRUE, "Player Ship" };
    }

    // Enemy ship
    if (s->galaxy.map_entity_count < MAX_MAP_ENTITIES) {
        s->galaxy.map_entities[s->galaxy.map_entity_count++] = MapEntity{
            s->fleet_state.enemy_ship.origin,
            bs_color{ 1.0f, 0.3f, 0.3f, 1.0f }, 10.0f, FALSE, "Enemy Ship" };
    }
}

void galaxy_map_update_orbits(game_state* s, f32 sim_dt) {
    // Orbital motion (always simulated, only visible in system view).
    for (i32 sys = 0; sys < s->galaxy.system_count; ++sys) {
        update_planet_positions(&s->galaxy.systems[sys], sim_dt);
    }
}

// ---- Sensor visibility ---- (moved from game.cpp; declared in state/game_state.h) ----

// Distance-based sensor visibility (0..1). Range and dist must be in the SAME units.
f32 sensor_visibility_from_dist(f32 dist, f32 range) {
    if (range <= 0.0f) return 1.0f;
    if (dist >= range) return 0.0f;
    f32 t = dist / range;
    return 1.0f - t * t * t;
}

// Compute sensor visibility (0..1) for an entity at render-local position `pos`.
// 1.0 = fully visible (inside strong sensor zone), 0.0 = outside range.
f32 get_sensor_visibility(const game_state* s, Vec2 pos) {
    if (!s->galaxy.map_draw_sensor_range || s->galaxy.map_sensor_range <= 0.0f || s->galaxy.map_entity_count == 0)
        return 1.0f;
    Vec2 ship_rel = hierpos_diff(&s->galaxy.map_entities[0].galaxy_pos, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(vec2_sub(pos, ship_rel));
    return sensor_visibility_from_dist(dist, s->galaxy.map_sensor_range);
}
