#pragma once

#include <defines.h>

struct game_state;

// =====================================================================================
// Defense laser overlay (presentation only). Draws the point-defense beams recorded by
// sim/point_defense.cpp this frame (s->defense_beams / defense_beam_count): a soft glow beam
// plus a hot core from each firing ship's hardpoint to its target projectile, and a small
// impact flash at the target that brightens as the target nears destruction.
// =====================================================================================
void defense_laser_overlay_draw(game_state* s);
