#include "sim/ship_control.h"
#include "game.h"
#include "core/cursor_world.h" // mouse_true_hierpos
#include <core/input.h> // input_is_key_down, KEY_*
#include <math.h>       // atan2f, sinf, cosf, fabsf

using namespace bs_math;

// ---- Ship CONTROL: pilot input -> forces.
// WASD here are NOT screen-relative; thrust is applied along the ship's heading
// (Starsector-style). The ship coasts (no passive drag); speed only changes via thrust
// (W/S/Q/E) or the brake (C). This function mutates only flight velocities, never the pose --
// integration is simulate_ship's job, so the ship keeps moving even when nobody is piloting.
// Returns TRUE if a turn is actively commanded this frame (A/D held) so simulate_ship knows to
// skip auto-stabilizing the spin; FALSE otherwise.
b8 control_ship_global(game_state* s, FleetShip* pf, f32 dt) {
    if (!pf) return FALSE;
    Ship*       ship = &pf->ship;
    ShipFlight* fl   = &pf->flight;
    // Free camera mode: ship coasts; no pilot input this frame.
    if (s->camera_state.free_camera_active)
        return FALSE;
    // Heading basis: angle 0 => nose points +Y (up), matching the tilemap's nose-at-top.
    Vec2 fwd   = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle); // forward (nose)
    Vec2 right = vec2_rotate(Vec2{ 1.0f, 0.0f }, ship->angle); // starboard
    // Per-ship motion tuning (resolved from the hull's size class at load: a drone is
    // nimble, a cruiser is a slow-turning wall of metal).
    const ShipMotion& m = ship->motion;
    f32 strafe = m.accel; // full strafe thrust
    // ---- Linear thrust (accumulate this frame's acceleration along the heading) ----
    Vec2 acc{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) acc = vec2_add(acc, vec2_scale(fwd,   m.accel)); // forward
    if (input_is_key_down(KEY_S)) acc = vec2_add(acc, vec2_scale(fwd,  -m.decel)); // reverse
    if (input_is_key_down(KEY_E)) acc = vec2_add(acc, vec2_scale(right, strafe));     // strafe right
    if (input_is_key_down(KEY_Q)) acc = vec2_add(acc, vec2_scale(right,-strafe));     // strafe left
    fl->velocity = vec2_add(fl->velocity, vec2_scale(acc, dt));
    // ---- Brake (C): bleed the current velocity toward zero at the decel rate ----
    if (input_is_key_down(KEY_C)) {
        f32 spd = vec2_length(fl->velocity);
        if (spd > 0.0001f) {
            f32 ns = spd - m.decel * dt;
            if (ns < 0.0f) ns = 0.0f;
            fl->velocity = vec2_scale(fl->velocity, ns / spd);
        }
    }
    // ---- Angular: A/D ramp angular velocity toward +/- max. The COMPLEMENTARY auto-stabilize
    // (no turn input -> bleed spin to zero) is deliberately NOT done here; it lives in
    // simulate_ship so it runs in BOTH modes. Reason: simulate_ship integrates angular_velocity
    // into ship->angle every frame regardless of mode, but this control fn only runs while
    // piloting (global). If the stabilizer lived here, spin built up in global mode and carried
    // into local mode would never bleed off and the integrator would rotate the ship forever.
    // Here we only ADD the commanded turn and report whether one was issued so the simulator
    // knows to skip stabilizing the spin this frame.
    f32 turn_in = 0.0f;
    if (s->view.alt_movement_active) {
        // ---- Mouse-follow mode: smoothly rotate toward the mouse cursor ----
        bs_math::HierPos2 mw = mouse_true_hierpos(s);
        Vec2 to = hierpos_diff(&mw, &ship->origin, BS_HIERPOS_CELL_SIZE);
        if (vec2_length(to) > 0.001f) {
            f32 desired = atan2f(-to.x, to.y); // ship angle 0 = nose points +Y
            f32 diff  = atan2f(sinf(desired - ship->angle), cosf(desired - ship->angle));
            if (fabsf(diff) > 0.01f) {
                // PD controller: proportional pulls toward target;
                // derivative term (current spin) damps overshoot.
                turn_in = clampf(diff * 3.0f - fl->angular_velocity * 1.5f, -1.0f, 1.0f);
            } // else: no turn -> simulate_ship stabilizer bleeds residual spin smoothly
        }
    } else {
        if (input_is_key_down(KEY_A)) turn_in += 1.0f; // turn left (CCW)
        if (input_is_key_down(KEY_D)) turn_in -= 1.0f; // turn right (CW)
    }
    if (turn_in != 0.0f) {
        fl->angular_velocity += turn_in * m.turn_accel * dt;
        return TRUE;  // a turn is actively commanded this frame
    }
    return FALSE;     // no turn commanded -> simulate_ship will auto-stabilize the spin
}

// Ship-ship collision response (combat). The enemy hull is an inoperative derelict -- an
// IMMOVABLE obstacle -- so no fleet ship can shove it; each fleet ship is pushed OUT of any
// penetration instead. Called AFTER the fleet has integrated its poses: test each fleet hull's
// oriented bounding box against the enemy (ships_collide -> SAT minimum-translation vector).
// On overlap, translate the ship clear by the FULL MTV and cancel the INWARD component of its
// linear velocity, so the ship slides along the hull instead of sticking or phasing through.
void resolve_ship_collision(game_state* s) {
    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
        FleetShip& fs = s->fleet_state.fleet.at(i);
        Vec2 mtv{ 0.0f, 0.0f };
        if (!ships_collide(&fs.ship, &s->fleet_state.enemy_ship, &mtv)) continue; // hulls clear
        // Push this ship fully out of the enemy hull (enemy immovable).
        fs.ship.origin = hierpos_add_vec2(&fs.ship.origin, mtv);
        f32 mlen = vec2_length(mtv);
        if (mlen > 1.0e-6f) {
            Vec2 n  = vec2_scale(mtv, 1.0f / mlen);       // unit outward normal (enemy -> ship)
            f32  vn = vec2_dot(fs.flight.velocity, n);    // signed speed along that normal
            if (vn < 0.0f)                                 // moving INTO the hull
                fs.flight.velocity = vec2_sub(fs.flight.velocity, vec2_scale(n, vn)); // kill inward part
        }
    }
}

// World-space origin of the ship the player is currently controlling (the manually-piloted
// ship, defaulting to the flagship). Used for camera follow.
HierPos2 piloted_ship_origin(game_state* s) {
    i32 idx = s->rts_controls.piloted_index();
    if (idx < 0 || idx >= s->fleet_state.fleet.count()) idx = 0;
    return s->fleet_state.fleet.at(idx).ship.origin;
}
