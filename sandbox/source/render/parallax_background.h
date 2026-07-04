#pragma once

#include <defines.h> // f32

struct game_state;

// Procedural parallax background pass (starfield + nebula). Updates the current star's render-space
// position (for the dazzle effect), then draws the virtual-quadtree LOD background across the full
// zoom range and restores the render-space camera. Extracted verbatim from game_render.
void draw_parallax_background(game_state* s, f32 dt);
