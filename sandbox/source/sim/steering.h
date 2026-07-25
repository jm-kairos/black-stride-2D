#pragma once
#include <defines.h>
#include <math/math_utils.h>   // bs_math::Vec2

struct Ship;
struct ShipFlight;

// =====================================================================================
// Shared steering primitives (General Ship AI + fleet autopilot).
//
// Pure functions operate on LOCAL vectors (e.g. produced by hierpos_diff) so they stay precision-
// safe anywhere in the galaxy. `apply` converts a desired velocity into thrust + turn under a
// ship's limits and integrates the pose. One locomotion layer, reused by any Ship-backed agent.
// =====================================================================================
namespace steering {

// Desired velocity to arrive at a target `to_target` units away, easing down inside slow_radius.
bs_math::Vec2 arrive(bs_math::Vec2 to_target, f32 max_speed, f32 slow_radius);

// Desired velocity to seek (full speed toward target) / flee (full speed away from a threat).
bs_math::Vec2 seek(bs_math::Vec2 to_target, f32 max_speed);
bs_math::Vec2 flee(bs_math::Vec2 from_threat, f32 max_speed);

// Hold a standoff ring: approach when farther than standoff_dist, back off when closer.
bs_math::Vec2 standoff(bs_math::Vec2 to_target, f32 dist, f32 standoff_dist, f32 max_speed);

// Apply a desired velocity: accel-clamp the ship's velocity toward it, cap to max_speed, slew the
// nose toward the travel heading (angle 0 => nose +Y), and integrate the origin. Precision-safe.
void apply(Ship* sh, ShipFlight* fl, bs_math::Vec2 desired_vel,
           f32 accel, f32 max_speed, f32 turn_rate, f32 dt);

// Same as apply, but aim the nose at `face_dir` (e.g. a target) instead of the travel heading.
void apply_face(Ship* sh, ShipFlight* fl, bs_math::Vec2 desired_vel, bs_math::Vec2 face_dir,
                f32 accel, f32 max_speed, f32 turn_rate, f32 dt);

} // namespace steering
