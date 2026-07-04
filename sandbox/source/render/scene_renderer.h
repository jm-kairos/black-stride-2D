#pragma once

#include <defines.h> // f32

struct game_state;

// Scene render orchestrator: owns the ordered world-drawing passes for one frame
//   galaxy-map look -> parallax background -> frame lighting -> ships -> gameplay overlays.
// The pass ORDER is a hard dependency (parallax sets star_pos; lighting and the ship pass read
// it), so it is expressed in ONE place here instead of being implied by call order in game_render.
// UI panels and frame timing stay in game_render.
void render_scene(game_state* s, f32 dt);
