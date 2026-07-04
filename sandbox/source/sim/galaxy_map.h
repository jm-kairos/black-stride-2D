#pragma once
#include <defines.h>

struct game_state;

// Galaxy-map subsystem: procedural galaxy generation plus the per-frame upkeep that keeps the
// galaxy map in sync with the world. Rendering lives in render/galaxy_map_render.cpp; the
// sensor-visibility helpers (sensor_visibility_from_dist / get_sensor_visibility) are declared
// in state/game_state.h and defined alongside this module in sim/galaxy_map.cpp.

// One-time init (called from game_init): procedurally place the star cluster, build the Voronoi
// territory / Delaunay lane diagram, and initialise all galaxy-map animation, draw-toggle and
// recenter-animation state plus the seed map entity.
void galaxy_map_init(game_state* s);

// Per-frame (called from game_update): rebuild the generic map_entities list from the current
// world entities (player ship, enemy ship, ...).
void galaxy_map_sync_entities(game_state* s);

// Per-frame (called from game_update): advance orbital motion for every system. Always simulated
// even though the orbits are only visible in the galaxy-map look.
void galaxy_map_update_orbits(game_state* s, f32 sim_dt);
