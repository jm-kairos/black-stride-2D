#pragma once

#include <defines.h>

struct game_state;

// =====================================================================================
// Sensor overlay (presentation). Single entry point for all sensor visuals:
//   1. Flagship sensor rings (Layer 0/1/2), gated on s->show_sensor_layers.
//   2. World-space radar "contacts" for detected hostile projectiles (always on): a small
//      sprite that fades in/out like a sonar contact between Layer 2 and Layer 1, intensifying
//      and pulsing faster as it closes, then steady/identified inside Layer 1.
//   3. Screen-edge HUD indicators pointing at off-screen contacts.
//
// Detection is delegated to sim/sensor_system (fleet-wide union). This module only draws.
// =====================================================================================
void sensor_overlay_draw(game_state* s);

// Editor-tunable: distance (world units) at which a contact's blip fades to its dim floor. Layer 2
// is unbounded, so contacts remain detectable at any range; this only controls how quickly distant
// returns dim. Full brightness up to the flagship's Layer 2 falloff, dim floor at/after this distance.
extern f32 g_sensor_fade_distance;
