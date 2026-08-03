#include "sim/point_defense.h"
#include "game.h"          // game_state, Fleet, Ship, ProjectileSystem, MAX_PROJECTILES, ship_local_to_world
#include "sim/action_log.h" // action_log_push (missile-intercept attribution, Phase E)
#include <math/math_utils.h> // vec2_length, clampf

using namespace bs_math;

void point_defense_update(game_state* s, f32 dt) {
    if (!s) return;
    s->defense_beam_count = 0;
    if (dt <= 0.0f) return;

    Fleet& fleet = s->fleet_state.fleet;

    for (i32 i = 0; i < fleet.count(); ++i) {
        Ship&         sh = fleet.at(i).ship;
        DefenseLaser& L  = sh.point_defense;
        if (!L.enabled) continue;

        // ---- Doctrine (Phase C) --------------------------------------------------------------
        // HOLD is an order, not a suggestion: drop any in-progress lock, no drain, no acquisition.
        if (L.stance == PD_HOLD) {
            L.target_index       = -1;
            L.dwell_remaining    = 0.0f;
            continue;
        }
        // OVERDRIVE: burst insurance, not a sustain mode (drain outruns regen ~2x over).
        const b8  over      = (L.stance == PD_OVERDRIVE);
        const f32 dps       = L.damage_per_second * (over ? 2.0f : 1.0f);
        const f32 drain     = L.cap_drain_per_s   * (over ? 3.0f : 1.0f);
        const f32 retarget  = L.retarget_cooldown * (over ? 0.5f : 1.0f);
        // Engagement gate: fraction of the resolved range PD is allowed to engage inside.
        static const f32 GATE_FRAC[3] = { 0.6f, 0.8f, 1.0f };
        const f32 gate_frac = GATE_FRAC[(L.gate_tier < 3) ? L.gate_tier : 2];

        // Engagement range: 0 => live-coupled to THIS ship's Layer 0 sensor radius, so the
        // laser range tracks the Layer 0 slider automatically. The gate narrows it further.
        // Layer 0 is the close-in "comfort zone": point defense is a last-ditch screen and must
        // not reach out to identification range.
        const f32 range = ((L.range > 0.0f) ? L.range : sh.sensors.layer0_radius) * gate_frac;

        // Tick the retarget cooldown down toward zero.
        if (L.cooldown_remaining > 0.0f) {
            L.cooldown_remaining -= dt;
            if (L.cooldown_remaining < 0.0f) L.cooldown_remaining = 0.0f;
        }

        // ---- Validate the current lock (still active, hostile, inside the gated range) ------
        b8 have_target = FALSE;
        if (L.target_index >= 0 && L.target_index < MAX_PROJECTILES) {
            Projectile& p = s->projectiles.pool[L.target_index];
            if (p.active && p.faction_id != sh.faction_id) {
                f32 d = vec2_length(hierpos_diff(&p.position, &sh.origin, BS_HIERPOS_CELL_SIZE));
                if (d <= range) have_target = TRUE;
            }
            if (!have_target) {
                // Destroyed / left range / expired -> release and start a brief cooldown.
                L.target_index       = -1;
                L.dwell_remaining    = 0.0f;
                L.cooldown_remaining = retarget;
            }
        }

        // ---- Acquire the best hostile projectile in range (if idle and off cooldown) --------
        // Capacitor reserve floor: below it the laser refuses NEW locks (existing locks above
        // keep burning to their dwell end), so PD throttles itself before bottoming the bank.
        // Scoring is doctrine-driven (lower = better): impact time, missiles-first, or nearest.
        const b8 cap_ok = sh.cap_current >= L.reserve_floor * sh.cap_max;
        if (L.target_index < 0 && L.cooldown_remaining <= 0.0f && cap_ok) {
            i32 best       = -1;
            f32 best_score = 1.0e30f;
            for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
                const Projectile& p = s->projectiles.pool[pi];
                if (!p.active) continue;
                if (p.faction_id == sh.faction_id) continue; // never target friendly fire
                Vec2 to_ship = hierpos_diff(&sh.origin, &p.position, BS_HIERPOS_CELL_SIZE);
                f32  d       = vec2_length(to_ship);
                if (d > range) continue;
                f32 score;
                if (L.priority == PD_PRI_NEAREST) {
                    score = d;
                } else {
                    // Time-to-impact: distance over closing speed toward THIS ship; receding
                    // or tangential threats score huge (engaged only when nothing is closing).
                    f32 closing = (d > 1.0e-3f)
                                      ? (p.velocity.x * to_ship.x + p.velocity.y * to_ship.y) / d
                                      : 1.0f;
                    score = (closing > 1.0f) ? (d / closing) : 1.0e6f + d;
                    if (L.priority == PD_PRI_GUIDED_FIRST && p.kind != PROJ_MISSILE)
                        score += 1.0e7f;   // missiles strictly outrank shells
                }
                if (score < best_score) { best_score = score; best = pi; }
            }
            if (best >= 0) {
                L.target_index    = best;
                L.dwell_remaining = L.dwell_time;
                have_target       = TRUE;
            }
        }

        if (!have_target || L.target_index < 0) continue;

        // ---- Capacitor drain: the beam consumes energy per second while active --------------
        // Hitting empty mid-dwell drops the lock (this is the "capacitor dry" saturation
        // failure the feedback phase attributes) and starts the retarget cooldown.
        sh.cap_current -= drain * dt;
        if (sh.cap_current <= 0.0f) {
            sh.cap_current       = 0.0f;
            L.target_index       = -1;
            L.dwell_remaining    = 0.0f;
            L.cooldown_remaining = retarget;
            continue;
        }

        // ---- Fire: apply damage over time and follow the target's exact position ------------
        Projectile& p = s->projectiles.pool[L.target_index];
        p.hp              -= dps * dt;
        L.dwell_remaining -= dt;

        // Swing the PD turret art onto the locked projectile.
        if (sh.point_defense_mount >= 0) {
            Vec2 to_p = hierpos_diff(&p.position, &sh.origin, BS_HIERPOS_CELL_SIZE);
            ship_turret_aim_at(&sh, sh.point_defense_mount, to_p);
        }

        // Record the beam for the overlay (the PD's own hardpoint -> current target position;
        // unmounted PDs fall back to the legacy shared fire offset).
        if (s->defense_beam_count < MAX_DEFENSE_BEAMS) {
            DefenseBeam& beam = s->defense_beams[s->defense_beam_count++];
            beam.origin    = ship_hardpoint_fire_origin(&sh, sh.point_defense_mount);
            beam.target    = p.position;
            f32 frac       = (p.max_hp > 1.0e-4f) ? (1.0f - p.hp / p.max_hp) : 1.0f;
            beam.intensity = clampf(frac, 0.0f, 1.0f);
        }

        if (p.hp <= 0.0f) {
            // Destroyed: free the projectile slot before it advances / collides this frame.
            // Missile intercepts are logged (rare + meaningful); shell kills stay silent.
            if (p.kind == PROJ_MISSILE) action_log_push(s, "PD: missile intercepted");
            p.active = FALSE;
            --s->projectiles.count;
            L.target_index       = -1;
            L.cooldown_remaining = retarget;
        } else if (L.dwell_remaining <= 0.0f) {
            // Dwell elapsed with the target still alive: let the survivor through, re-engage soon.
            L.target_index       = -1;
            L.cooldown_remaining = retarget;
        }
    }
}
