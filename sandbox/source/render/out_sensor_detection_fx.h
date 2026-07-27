#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
#include "sim/ship.h"

// =====================================================================================
// Out-of-sensor-range contact effect for enemy ships in global/combat mode.
// Draws a glowing radar-style contact signature instead of the real hull sprite.
// =====================================================================================
struct OutSensorDetectionFX {
    bs_color color = bs_color{ 1.0f, 0.25f, 0.25f, 1.0f }; // shared tint
    f32 intensity = 1.0f;   // overall overlay brightness multiplier
    f32 radius_scale = 0.6f; // multiplier on the ship bounding radius for the overlay ring
    bs_glow_params overlay_glow; // per-sprite glow params for the overlay elements
    f32 sweep_speed = 1.5f; // radar sweep rotation speed (rad/s)
    f32 sweep_angle = 0.0f; // current sweep angle (rad)
    f32 ping_phase  = 0.0f; // expanding ping ring phase (0..1)

    void init();
    void update(f32 dt);
    void render(f32 elapsed_time, const Ship* enemy, const Ship* player, f32 sensor_range, f32 zoom);
};
