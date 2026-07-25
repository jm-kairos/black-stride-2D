#pragma once
#include <defines.h>

struct game_state;

// Per-system civilian stations (owned systems only): pulsing antenna marker until scanned within
// the flagship's Layer 1 sensor radius, then a filled circle + internal quad in the owner's colour.
void draw_system_stations(game_state* s);

// Per-system natural asteroids (every system): tumbling polygon silhouettes on LAYER_CELESTIAL,
// faded by sensor visibility and collapsed to a quad at wide zoom.
void draw_system_asteroids(game_state* s);

// Per-system resource nodes (every system; concentrated in belt/mid zones): small rings on
// LAYER_CELESTIAL, faded by sensor visibility and collapsed to a quad at wide zoom.
void draw_system_resources(game_state* s);

// Per-system ambient dust decorations (every system; scattered across all zones): tiny quads on
// LAYER_CELESTIAL tinted by the local star, faded by sensor visibility.
void draw_system_decorations(game_state* s);

