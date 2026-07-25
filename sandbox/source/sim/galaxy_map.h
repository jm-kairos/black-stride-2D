#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

struct game_state;

// Radius (world units) around the camera within which galaxy nodes are fully materialised
// into the hot cache (star + planets). Nodes beyond this render only as map dots. Kept small
// relative to the ~2.5e8 inter-system spacing so only a few non-overlapping neighbours ever
// draw their orbit rings in full detail at once.
static const f64 GALAXY_MATERIALIZE_RADIUS = 3.0e8;

// Galaxy-map subsystem: procedural galaxy generation plus the per-frame upkeep that keeps the
// galaxy map in sync with the world. Rendering lives in render/galaxy_map_render.cpp; the
// sensor-visibility helpers (sensor_visibility_from_dist / get_sensor_visibility) are declared
// in state/game_state.h and defined alongside this module in sim/galaxy_map.cpp.

// One-time init (called from game_init): procedurally generate the ~10k-system galaxy (nodes,
// spatial grid, lane graph), initialise all galaxy-map animation/draw-toggle/recenter state and
// the seed map entity, then materialise the initial hot cache around the origin.
void galaxy_map_init(game_state* s);

// Phase A staged generation (one heavy step per frame while APP_GENERATING). Split so the New
// Game progress bar can advance with a label between each: worldgen (nodes/grid/lanes/habitability
// + map state, reads s->setup) then finalize (hot cache + player entity + initial materialise).
// The history stages (galaxy_history_init / galaxy_history_generate) run in between.
void galaxy_map_worldgen(game_state* s);
void galaxy_map_finalize(game_state* s);

// Per-frame (called from game_update): rebuild the generic map_entities list from the current
// world entities (player ship, enemy ship, ...).
void galaxy_map_sync_entities(game_state* s);

// Per-frame (called from game_update): advance orbital motion for every MATERIALISED system in
// the hot cache. Far systems are inert until the camera brings them into materialisation range.
void galaxy_map_update_orbits(game_state* s, f32 sim_dt);

// Per-frame (called early in game_update): reconcile the hot cache with the nodes nearest the
// camera — materialise newcomers, evict systems that drifted out of range, and set
// s->galaxy.current_system to the cache slot of the nearest system.
void galaxy_materialize_update(game_state* s);

// Nearest galaxy node index to a galaxy position (via the spatial grid). -1 if the galaxy is
// empty. Pure read; safe to call from any thread once generation has completed.
i32 galaxy_nearest_node(const game_state* s, const bs_math::HierPos2* pos);

// Return the hot-cache slot holding `node_idx`, materialising it into a free slot if needed.
// Returns -1 for an invalid node or when the cache is full.
i32 galaxy_ensure_materialized(game_state* s, i32 node_idx);

// Cross-system Ship AI travel (Step 1): shortest-hop route over the lane graph.
// Fills out_route (capacity `max`) with the sequence of node hops from `from` to `to`, EXCLUDING
// the start node and ending at `to`, and writes the hop count to *out_len. Returns TRUE on success
// (the MST spanning tree guarantees connectivity), FALSE for invalid nodes, no path, or a route
// longer than `max`. from == to yields a zero-length route and TRUE. Pure read.
b8 galaxy_route_find(const game_state* s, i32 from, i32 to, i32* out_route, i32 max, i32* out_len);

// Cross-system Ship AI travel (Step 1): world-space length of the lane between two nodes (distance
// between their star centres). Adjacency is not verified; intended for lane-weighted routing.
f32 galaxy_lane_length(const game_state* s, i32 node_a, i32 node_b);

// ---- Station identity + deterministic layout (station-issued trade contracts) -------------
// Stations are keyed by a galaxy-unique id packing (node index, station index). The layout is a
// PURE function of the lightweight GalaxyNode summary (seed + orbit radii), so the macro mission
// layer can reproduce any system's station positions without materialising the full StarSystem.

static inline i32 station_id_make(i32 node, i32 idx) { return (node << 8) | (idx & 0xFF); }
static inline i32 station_id_node(i32 id)            { return id >> 8; }
static inline i32 station_id_index(i32 id)           { return id & 0xFF; }

// One entry of a system's deterministic station layout (same values materialisation writes into
// SystemStation; see generate_system_stations in galaxy_map.cpp).
struct StationLayoutEntry {
    bs_math::HierPos2 pos;         // absolute galaxy position
    f32               radius;      // world units
    f32               pulse_phase; // undiscovered-marker pulse phase
};

// Reproduce the station layout of galaxy node `node_idx` into out[] (capacity max_out). Returns
// the number of stations (0 for unowned/orbitless nodes -- ownership is NOT checked here; callers
// gate on node_owner). Deterministic: always matches the materialised StarSystem's stations.
i32 galaxy_node_station_layout(const game_state* s, i32 node_idx, StationLayoutEntry* out, i32 max_out);

// Absolute position (and optionally radius) of the station with `station_id`. Returns FALSE when
// the id is out of range for its node's layout (caller should fall back to the node centre).
b8 galaxy_station_pos_by_id(const game_state* s, i32 station_id, bs_math::HierPos2* out_pos, f32* out_radius);

