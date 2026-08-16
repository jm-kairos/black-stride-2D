#pragma once

// In-world gameplay overlays drawn once per frame in game_render (projectiles, RTS overlay,
// edit-mode selection highlights + gizmos, travel debug, sensor range, heat map). Drawn in
// ALL looks so gameplay stays continuous across the arena <-> galaxy-map blend.
// Extracted verbatim from game.cpp (R1 scene_renderer decomposition).

struct game_state;

void draw_gameplay_overlays(game_state* s);
