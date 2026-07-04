#pragma once

struct game_state;

// Ship rendering pass: computes render-space positions for fleet/enemy/combat entities, the
// HierPos2 debug grid, per-ship directional star lighting, ship sprites, engine exhaust, collider
// overlays, the sensor-gated enemy ship, non-ship combat quads, and fleet emblems. Extracted
// verbatim from game_render. Must run after the star position + lighting are established.
void draw_ship_scene(game_state* s);
