#include "sim/galaxy_map.h"
#include "game.h"              // full game_state
#include "ss_generation.h"     // generate_star_system / update_planet_positions
#include "sim/galaxy_gen.h"    // galaxy_generate
#include "sim/galaxy_spatial.h"// galaxy_grid_nearest / galaxy_grid_query_radius
#include "sim/galaxy_history.h" // galaxy_history_init (Phase 1 civilization seeding)
#include <core/logger.h>       // BS_LOG_INFO (temp Phase 0 verification)
#include <core/memory/bs_memory.h> // bs_memory_allocator / bs_memory_free (route-finder scratch)
#include <math.h>              // cosf, sinf
#include <stdlib.h>            // qsort

using namespace bs_math;

// GalaxyState is a named sub-struct of game_state; alias it for the cache helpers below.
using GalaxyState = game_state::GalaxyState;

// Master seed the whole galaxy derives from (deterministic: same seed -> same 10k-system layout).
static const u64 GALAXY_MASTER_SEED = 0x9E3779B97F4A7C15ull;

// ---- Hot-cache materialisation --------------------------------------------------------------

// splitmix64 finalizer — a tiny deterministic PRNG for station layout (independent of the global
// star-system RNG so it never perturbs planet generation determinism).
static inline u64 station_mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Is galaxy node `node` within 1 or 2 lane hops of a system that was HABITED at galaxy generation?
// Reads the FROZEN node_owner_gen snapshot (Option B) so the answer is stable for the whole session
// regardless of how live borders shift. Used to bias the uninhabited-station spawn roll. Pure read.
static b8 station_near_habited_gen(const game_state* s, i32 node) {
    const GalaxyState& g = s->galaxy;
    if (!g.node_owner_gen) return FALSE;
    const GalaxyLaneGraph& lg = g.lanes;
    if (!lg.adj_start || !lg.adj_neighbor) return FALSE;
    if (node < 0 || node >= g.node_count) return FALSE;
    i32 a0 = lg.adj_start[node], a1 = lg.adj_start[node + 1];
    for (i32 k1 = a0; k1 < a1; ++k1) {
        i32 nb1 = lg.adj_neighbor[k1];
        if (nb1 < 0 || nb1 >= g.node_count) continue;
        if (g.node_owner_gen[nb1] >= 0) return TRUE;                 // 1 hop away
        i32 b0 = lg.adj_start[nb1], b1 = lg.adj_start[nb1 + 1];
        for (i32 k2 = b0; k2 < b1; ++k2) {
            i32 nb2 = lg.adj_neighbor[k2];
            if (nb2 == node || nb2 < 0 || nb2 >= g.node_count) continue;
            if (g.node_owner_gen[nb2] >= 0) return TRUE;             // 2 hops away
        }
    }
    return FALSE;
}

// How many of a node's `geo` geometric station slots actually exist, given ownership policy:
//   - Habited NOW (live node_owner, alive civ): the full set.
//   - Uninhabited: a deterministic 10% spawn roll (+10% if within 1-2 hops of a system habited at
//     galaxy generation), and when it spawns, HALF the habited count (task requirement). The roll
//     uses an independent PRNG stream (does not perturb station geometry) and the FROZEN snapshot,
//     so an uninhabited system's station set is fixed for the session.
static i32 station_count_target(const game_state* s, i32 node_idx, i32 geo) {
    const GalaxyState& g = s->galaxy;
    i32 owner = galaxy_history_owner_at_node(s, node_idx);
    if (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0) return geo; // habited: full set
    if (geo <= 0) return 0;
    f32 prob = 0.10f + (station_near_habited_gen(s, node_idx) ? 0.10f : 0.0f);
    u64 r = station_mix(g.nodes[node_idx].seed ^ 0x5741710Bull);
    f32 roll = (f32)(r & 0xFFFFFF) / (f32)0xFFFFFF;
    if (roll >= prob) return 0;                       // no stations in this uninhabited system
    return geo / 2;                                   // uninhabited: half the habited count
}

// Deterministic station layout core: reproduces a node's station placement PURELY from the
// lightweight GalaxyNode summary (seed + sorted orbit radii + star radius), so the macro mission
// layer can query any system's stations without materialising it. Stations are placed statically
// around the star in concentric orbital zones (outside-in), with counts decreasing inward:
// Zone 0 = 3, Zone 1 = 2, Zone 2 = 1 (6 total habited). The full geometry is always generated, then
// truncated to station_count_target() so habited/uninhabited share a deterministic station PREFIX.
i32 galaxy_node_station_layout(const game_state* s, i32 node_idx, StationLayoutEntry* out, i32 max_out) {
    if (node_idx < 0 || node_idx >= s->galaxy.node_count || max_out <= 0) return 0;
    const GalaxyNode& nd = s->galaxy.nodes[node_idx];
    const f32* orbits = nd.orbit_radii;          // already sorted ascending (node summary)
    i32 oc = nd.orbit_count;
    if (oc <= 0) return 0;

    const i32 ZONE_COUNTS[4] = { 3, 2, 1, 0 };
    const f32 star_clear = nd.star_radius * 2.0f; // keep stations off the star surface
    u64 rng = station_mix(nd.seed ^ 0x57A7104Eull);
    auto frand = [&]() -> f32 { rng = station_mix(rng); return (f32)(rng & 0xFFFFFF) / (f32)0xFFFFFF; };

    // Zone index outside-in: 0 = beyond outermost orbit; j = between orbit[oc-1-j] and orbit[oc-j];
    // oc = inside the innermost orbit. We only populate the outer four zones (0..3).
    i32 count_out = 0;
    i32 zone_max = oc; // there are (oc + 1) zones total (0..oc)
    for (i32 z = 0; z < 4; ++z) {
        f32 inner, outer;
        if (z == 0) {                       // beyond the outermost orbit
            inner = orbits[oc - 1];
            outer = orbits[oc - 1] * 1.4f;
        } else if (z < zone_max) {          // annulus between two orbit rings
            inner = orbits[oc - 1 - z];
            outer = orbits[oc - z];
        } else if (z == zone_max) {         // inside the innermost orbit
            inner = star_clear;
            outer = orbits[0];
        } else {
            break;                          // this system has no such zone
        }
        if (outer <= inner) continue;

        i32 count = ZONE_COUNTS[z];
        for (i32 k = 0; k < count && count_out < SYSTEM_STATION_MAX; ++k) {
            f32 ang = frand() * 2.0f * BS_PI;
            f32 rad = inner + frand() * (outer - inner);
            Vec2 off = Vec2{ cosf(ang) * rad, sinf(ang) * rad };
            StationLayoutEntry& e = out[count_out < max_out ? count_out : max_out - 1];
            if (count_out < max_out) {
                e.pos         = hierpos_add_vec2(&nd.galaxy_center, off);
                e.radius      = 10000.0f + frand() * 10000.0f; // 10k..20k world units
                e.pulse_phase = frand() * 2.0f * BS_PI;
                ++count_out;
            } else {
                frand(); frand();            // keep the rng stream aligned even when out is full
            }
        }
    }
    // Truncate the fully-generated geometry to the ownership policy target (Option B): habited -> all,
    // uninhabited -> a deterministic 0 or half-count. Both share the same station PREFIX so the macro
    // mission layer and the materialised StarSystem always agree.
    i32 target = station_count_target(s, node_idx, count_out);
    return (target < count_out) ? target : count_out;
}

b8 galaxy_station_pos_by_id(const game_state* s, i32 station_id, HierPos2* out_pos, f32* out_radius) {
    if (station_id < 0) return FALSE;
    i32 node = station_id_node(station_id);
    i32 idx  = station_id_index(station_id);
    StationLayoutEntry layout[SYSTEM_STATION_MAX];
    i32 n = galaxy_node_station_layout(s, node, layout, SYSTEM_STATION_MAX);
    if (idx >= n) return FALSE;
    if (out_pos)    *out_pos    = layout[idx].pos;
    if (out_radius) *out_radius = layout[idx].radius;
    return TRUE;
}

// Populate ss.stations from the shared deterministic layout core (see galaxy_node_station_layout).
// Works for BOTH habited systems (owner_civ = controlling civ) and uninhabited systems that won the
// station roll (owner_civ = -1); the layout core already returns 0 for uninhabited systems without
// stations. The first MISSION_HUBS_PER_SYSTEM stations become the system's trade-contract issuers.
static void generate_system_stations(game_state* s, StarSystem& ss, i32 node_idx, i32 owner_civ) {
    ss.station_count = 0;
    i16 owner = (owner_civ >= 0 && owner_civ < s->galaxy.civ_count) ? (i16)owner_civ : (i16)-1;

    StationLayoutEntry layout[SYSTEM_STATION_MAX];
    i32 n = galaxy_node_station_layout(s, node_idx, layout, SYSTEM_STATION_MAX);
    for (i32 k = 0; k < n; ++k) {
        SystemStation& st = ss.stations[ss.station_count++];
        st.pos         = layout[k].pos;
        st.radius      = layout[k].radius;
        st.pulse_phase = layout[k].pulse_phase;
        st.owner_civ   = owner;
        st.discovered  = FALSE;
        st.station_id  = station_id_make(node_idx, k);
        st.mission_hub = (k < MISSION_HUBS_PER_SYSTEM);
    }
}

// Asteroid silhouette palette (grey-brown / grey / rusty / blue-grey).
static const bs_color SYS_ASTEROID_COLS[4] = {
    { 0.55f, 0.50f, 0.45f, 1.0f },
    { 0.48f, 0.46f, 0.44f, 1.0f },
    { 0.60f, 0.48f, 0.36f, 1.0f },
    { 0.42f, 0.44f, 0.48f, 1.0f },
};

// Populate ss.asteroids for EVERY system. Asteroids are natural, so unlike stations they are not
// civ-gated. Placed in the same concentric orbital zones as stations (outside-in) with counts
// decreasing inward: Zone 0 = 300, Zone 1 = 240, Zone 2 = 120, Zone 3 = 60 (capped at
// SYSTEM_ASTEROID_MAX). Deterministic from `seed`, independent of the star-system RNG.
static void generate_system_asteroids(game_state* s, StarSystem& ss, u64 seed) {
    (void)s;
    ss.asteroid_count = 0;

    // Sorted (ascending) orbit radii = the ring boundaries of the system's zones.
    f32 orbits[5]; i32 oc = ss.planet_count < 5 ? ss.planet_count : 5;
    for (i32 i = 0; i < oc; ++i) orbits[i] = ss.planets[i].semi_major_axis;
    for (i32 i = 1; i < oc; ++i) { // insertion sort (tiny)
        f32 v = orbits[i]; i32 j = i - 1;
        while (j >= 0 && orbits[j] > v) { orbits[j + 1] = orbits[j]; --j; }
        orbits[j + 1] = v;
    }
    if (oc <= 0) return;

    const i32 ZONE_COUNTS[4] = { 1200, 960, 600, 300 };
    const f32 star_clear = ss.star.radius * 2.0f; // keep asteroids off the star surface
    u64 rng = station_mix(seed ^ 0xA57E401DCAFEull);
    auto frand = [&]() -> f32 { rng = station_mix(rng); return (f32)(rng & 0xFFFFFF) / (f32)0xFFFFFF; };

    i32 zone_max = oc; // there are (oc + 1) zones total (0..oc)
    for (i32 z = 0; z < 4; ++z) {
        f32 inner, outer;
        if (z == 0) {                       // beyond the outermost orbit
            inner = orbits[oc - 1];
            outer = orbits[oc - 1] * 1.4f;
        } else if (z < zone_max) {          // annulus between two orbit rings
            inner = orbits[oc - 1 - z];
            outer = orbits[oc - z];
        } else if (z == zone_max) {         // inside the innermost orbit
            inner = star_clear;
            outer = orbits[0];
        } else {
            break;                          // this system has no such zone
        }
        if (outer <= inner) continue;

        i32 count = ZONE_COUNTS[z];
        for (i32 k = 0; k < count && ss.asteroid_count < SYSTEM_ASTEROID_MAX; ++k) {
            f32 ang = frand() * 2.0f * BS_PI;
            f32 rad = inner + frand() * (outer - inner);
            Vec2 off = Vec2{ cosf(ang) * rad, sinf(ang) * rad };
            SystemAsteroid& a = ss.asteroids[ss.asteroid_count++];
            a.pos      = hierpos_add_vec2(&ss.galaxy_center, off);
            a.radius   = 400.0f + frand() * 600.0f;                       // 400..1000 world units
            a.rotation = frand() * 2.0f * BS_PI;
            a.spin     = (0.01f + frand() * 0.09f) * (frand() < 0.5f ? -1.0f : 1.0f);
            a.color    = SYS_ASTEROID_COLS[(i32)(frand() * 3.999f)];
            a.verts    = 7 + (i32)(frand() * ((f32)(ASTEROID_MAX_VERTS - 7) + 0.999f));
            if (a.verts > ASTEROID_MAX_VERTS) a.verts = ASTEROID_MAX_VERTS;
            for (i32 v = 0; v < a.verts; ++v) a.vert_jitter[v] = 0.60f + frand() * 0.40f;
        }
    }
}

// Populate ss.resources for EVERY system. Resources are CONCENTRATED in the belt (zone 1) and mid
// (zone 2) orbital rings, with only a sparse scatter in the outer/inner zones. Placed in the same
// concentric zones as asteroids (outside-in), capped at SYSTEM_RESOURCE_MAX. Deterministic from
// `seed`, independent of the star-system RNG.
static void generate_system_resources(game_state* s, StarSystem& ss, u64 seed) {
    (void)s;
    ss.resource_count = 0;

    // Sorted (ascending) orbit radii = the ring boundaries of the system's zones.
    f32 orbits[5]; i32 oc = ss.planet_count < 5 ? ss.planet_count : 5;
    for (i32 i = 0; i < oc; ++i) orbits[i] = ss.planets[i].semi_major_axis;
    for (i32 i = 1; i < oc; ++i) { // insertion sort (tiny)
        f32 v = orbits[i]; i32 j = i - 1;
        while (j >= 0 && orbits[j] > v) { orbits[j + 1] = orbits[j]; --j; }
        orbits[j + 1] = v;
    }
    if (oc <= 0) return;

    // Zone 0 = beyond outermost (fringe), Zone 1 = belt, Zone 2 = mid, Zone 3 = inner-ish.
    // Concentrated in the belt/mid rings; sparse elsewhere.
    const i32 ZONE_COUNTS[4] = { 8, 48, 32, 6 };
    const bs_color RESOURCE_COL = bs_color{ 0.35f, 0.95f, 0.90f, 1.0f };
    const f32 star_clear = ss.star.radius * 2.0f; // keep resources off the star surface
    u64 rng = station_mix(seed ^ 0x4E5011C2DEADull);
    auto frand = [&]() -> f32 { rng = station_mix(rng); return (f32)(rng & 0xFFFFFF) / (f32)0xFFFFFF; };

    i32 zone_max = oc; // there are (oc + 1) zones total (0..oc)
    for (i32 z = 0; z < 4; ++z) {
        f32 inner, outer;
        if (z == 0) {                       // beyond the outermost orbit
            inner = orbits[oc - 1];
            outer = orbits[oc - 1] * 1.4f;
        } else if (z < zone_max) {          // annulus between two orbit rings
            inner = orbits[oc - 1 - z];
            outer = orbits[oc - z];
        } else if (z == zone_max) {         // inside the innermost orbit
            inner = star_clear;
            outer = orbits[0];
        } else {
            break;                          // this system has no such zone
        }
        if (outer <= inner) continue;

        i32 count = ZONE_COUNTS[z];
        for (i32 k = 0; k < count && ss.resource_count < SYSTEM_RESOURCE_MAX; ++k) {
            f32 ang = frand() * 2.0f * BS_PI;
            f32 rad = inner + frand() * (outer - inner);
            Vec2 off = Vec2{ cosf(ang) * rad, sinf(ang) * rad };
            SystemResource& r = ss.resources[ss.resource_count++];
            r.pos    = hierpos_add_vec2(&ss.galaxy_center, off);
            r.radius = 40.0f + frand() * 30.0f; // 40..70 world units
            r.color  = RESOURCE_COL;
        }
    }
}

// Populate ss.decorations for EVERY system. Faint ambient dust motes SCATTERED evenly across all
// orbital zones (outside-in), tinted by the local star, capped at SYSTEM_DECORATION_MAX.
// Deterministic from `seed`, independent of the star-system RNG.
static void generate_system_decorations(game_state* s, StarSystem& ss, u64 seed) {
    (void)s;
    ss.decoration_count = 0;

    // Sorted (ascending) orbit radii = the ring boundaries of the system's zones.
    f32 orbits[5]; i32 oc = ss.planet_count < 5 ? ss.planet_count : 5;
    for (i32 i = 0; i < oc; ++i) orbits[i] = ss.planets[i].semi_major_axis;
    for (i32 i = 1; i < oc; ++i) { // insertion sort (tiny)
        f32 v = orbits[i]; i32 j = i - 1;
        while (j >= 0 && orbits[j] > v) { orbits[j + 1] = orbits[j]; --j; }
        orbits[j + 1] = v;
    }
    if (oc <= 0) return;

    // Scattered roughly evenly across every zone (outside-in).
    const i32 ZONE_COUNTS[4] = { 90, 80, 70, 50 };
    // Faint dust tinted by the local star (alpha kept low so it reads as ambient haze).
    const bs_color star = ss.star.color;
    const bs_color DECO_COL = bs_color{ star.r, star.g, star.b, 0.35f };
    const f32 star_clear = ss.star.radius * 2.0f; // keep dust off the star surface
    u64 rng = station_mix(seed ^ 0xDEC0DA7A5EEDull);
    auto frand = [&]() -> f32 { rng = station_mix(rng); return (f32)(rng & 0xFFFFFF) / (f32)0xFFFFFF; };

    i32 zone_max = oc; // there are (oc + 1) zones total (0..oc)
    for (i32 z = 0; z < 4; ++z) {
        f32 inner, outer;
        if (z == 0) {                       // beyond the outermost orbit
            inner = orbits[oc - 1];
            outer = orbits[oc - 1] * 1.4f;
        } else if (z < zone_max) {          // annulus between two orbit rings
            inner = orbits[oc - 1 - z];
            outer = orbits[oc - z];
        } else if (z == zone_max) {         // inside the innermost orbit
            inner = star_clear;
            outer = orbits[0];
        } else {
            break;                          // this system has no such zone
        }
        if (outer <= inner) continue;

        i32 count = ZONE_COUNTS[z];
        for (i32 k = 0; k < count && ss.decoration_count < SYSTEM_DECORATION_MAX; ++k) {
            f32 ang = frand() * 2.0f * BS_PI;
            f32 rad = inner + frand() * (outer - inner);
            Vec2 off = Vec2{ cosf(ang) * rad, sinf(ang) * rad };
            SystemDecoration& d = ss.decorations[ss.decoration_count++];
            d.pos    = hierpos_add_vec2(&ss.galaxy_center, off);
            d.radius = 6.0f + frand() * 10.0f; // 6..16 world units
            d.color  = DECO_COL;
        }
    }
}

// Materialise a galaxy node into cache slot `slot`: derive the full StarSystem from the node's
// seed, then overwrite galaxy_center with the node's PRECISE HierPos2 (generate_star_system would
// otherwise round-trip a lossy Vec2, jittering far systems).
static void materialize_slot(game_state* s, i32 slot, i32 node_idx) {
    const GalaxyNode& nd = s->galaxy.nodes[node_idx];
    // Derive the same structural environment the map dot used (a pure function of the node's
    // position) so the star flown into matches the summarised dot's population and colour.
    SSGenEnv env = galaxy_env_at(&s->galaxy.gen_params, &nd.galaxy_center);
    generate_star_system(&s->galaxy.systems[slot], nd.seed, Vec2{ 0.0f, 0.0f }, env);
    s->galaxy.systems[slot].galaxy_center = nd.galaxy_center;
    s->galaxy.systems[slot].name          = nd.name;
    s->galaxy.cache_node[slot]            = node_idx;

    // Civilian stations exist in habited systems and in the subset of uninhabited systems that won
    // the deterministic station roll (see galaxy_node_station_layout). Habited -> owner colour;
    // uninhabited -> neutral (owner_civ = -1). The layout core yields 0 stations for the rest.
    i32 owner = galaxy_history_owner_at_node(s, node_idx);
    b8 alive  = (owner >= 0 && owner < s->galaxy.civ_count && s->galaxy.civs[owner].status == 0);
    generate_system_stations(s, s->galaxy.systems[slot], node_idx, alive ? owner : -1);

    // Natural asteroids exist in EVERY system (not civ-gated).
    generate_system_asteroids(s, s->galaxy.systems[slot], nd.seed);

    // Resource nodes (belt/mid zones) + ambient dust (all zones) exist in EVERY system.
    generate_system_resources(s, s->galaxy.systems[slot], nd.seed);
    generate_system_decorations(s, s->galaxy.systems[slot], nd.seed);
}

i32 galaxy_nearest_node(const game_state* s, const HierPos2* pos) {
    f64 x, y; hierpos_to_f64(pos, BS_HIERPOS_CELL_SIZE, &x, &y);
    return galaxy_grid_nearest(&s->galaxy.grid, s->galaxy.nodes, x, y);
}

i32 galaxy_ensure_materialized(game_state* s, i32 node_idx) {
    if (node_idx < 0 || node_idx >= s->galaxy.node_count) return -1;
    for (i32 i = 0; i < s->galaxy.system_count; ++i)
        if (s->galaxy.cache_node[i] == node_idx) return i;
    if (s->galaxy.system_count >= GALAXY_MAX_SYSTEMS) return -1;
    i32 slot = s->galaxy.system_count++;
    materialize_slot(s, slot, node_idx);
    return slot;
}

f32 galaxy_lane_length(const game_state* s, i32 node_a, i32 node_b) {
    const GalaxyState& g = s->galaxy;
    if (node_a < 0 || node_a >= g.node_count || node_b < 0 || node_b >= g.node_count) return 0.0f;
    Vec2 d = hierpos_diff(&g.nodes[node_a].galaxy_center, &g.nodes[node_b].galaxy_center, BS_HIERPOS_CELL_SIZE);
    return vec2_length(d);
}

// Cross-system Ship AI travel (Step 1): breadth-first shortest-HOP route over the CSR lane graph.
// BFS is sufficient here -- lanes are short and the spanning tree guarantees connectivity -- and it
// yields a minimal-hop path deterministically. Scratch (visited/parent) is allocated per call and
// freed before return; routing is infrequent so the transient allocation is cheap.
b8 galaxy_route_find(const game_state* s, i32 from, i32 to, i32* out_route, i32 max, i32* out_len) {
    if (out_len) *out_len = 0;
    const GalaxyState& g = s->galaxy;
    const GalaxyLaneGraph& lg = g.lanes;
    if (!out_route || max <= 0) return FALSE;
    if (from < 0 || from >= g.node_count || to < 0 || to >= g.node_count) return FALSE;
    if (!lg.adj_start || !lg.adj_neighbor) return FALSE;
    if (from == to) return TRUE;   // already there: zero-length route

    const i32 N = g.node_count;
    u8*  visited = (u8*)bs_memory_allocator(sizeof(u8) * N, MEMORY_TAG_GAME);
    i32* parent  = (i32*)bs_memory_allocator(sizeof(i32) * N, MEMORY_TAG_GAME);
    i32* queue   = (i32*)bs_memory_allocator(sizeof(i32) * N, MEMORY_TAG_GAME);
    for (i32 i = 0; i < N; ++i) { visited[i] = 0; parent[i] = -1; }

    i32 head = 0, tail = 0;
    queue[tail++] = from;
    visited[from] = 1;
    b8 found = FALSE;
    while (head < tail) {
        i32 n = queue[head++];
        if (n == to) { found = TRUE; break; }
        i32 a0 = lg.adj_start[n], a1 = lg.adj_start[n + 1];
        for (i32 k = a0; k < a1; ++k) {
            i32 nb = lg.adj_neighbor[k];
            if (nb < 0 || nb >= N || visited[nb]) continue;
            visited[nb] = 1;
            parent[nb]  = n;
            queue[tail++] = nb;
        }
    }

    b8 ok = FALSE;
    if (found) {
        // Reconstruct to->from via parent[], counting hops (excludes the start node).
        i32 hops = 0;
        for (i32 n = to; n != from; n = parent[n]) ++hops;
        if (hops <= max) {
            *out_len = hops;
            i32 w = hops - 1;
            for (i32 n = to; n != from; n = parent[n]) out_route[w--] = n;
            ok = TRUE;
        }
    }

    bs_memory_free(queue,   sizeof(i32) * N, MEMORY_TAG_GAME);
    bs_memory_free(parent,  sizeof(i32) * N, MEMORY_TAG_GAME);
    bs_memory_free(visited, sizeof(u8)  * N, MEMORY_TAG_GAME);
    return ok;
}

struct NodeDist { i32 idx; f64 d; };
static int nodedist_cmp(const void* a, const void* b) {
    f64 d = ((const NodeDist*)a)->d - ((const NodeDist*)b)->d;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}

void galaxy_materialize_update(game_state* s) {
    GalaxyState* g = &s->galaxy;
    if (g->node_count <= 0) { g->current_system = -1; return; }

    f64 fx, fy; hierpos_to_f64(&s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE, &fx, &fy);

    const i32 RAWMAX = 256;
    i32 raw[RAWMAX];
    i32 raw_n = galaxy_grid_query_radius(&g->grid, g->nodes, fx, fy,
                                         GALAXY_MATERIALIZE_RADIUS, raw, RAWMAX);

    // Rank candidates by distance and keep the nearest cache-worth.
    NodeDist nd[RAWMAX];
    for (i32 i = 0; i < raw_n; ++i) {
        f64 x, y; hierpos_to_f64(&g->nodes[raw[i]].galaxy_center, BS_HIERPOS_CELL_SIZE, &x, &y);
        f64 dx = x - fx, dy = y - fy;
        nd[i] = NodeDist{ raw[i], dx * dx + dy * dy };
    }
    qsort(nd, raw_n, sizeof(NodeDist), nodedist_cmp);
    i32 want_n = raw_n < GALAXY_MAX_SYSTEMS ? raw_n : GALAXY_MAX_SYSTEMS;

    // If the radius somehow caught nothing (deep void), fall back to the single nearest node so
    // there is always a current system.
    if (want_n == 0) {
        i32 nn = galaxy_grid_nearest(&g->grid, g->nodes, fx, fy);
        if (nn >= 0) { nd[0] = NodeDist{ nn, 0.0 }; want_n = 1; }
    }

    // Step A: evict cache slots whose node fell out of range (compact in place, preserving the
    // orbit-animation state of the systems that stay).
    i32 w = 0;
    for (i32 i = 0; i < g->system_count; ++i) {
        i32 ndx = g->cache_node[i];
        b8 keep = FALSE;
        for (i32 k = 0; k < want_n; ++k) if (nd[k].idx == ndx) { keep = TRUE; break; }
        if (keep) {
            if (w != i) { g->systems[w] = g->systems[i]; g->cache_node[w] = ndx; }
            ++w;
        }
    }
    g->system_count = w;

    // Step B: materialise newly in-range nodes into free slots.
    for (i32 k = 0; k < want_n && g->system_count < GALAXY_MAX_SYSTEMS; ++k) {
        i32 ndx = nd[k].idx;
        b8 present = FALSE;
        for (i32 i = 0; i < g->system_count; ++i) if (g->cache_node[i] == ndx) { present = TRUE; break; }
        if (!present) { i32 slot = g->system_count++; materialize_slot(s, slot, ndx); }
    }

    // current_system = cache slot of the nearest system (guaranteed materialised above).
    i32 near_node = want_n > 0 ? nd[0].idx : -1;
    i32 slot = -1;
    for (i32 i = 0; i < g->system_count; ++i) if (g->cache_node[i] == near_node) { slot = i; break; }
    if (slot < 0) slot = galaxy_ensure_materialized(s, near_node);
    g->current_system = slot;
}

void galaxy_map_worldgen(game_state* s) {
    s->galaxy.galaxy_map_time    = 0.0f;
    s->galaxy.map_anim_scale     = true;
    s->galaxy.map_anim_rotate    = true;
    s->galaxy.map_anim_alpha     = true;
    s->galaxy.map_anim_thickness = true;

    s->galaxy.map_draw_jump_range   = FALSE;
    s->galaxy.map_jump_range        = 5000000.0f;
    s->galaxy.map_draw_sensor_range = FALSE;
    s->galaxy.map_sensor_range      = 250000.0f;
    s->galaxy.ai_speed_in_system    = 50000.0f;    // u/s: in-system legs (stations / jump-points)
    s->galaxy.ai_speed_jump         = 1.0e6f;      // u/s: between systems (jump-point to jump-point)

    // Market-delta pool starts empty. Station id 0 is a VALID id ((node 0 << 8) | 0), so "free"
    // must be an explicit -1, not the zero-init default.
    for (i32 i = 0; i < STATION_MARKET_DELTA_MAX; ++i)
        s->galaxy.market_deltas[i].station_id = -1;
    for (i32 i = 0; i < STATION_REVENUE_MAX; ++i)
        s->galaxy.station_revenues[i].station_id = -1;
    s->galaxy.map_draw_lanes        = TRUE;
    s->galaxy.map_draw_habitability = FALSE;
    s->galaxy.map_draw_civs         = FALSE;
    s->galaxy.show_legends          = FALSE;
    s->galaxy.show_houses           = FALSE;

    // Galaxy history clock: "present" is year 0; the simulated past spans back by the player-chosen
    // history depth (New Game setup screen).
    s->galaxy.clock.present_year = 0;
    s->galaxy.clock.start_year   = -(s->setup.history_depth_years > 0 ? s->setup.history_depth_years : 1000000);

    s->camera_state.recentering = FALSE;
    s->camera_state.recenter_t  = 0.0f;

    // ---- Full galaxy generation on a gaussian disc (size + seed from the setup screen) -----
    i32 size = s->setup.galaxy_size > 0 ? s->setup.galaxy_size : GALAXY_TARGET_SYSTEMS;
    u64 seed = s->setup.seed ? s->setup.seed : GALAXY_MASTER_SEED;
    galaxy_generate(s, seed, size, (GalaxyShape)s->setup.galaxy_shape);

    // TEMP (Phase 0 verify): count habitable systems + max habitability across the galaxy.
    {
        i32 hab_sys = 0, best = 0;
        for (i32 i = 0; i < s->galaxy.node_count; ++i) {
            if (s->galaxy.nodes[i].habitable_count > 0) ++hab_sys;
            if (s->galaxy.nodes[i].best_habitability > best) best = s->galaxy.nodes[i].best_habitability;
        }
        BS_LOG_INFO("Phase0 habitability: %d/%d systems habitable, best=%d/255", hab_sys, s->galaxy.node_count, best);
    }

    // Territory arrays are (re)allocated by galaxy_history_generate; ensure null before it runs.
    s->galaxy.node_owner = nullptr;
    s->galaxy.node_owner_gen = nullptr;
    s->galaxy.node_colonized_year = nullptr;
    s->galaxy.missions = nullptr;
    s->galaxy.mission_count = 0;
    s->galaxy.mission_capacity = 0;
}

void galaxy_map_finalize(game_state* s) {
    // Hot cache starts empty; materialise around the origin (node 0 == home) below.
    s->galaxy.system_count = 0;
    for (i32 i = 0; i < GALAXY_MAX_SYSTEMS; ++i) s->galaxy.cache_node[i] = -1;
    s->galaxy.current_system = -1;

    // Player ship starts at the home system; pre-seed map_entities[0] so the first-frame queries
    // have valid data. Rebuilt each frame in game_update.
    s->galaxy.map_entities[0] = MapEntity{ s->player_ship().origin,
                                    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, 12.0f, TRUE, "Player Ship" };
    s->galaxy.map_entity_count = 1;

    // Populate the initial hot cache (camera_hierpos was set to the origin before this call).
    galaxy_materialize_update(s);
}

void galaxy_map_init(game_state* s) {
    galaxy_map_worldgen(s);
    galaxy_history_generate(s);    // Phase B: deep-time simulation (begin + step-all + end)
    // OPTION B: freeze the present-year-0 ownership map. The uninhabited-station policy reads this
    // snapshot (not live node_owner) so a system's uninhabited station set stays stable for the whole
    // session even as civilizations expand/contract across the living galaxy.
    if (s->galaxy.node_owner && s->galaxy.node_owner_gen)
        for (i32 i = 0; i < s->galaxy.node_count; ++i)
            s->galaxy.node_owner_gen[i] = s->galaxy.node_owner[i];
    galaxy_map_finalize(s);
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
