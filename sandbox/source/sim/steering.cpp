#include "sim/steering.h"
#include "sim/ship.h"          // Ship (origin / angle)
#include "sim/fleet.h"         // ShipFlight (velocity)
#include <math/bs_hierpos.h>
#include <math.h>

using namespace bs_math;

namespace steering {

Vec2 arrive(Vec2 to_target, f32 max_speed, f32 slow_radius) {
    f32 d = vec2_length(to_target);
    if (d < 1.0f) return Vec2{ 0.0f, 0.0f };
    f32 spd = max_speed;
    if (slow_radius > 1.0f && d < slow_radius) spd = max_speed * (d / slow_radius);
    return vec2_scale(to_target, spd / d);
}

Vec2 seek(Vec2 to_target, f32 max_speed) {
    f32 d = vec2_length(to_target);
    if (d < 0.001f) return Vec2{ 0.0f, 0.0f };
    return vec2_scale(to_target, max_speed / d);
}

Vec2 flee(Vec2 from_threat, f32 max_speed) {
    f32 d = vec2_length(from_threat);
    if (d < 0.001f) return Vec2{ 0.0f, 0.0f };
    return vec2_scale(from_threat, -max_speed / d);
}

Vec2 standoff(Vec2 to_target, f32 dist, f32 standoff_dist, f32 max_speed) {
    if (dist < 0.001f) return Vec2{ 0.0f, 0.0f };
    Vec2 dir = vec2_scale(to_target, 1.0f / dist);
    f32 err  = dist - standoff_dist;                 // >0 too far (approach), <0 too close (back off)
    f32 band = standoff_dist * 0.25f + 1.0f;
    f32 t    = clampf(err / band, -1.0f, 1.0f);
    return vec2_scale(dir, max_speed * t);
}

Vec2 separation(Vec2 to_other, f32 dist, f32 min_sep, f32 max_speed) {
    if (min_sep <= 0.0f || dist >= min_sep || dist < 0.001f) return Vec2{ 0.0f, 0.0f };
    f32 t = 1.0f - dist / min_sep;                   // 0 at the ring -> 1 at contact
    return vec2_scale(to_other, -(max_speed * t) / dist);
}

static void apply_impl(Ship* sh, ShipFlight* fl, Vec2 desired_vel, const Vec2* face_dir,
                       f32 accel, f32 max_speed, f32 turn_rate, f32 dt, b8 integrate) {
    // Accel-clamp the velocity toward the desired velocity.
    Vec2 err = vec2_sub(desired_vel, fl->velocity);
    f32 a  = accel * dt;
    f32 el = vec2_length(err);
    if (el > a) err = vec2_scale(err, a / el);
    fl->velocity = vec2_add(fl->velocity, err);
    f32 spd = vec2_length(fl->velocity);
    if (spd > max_speed) fl->velocity = vec2_scale(fl->velocity, max_speed / spd);
    // Slew the nose (heading convention: angle 0 => nose +Y).
    Vec2 fd = face_dir ? *face_dir : ((spd > 1.0f) ? fl->velocity : desired_vel);
    if (fd.x != 0.0f || fd.y != 0.0f) {
        f32 desired_angle = atan2f(-fd.x, fd.y);
        f32 d = desired_angle - sh->angle;
        while (d >  BS_PI) d -= 2.0f * BS_PI;
        while (d < -BS_PI) d += 2.0f * BS_PI;
        f32 mr = turn_rate * dt;
        if (d >  mr) d =  mr;
        if (d < -mr) d = -mr;
        sh->angle += d;
    }
    // Integrate the pose (precision-safe HierPos2 add) -- skipped for ships whose integrator
    // runs elsewhere (control_face).
    if (integrate)
        sh->origin = hierpos_add_vec2(&sh->origin, vec2_scale(fl->velocity, dt));
}

void apply(Ship* sh, ShipFlight* fl, Vec2 desired_vel, f32 accel, f32 max_speed, f32 turn_rate, f32 dt) {
    apply_impl(sh, fl, desired_vel, nullptr, accel, max_speed, turn_rate, dt, TRUE);
}

void apply_face(Ship* sh, ShipFlight* fl, Vec2 desired_vel, Vec2 face_dir,
                f32 accel, f32 max_speed, f32 turn_rate, f32 dt) {
    apply_impl(sh, fl, desired_vel, &face_dir, accel, max_speed, turn_rate, dt, TRUE);
}

void control_face(Ship* sh, ShipFlight* fl, Vec2 desired_vel, Vec2 face_dir,
                  f32 accel, f32 max_speed, f32 turn_rate, f32 dt) {
    apply_impl(sh, fl, desired_vel, &face_dir, accel, max_speed, turn_rate, dt, FALSE);
}

} // namespace steering
