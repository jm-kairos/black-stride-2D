#pragma once

#include <defines.h>
#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include "ship.h" // SensorSuite, VesselFaction

struct game_state;

// =====================================================================================
// Sensor system (detection logic only — no rendering).
//
// Every ship carries a SensorSuite with three concentric layers. The fleet's detection
// picture is the UNION of all friendly ships' coverage: a contact's confidence is the
// strongest single reading (max) across the fleet, so overlapping sensor layers simply
// widen coverage and raise confidence without double-counting.
// =====================================================================================

// A detected radar contact: one per hostile projectile the fleet can currently sense.
struct SensorContact {
    bs_math::HierPos2 position;   // world position of the contact
    bs_math::Vec2     velocity;   // world velocity (for heading / lead display)
    f32               confidence; // 0..1: 0 = entering Layer 2, 1 = at/inside Layer 1 (identified)
    VesselFaction     owner;      // who fired it
};

// Per-ship sensor confidence for a target at distance `dist` from that ship.
// Returns clamp((L2 - dist) / (L2 - L1), 0, 1): 0 outside Layer 2, ramping to 1 at Layer 1
// and everywhere inside it.
f32 sensor_reading(const SensorSuite* suite, f32 dist);

// Scan all active hostile projectiles and fill `out` with the ones the fleet can detect.
// Detection is fleet-wide (union): each contact's confidence is the MAX per-ship reading over
// every friendly fleet ship, using that ship's own SensorSuite radii. Only contacts with
// confidence > 0 are written. Returns the number written (clamped to `max_out`).
i32 sensor_gather_hostile_contacts(const game_state* s, SensorContact* out, i32 max_out);
