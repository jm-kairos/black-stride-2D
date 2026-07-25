#pragma once

struct game_state;

// Radiation heat map. Gathers detector sources (player fleet) and heat emitters (enemy
// combat entities + projectiles, with velocity-extrapolated trails), then submits a single
// GPU heat-map draw in the render frame. Faded out only when zoomed far past the
// arena toward the galaxy floor. No-op when the metaball UI is off or fully faded.
void draw_ship_metaballs(game_state* s);
