#pragma once
#include <defines.h>

struct game_state;

// Camera zoom + view-mode controller. Reads the mouse wheel to nudge a target zoom, eases
// the actual camera zoom toward it in log space, flips the arena<->galaxy-map "look" label
// when crossing the mode threshold (with a free-camera hand-off), and pins the point under
// the cursor (or the piloted ship) while zooming.
void update_zoom_and_mode(game_state* s, f32 dt);
