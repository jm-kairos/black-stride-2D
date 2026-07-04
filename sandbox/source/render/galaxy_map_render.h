#pragma once

#include <defines.h> // f32

struct game_state;

// Galaxy-map "look" render pass (was MODE_SYSTEM): Voronoi lanes/cells, star sunbursts,
// planet/orbit rings, map entities, jump/sensor range rings, and the hover tooltip. Cross-fades
// in by map weight (1 - view_arena_w) and no-ops when fully in the arena look.
// Extracted verbatim from game_render (R1 scene_renderer decomposition).
void draw_galaxy_map_look(game_state* s, f32 dt);
