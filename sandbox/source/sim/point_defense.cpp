#include "sim/point_defense.h"
#include "game.h"          // game_state, Fleet, Ship, ProjectileSystem, MAX_PROJECTILES, ship_local_to_world
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

        // Engagement range: 0 => live-coupled to THIS ship's Layer 1 sensor radius, so the
        // laser range tracks the Layer 1 slider automatically.
        const f32 range = (L.range > 0.0f) ? L.range : sh.sensors.layer1_radius;

        // Tick the retarget cooldown down toward zero.
        if (L.cooldown_remaining > 0.0f) {
            L.cooldown_remaining -= dt;
            if (L.cooldown_remaining < 0.0f) L.cooldown_remaining = 0.0f;
        }

        // ---- Validate the current lock (still active, hostile, in range) --------------------
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
                L.cooldown_remaining = L.retarget_cooldown;
            }
        }

        // ---- Acquire the nearest hostile projectile in range (if idle and off cooldown) -----
        if (L.target_index < 0 && L.cooldown_remaining <= 0.0f) {
            i32 best   = -1;
            f32 best_d = range;
            for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
                const Projectile& p = s->projectiles.pool[pi];
                if (!p.active) continue;
                if (p.faction_id == sh.faction_id) continue; // never target friendly fire
                f32 d = vec2_length(hierpos_diff(&p.position, &sh.origin, BS_HIERPOS_CELL_SIZE));
                if (d <= best_d) { best_d = d; best = pi; }
            }
            if (best >= 0) {
                L.target_index    = best;
                L.dwell_remaining = L.dwell_time;
                have_target       = TRUE;
            }
        }

        if (!have_target || L.target_index < 0) continue;

        // ---- Fire: apply damage over time and follow the target's exact position ------------
        Projectile& p = s->projectiles.pool[L.target_index];
        p.hp              -= L.damage_per_second * dt;
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
            p.active = FALSE;
            --s->projectiles.count;
            L.target_index       = -1;
            L.cooldown_remaining = L.retarget_cooldown;
        } else if (L.dwell_remaining <= 0.0f) {
            // Dwell elapsed with the target still alive: let the survivor through, re-engage soon.
            L.target_index       = -1;
            L.cooldown_remaining = L.retarget_cooldown;
        }
    }
}
