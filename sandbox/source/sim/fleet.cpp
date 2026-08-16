#include "sim/fleet.h"
#include "sim/steering.h"   // escort station-keeping and avoid both reuse the shared primitives
#include "game.h"
#include "sim/weapon.h"
#include "sim/galaxy_history.h" // galaxy_history_faction_is_hostile (ROE self-acquisition)
#include "sim/action_log.h"     // engagement announcements when a ship picks its own fight
#include <math/math_utils.h>
using namespace bs_math;
// =====================================================================================
// Autopilot tuning (mirrors the legacy RTS order constants).
// =====================================================================================
static constexpr f32 RTS_MOVE_ARRIVE_DIST = 60.0f;   // units
static constexpr f32 RTS_MOVE_STOP_SPEED  = 15.0f;   // units/s
static constexpr f32 RTS_MOVE_FACE_ANGLE  = 0.3f;    // rad; must face target within this to thrust
static constexpr f32 RTS_ATTACK_RANGE      = 10000.0f; // fallback standoff for an UNARMED hull
static constexpr f32 RTS_ATTACK_REACH_FRAC = 0.85f;  // close to this fraction of weapon reach
// ---- Turret-first gunnery (bear-check gating + arc steering + crab approach) ----------
// The hull is not a gun sight: turrets aim themselves, and per-shot arc/slew convergence is
// the validator's job. The hull turns only to unmask an arc or to point the main drive.
// Margin kept inside an arc edge when steering the hull to unmask a weapon, so the turret
// converges comfortably inside its stops instead of chattering on the edge.
static constexpr f32 ROE_ARC_MARGIN = 0.10f;   // rad
// Max heading offset from the closing vector while the approach burn is lit. Inside this
// cone the drive still closes at >= cos(0.6) ~ 83% efficiency and the hull may crab to keep
// a side mount's arc on the target; a plan outside it burns cone-edge and accepts that the
// weapon unmasks only on arrival.
static constexpr f32 RTS_BURN_CONE  = 0.60f;   // rad
// ---- ROE self-acquisition tuning ------------------------------------------------------
// Acquisition envelope: a ship spots targets for itself out to its longest weapon's reach
// times this. Above 1 so an AGGRESSIVE ship starts closing before the target is shootable;
// DEFENSIVE simply tracks until the target enters real reach (it never approaches).
static constexpr f32 ROE_ACQUIRE_RANGE_MUL = 1.5f;
// Drop hysteresis: an auto-acquired target is released once it sits this far beyond the
// acquisition envelope, so a hostile hovering at the edge cannot flicker the engagement.
static constexpr f32 ROE_DROP_RANGE_MUL    = 1.3f;
// How long a faction stays an aggressor after last landing a hit on a fleet hull.
static constexpr f32 ROE_AGGRESSION_S      = 20.0f;
// Focus fire: a hostile already engaged by a wingman scores as if this much closer, so the
// fleet converges on one kill instead of spreading damage across the contact list.
static constexpr f32 ROE_FOCUS_BONUS       = 8000.0f;
// Kiting: an AGGRESSIVE ship backs out when the target pushes inside this fraction of its
// preferred standoff, holding the gun band open. The zone between here and the standoff is
// the hold band -- no thrust either way -- so the two regimes cannot oscillate.
static constexpr f32 RTS_KITE_FRAC         = 0.55f;
static constexpr f32 RTS_ATTACK_STOP_DIST  = 120.0f; // keep this distance from target
static constexpr f32 RTS_ATTACK_MIN_RANGE  = 150.0f; // do not fire closer than this
// Strafing movement controller: how aggressively the ship corrects its velocity toward the
// desired arrival velocity. Projected onto local thrusters (forward/back + lateral).
static constexpr f32 RTS_STRAFE_GAIN = 3.0f;
// Turn-and-burn: a move order farther than this rotates the ship onto the destination and
// rides the MAIN drive; anything closer is precision work (docking-scale hops, formation
// trims) flown on RCS by the strafing controller, where swinging a cruiser's nose around
// twice would cost more time than sliding saves. Sized from the hull with a floor like the
// other hull-derived rings, and comfortably above the vanguard's ~410-unit full-speed
// braking distance, so main-engine cutoff hands over to RCS before the retro brake begins.
static constexpr f32 RTS_MOVE_FACE_DIST_MUL = 3.0f;    // x own bounding radius
static constexpr f32 RTS_MOVE_FACE_DIST_MIN = 1500.0f; // floor for small hulls
// Lateral authority while cruising: cross-track error is TRIMMED at a fraction of full
// thrust, so the side jets read as course corrections, never as a second drive axis.
static constexpr f32 RTS_CRUISE_TRIM_FRAC   = 0.25f;
// Formation layout.
static constexpr f32 FORMATION_SPACING_MUL = 2.5f;   // x max bounding radius
static constexpr f32 FORMATION_MIN_SPACING = 400.0f; // world units floor
// Minimum fleet-ship separation: center distance below the SUM of the two hulls' bounding radii
// times this counts as too close, so at 1.0 the bounding circles never overlap. Sized from the
// hulls, never a constant, like escort's station ring -- and deliberately below both
// FORMATION_SPACING_MUL (2.5x one radius) and ESCORT_STATION_MUL (2.6x), so holding a formation
// slot or an escort station never puts a ship inside the ring it is pushed out of.
static constexpr f32 FLEET_MIN_SEPARATION_MUL = 1.0f;
// =====================================================================================
static f32 normalize_angle(f32 a) {
    while (a > BS_PI) a -= 2.0f * BS_PI;
    while (a < -BS_PI) a += 2.0f * BS_PI;
    return a;
}
static CombatEntity* find_combat_entity_for_ship(game_state* s, Ship* ship) {
    if (!s || !ship) return nullptr;
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (ce->active && ce->ship == ship) return ce;
    }
    return nullptr;
}
// =====================================================================================
// Thruster telemetry: ease one visual channel toward its clamped command. Attack is faster
// than release so a tapped key flares immediately while the flame takes a beat to die --
// the same asymmetry a real engine's spool-down shows, and what keeps the autopilot's
// bang-bang thrust corrections from strobing the jets.
static f32 telemetry_ease(f32 vis, f32 cmd, f32 dt) {
    cmd = clampf(cmd, -1.0f, 1.0f);
    f32 rate = (fabsf(cmd) > fabsf(vis)) ? 14.0f : 6.0f;
    f32 k = rate * dt;
    if (k > 1.0f) k = 1.0f;
    return vis + (cmd - vis) * k;
}
void flight_telemetry_tick(ShipFlight* fl, f32 dt) {
    if (!fl) return;
    fl->thrust_vis.x = telemetry_ease(fl->thrust_vis.x, fl->thrust_cmd.x, dt);
    fl->thrust_vis.y = telemetry_ease(fl->thrust_vis.y, fl->thrust_cmd.y, dt);
    fl->turn_vis     = telemetry_ease(fl->turn_vis,     fl->turn_cmd,     dt);
    // Nozzle thermal state: chases the forward burn on a SECONDS timescale (heat soaks in
    // slower than it radiates nothing -- rise ~1s to glow, fall ~3s to cold), so the bell
    // stays hot through throttle blips and the burn-vs-heat gap marks a cold ignition.
    {
        f32 target = fmaxf(0.0f, fl->thrust_vis.y);
        f32 rate = (target > fl->heat_vis) ? 1.1f : 0.35f;
        f32 k = rate * dt;
        if (k > 1.0f) k = 1.0f;
        fl->heat_vis += (target - fl->heat_vis) * k;
    }
    fl->thrust_cmd = Vec2{ 0.0f, 0.0f };
    fl->turn_cmd   = 0.0f;
}
// =====================================================================================
// FleetShip
// =====================================================================================
void FleetShip::simulate(f32 dt, b8 turn_commanded) {
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    const ShipMotion& m = sh->motion; // per-ship tuning from the hull's size class
    // Auto-stabilize spin toward zero whenever no turn is commanded. Bleeds at the turn-accel
    // rate, mirroring the A/D ramp, so a released turn coasts down the same way it spun up.
    if (!turn_commanded) {
        f32 drop = m.turn_accel * dt;
        if (fl->angular_velocity > 0.0f)      fl->angular_velocity = (fl->angular_velocity > drop) ? fl->angular_velocity - drop : 0.0f;
        else if (fl->angular_velocity < 0.0f) fl->angular_velocity = (fl->angular_velocity < -drop) ? fl->angular_velocity + drop : 0.0f;
    }
    // Clamp linear + angular speed to their caps. The linear cap is the SELECTED
    // speed-limit gear (ship_speed_cap), not motion.max_speed -- this clamp governs the
    // piloted ship too, so the gear is a true per-ship throttle in both control modes.
    f32 cap = ship_speed_cap(sh);
    f32 spd = vec2_length(fl->velocity);
    if (spd > cap) fl->velocity = vec2_scale(fl->velocity, cap / spd);
    fl->angular_velocity = clampf(fl->angular_velocity, -m.max_turn, m.max_turn);
    // Integrate the rigid-body pose.
    sh->origin = hierpos_add_vec2(&sh->origin, vec2_scale(fl->velocity, dt));
    sh->angle += fl->angular_velocity * dt;
    // Consume this tick's thruster commands (every control path has written by now: the
    // pilot runs before the autopilot, the autopilot before this one integrator).
    flight_telemetry_tick(fl, dt);
}
// =====================================================================================
void FleetShip::update_move(f32 dt) {
    if (!has_move_target) return;
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    const ShipMotion& m = sh->motion; // per-ship tuning from the hull's size class
    Vec2 to_target = hierpos_diff(&move_target, &sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Arrival: close enough and nearly stopped.
    if (dist < RTS_MOVE_ARRIVE_DIST && speed < RTS_MOVE_STOP_SPEED) {
        clear_move_target();
        fl->velocity = Vec2{ 0.0f, 0.0f };
        return;
    }
    // Desired velocity: toward the target, capped by the distance we can still brake from
    // and by the ship's selected speed-limit gear (the braking envelope handles a high
    // gear automatically -- the ship just starts braking proportionally further out).
    f32 cap = ship_speed_cap(sh);
    Vec2 dir = (dist > 0.0001f) ? vec2_scale(to_target, 1.0f / dist) : Vec2{ 0.0f, 0.0f };
    f32 desired_speed = (dist > 0.0001f) ? fminf(cap, sqrtf(2.0f * m.decel * dist)) : 0.0f;
    Vec2 desired_vel = vec2_scale(dir, desired_speed);
    Vec2 vel_err = vec2_sub(desired_vel, fl->velocity);
    // Desired acceleration to correct velocity error, then project onto local thrusters.
    Vec2 desired_acc = vec2_scale(vel_err, RTS_STRAFE_GAIN);
    Vec2 fwd   = vec2_rotate(Vec2{ 0.0f, 1.0f }, sh->angle);
    Vec2 right = vec2_rotate(Vec2{ 1.0f, 0.0f }, sh->angle);
    // ---- TURN-AND-BURN regime: far orders fly nose-first on the main drive --------------
    // The ship rotates to FACE the destination and accelerates along its own axis; lateral
    // thrusters are demoted to cross-track trim. The strafing controller below survives as
    // the PRECISION regime -- final approach, docking-scale hops, formation-slot corrections
    // -- where sliding on RCS beats swinging a cruiser's nose around twice. The nose is only
    // ours to steer when no attack order holds it on a target: move+attack coexist by design
    // (strafe toward the destination while the guns track), and two writers on the angle
    // would fight every frame.
    f32 face_dist = fmaxf(ship_bounding_radius(sh) * RTS_MOVE_FACE_DIST_MUL,
                          RTS_MOVE_FACE_DIST_MIN);
    if (!has_attack_target && dist > face_dist) {
        f32 desired_angle = atan2f(-dir.x, dir.y); // heading convention: angle 0 = nose +Y
        f32 angle_diff = normalize_angle(desired_angle - sh->angle);
        f32 max_rot = m.max_turn * dt;
        // Angular ARRIVE: slew rate follows sqrt(2 * turn_accel * |error|), capped at
        // max_turn -- the rotational twin of the linear sqrt(2*decel*dist) envelope above --
        // so the nose sheds rotation over the last few degrees and eases onto the heading
        // instead of slamming from full rate to a dead stop in one tick. The telemetry
        // records the tapering step, so the turn jets fade out with the turn.
        f32 slew = fminf(m.max_turn, sqrtf(2.0f * m.turn_accel * fabsf(angle_diff)));
        f32 step = clampf(angle_diff, -slew * dt, slew * dt);
        sh->angle += step;
        fl->angular_velocity = 0.0f;
        if (max_rot > 0.0f) fl->turn_cmd += clampf(step / max_rot, -1.0f, 1.0f);
        // Main drive lights only once the nose is roughly on. Retro (negative) thrust stays
        // allowed at any angle so leftover forward speed bleeds off while still turning.
        f32 burn_acc = clampf(vec2_dot(desired_acc, fwd), -m.decel, m.accel);
        if (burn_acc > 0.0f && fabsf(angle_diff) > RTS_MOVE_FACE_ANGLE) burn_acc = 0.0f;
        f32 trim_cap = m.accel * RTS_CRUISE_TRIM_FRAC;
        f32 trim_acc = clampf(vec2_dot(desired_acc, right), -trim_cap, trim_cap);
        fl->thrust_cmd.y += (burn_acc >= 0.0f) ? ((m.accel > 0.0f) ? burn_acc / m.accel : 0.0f)
                                               : ((m.decel > 0.0f) ? burn_acc / m.decel : 0.0f);
        fl->thrust_cmd.x += (m.accel > 0.0f) ? trim_acc / m.accel : 0.0f;
        Vec2 burn = vec2_add(vec2_scale(fwd, burn_acc), vec2_scale(right, trim_acc));
        fl->velocity = vec2_add(fl->velocity, vec2_scale(burn, dt));
        f32 cs = vec2_length(fl->velocity);
        if (cs > cap) fl->velocity = vec2_scale(fl->velocity, cap / cs);
        return;
    }
    // ---- PRECISION regime: the strafing controller (no rotation, RCS does the work) ------
    f32 fwd_acc  = vec2_dot(desired_acc, fwd);
    f32 right_acc = vec2_dot(desired_acc, right);
    if (fwd_acc > 0.0f) fwd_acc = fminf(fwd_acc, m.accel);
    else                fwd_acc = fmaxf(fwd_acc, -m.decel);
    right_acc = clampf(right_acc, -m.accel, m.accel);
    // Thruster telemetry: the local-frame projection above IS this controller's thrust
    // command; record it normalized to the caps so the exhaust pass fires the same jets.
    fl->thrust_cmd.y += (fwd_acc >= 0.0f) ? ((m.accel > 0.0f) ? fwd_acc / m.accel : 0.0f)
                                          : ((m.decel > 0.0f) ? fwd_acc / m.decel : 0.0f);
    fl->thrust_cmd.x += (m.accel > 0.0f) ? right_acc / m.accel : 0.0f;
    Vec2 acc = vec2_add(vec2_scale(fwd, fwd_acc), vec2_scale(right, right_acc));
    fl->velocity = vec2_add(fl->velocity, vec2_scale(acc, dt));
    f32 cur_speed = vec2_length(fl->velocity);
    if (cur_speed > cap) fl->velocity = vec2_scale(fl->velocity, cap / cur_speed);
}
// =====================================================================================
// ROE self-acquisition: the autopilot picks its own fight. Runs before update_attack each
// tick, only for hulls the player is not hand-flying (update_autopilot's auto_skip). A
// player designation always wins -- this pass never touches a target that arrived without
// auto_acquired -- and target death/defection stays update_attack's job (it already clears
// dead or turned-friendly targets); this pass adds only the envelope drop for auto targets.
// Firing itself still runs through update_attack's stance gates and the shared validator,
// so ROE changes WHO gets shot at, never HOW.
void FleetShip::update_engagement(game_state* s) {
    Ship* sh = &ship;
    // The acquisition yardstick is the longest TACTICAL range aboard -- an unarmed hull never
    // self-acquires (nothing to fire; approaching would be theatre). weapon_engage_range, not
    // effective_reach: a missile's flight endurance would put the envelope at megameters and
    // the ship would pick fights with everything its sensors ever see.
    f32 best_reach = 0.0f;
    for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi) {
        f32 r = weapon_engage_range(sh->mounts[hpi]);
        if (r > best_reach) best_reach = r;
    }
    b8 stance_refuses = (stance == FLEET_STANCE_PASSIVE || stance == FLEET_STANCE_HOLD_FIRE);
    // Drop a stale AUTO target: ROE tightened, stance turned pacifist, hull disarmed, or the
    // target left the envelope (with hysteresis). Player-designated targets are never touched.
    if (has_attack_target && auto_acquired) {
        b8 drop = (roe == ROE_HOLD) || stance_refuses || (best_reach <= 0.0f) || !attack_target;
        if (!drop) {
            Vec2 to_t = hierpos_diff(&attack_target->origin, &sh->origin);
            drop = vec2_length(to_t) > best_reach * ROE_ACQUIRE_RANGE_MUL * ROE_DROP_RANGE_MUL;
        }
        if (drop) clear_attack_target();
    }
    if (has_attack_target) return;   // designated, or a still-valid auto target
    if (roe == ROE_HOLD || stance_refuses || best_reach <= 0.0f) return;
    // Nearest hostile ship inside the acquisition envelope. Hostility is the per-faction
    // query the projectile hit path uses -- not a hardcoded faction compare -- and
    // RETURN_FIRE additionally requires the target's faction to hold a live aggression
    // entry (someone of theirs recently hit someone of ours).
    const Fleet& fleet = s->fleet_state.fleet;
    const f32 acquire_r   = best_reach * ROE_ACQUIRE_RANGE_MUL;
    f32   best_score  = 1.0e30f;
    Ship* best_target = nullptr;
    for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
        const CombatEntity* ce = &s->combat_entities[ci];
        if (!ce->active || !ce->ship) continue;
        if (ce->faction_id == sh->faction_id) continue;
        if (!galaxy_history_faction_is_hostile(s, ce->faction_id)) continue;
        if (roe == ROE_RETURN_FIRE && !fleet.is_aggressor(ce->faction_id)) continue;
        Vec2 to = hierpos_diff(&ce->position, &sh->origin);
        f32 d = vec2_length(to);
        if (d > acquire_r) continue;   // hard envelope; the focus bonus never extends it
        // Focus fire: converge on a wingman's pick -- a target already under fleet attack
        // scores as if ROE_FOCUS_BONUS closer, so escorts mass fire instead of spreading it.
        f32 score = d;
        for (i32 fi = 0; fi < fleet.count(); ++fi) {
            const FleetShip& other = fleet.at(fi);
            if (&other == this) continue;
            if (other.has_attack_target && other.attack_target == ce->ship) {
                score -= ROE_FOCUS_BONUS;
                break;
            }
        }
        if (score < best_score) { best_score = score; best_target = ce->ship; }
    }
    if (!best_target) return;
    has_attack_target = TRUE;
    attack_target     = best_target;
    auto_acquired     = TRUE;
    action_log_push(s, "%s: engaging %s",
                    sh->vessel_name ? sh->vessel_name : "Ship",
                    best_target->vessel_name ? best_target->vessel_name : "hostile");
}
// =====================================================================================
void FleetShip::update_attack(game_state* s, f32 dt) {
    if (!has_attack_target) return;
    // PASSIVE refuses the engagement outright rather than holding fire while tracking: a ship
    // told not to fight should not be turning its nose at things either, or it drifts off station
    // chasing a heading. Clearing the order rather than early-returning makes that visible in the
    // HUD instead of leaving a live order the ship silently ignores.
    if (stance == FLEET_STANCE_PASSIVE) { clear_attack_target(); return; }
    if (!attack_target) { clear_attack_target(); return; }
    CombatEntity* ce = find_combat_entity_for_ship(s, attack_target);
    if (!ce || !ce->active || !ce->ship) { clear_attack_target(); return; }
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    const ShipMotion& m = sh->motion; // per-ship tuning from the hull's size class
    if (ce->faction == sh->faction) { clear_attack_target(); return; } // no longer hostile
    Ship* target   = ce->ship;
    f32 ship_r     = ship_bounding_radius(sh);
    f32 target_r   = ship_bounding_radius(target);
    Vec2 to_target = hierpos_diff(&target->origin, &sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Engagement geometry is driven by the WEAPONS THIS SHIP ACTUALLY CARRIES, not by a fixed
    // distance. `fire_reach` is the FLIGHT reach of the weapon that bears on the target
    // (weapon_effective_reach: can a shot physically arrive); `approach_reach` is the longest
    // TACTICAL range aboard (weapon_engage_range, the same function the HUD ring draws: how
    // close is worth flying). The two split at missiles -- a harpoon's shot can arrive from
    // 4,000,000 units out but is only worth fighting at its authored engage_range, and feeding
    // the flight number in here made mounting a launcher read as the ship fleeing the fight
    // (standoff 3.4M, kite band 1.87M -- every real combat distance was "too close").
    // Weapon choice honours the player's micro-selection override: when one is set, ONLY that
    // mount engages, and the approach distance follows ITS reach rather than the longest reach
    // aboard -- so picking a short cannon makes the ship close in instead of loitering at the
    // missile standoff, firing nothing. Unset (-1: every AI hull and every escort) keeps the
    // legacy "best weapon that bears" behaviour untouched.
    i32 whp;
    if (sh->weapon_override >= 0) {
        whp = sh->weapon_override;
        if (!sh->mounts[whp] || !ship_hardpoint_can_aim(sh, whp, to_target)) whp = -1;
    } else {
        whp = ship_select_bearing_weapon(sh, to_target);
    }
    // A MODE_FLAK gun never engages SHIPS: flak shells cannot hit hulls (the collision pass
    // skips PROJ_FLAK), so hull-tracking one is pure waste -- and the every-frame aim
    // assertion here was the bug that starved the autonomous screen: it dragged the barrel
    // back onto the hull target each tick, the screen re-aimed it at the ordnance a moment
    // later, and the turret ping-ponged between the two goals without ever converging inside
    // the validator's slew gate. A screen CLAIM (any mount defending against ordnance this
    // tick, AP included) yields the same way -- defense preempts offense for the mount.
    // A SKILL claim (a fleet-skill volley pending on the mount, sim/skill_system.cpp) yields
    // identically: an explicit cast outranks the standing order for the tubes it committed,
    // or this aim re-assertion would hold a volley tube off the player's target forever.
    if (whp >= 0 && ((sh->mounts[whp]->wkind == WEAPON_KIND_BALLISTIC &&
                      static_cast<BallisticWeapon*>(sh->mounts[whp])->fire_mode == MODE_FLAK) ||
                     sh->flak_screen_claimed[whp] || sh->skill_claimed[whp])) whp = -1;
    Weapon* w = (whp >= 0) ? sh->mounts[whp] : nullptr;
    // ONLY the engaging mount tracks the designated target. Every other turret is left alone so
    // the weapons in the player's fire selection keep following the cursor (game_update sets that
    // aim earlier in the same frame; this runs after it and would otherwise stomp it). The result
    // is what the player expects while shooting under a standing designation: the launcher holds
    // on its target, the guns go where they are pointed.
    if (whp >= 0) ship_turret_aim_at(sh, whp, to_target);
    f32 fire_reach = weapon_effective_reach(w);
    f32 approach_reach = 0.0f;
    f32 gun_reach      = 0.0f;   // longest BALLISTIC reach aboard (missile CONSERVE gate)
    if (sh->weapon_override >= 0) {
        Weapon* ow = sh->mounts[sh->weapon_override];
        approach_reach = weapon_engage_range(ow);
        if (ow && ow->wkind == WEAPON_KIND_BALLISTIC) gun_reach = approach_reach;
    } else {
        for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi) {
            f32 r = weapon_engage_range(sh->mounts[hpi]);
            if (r > approach_reach) approach_reach = r;
            if (sh->mounts[hpi] && sh->mounts[hpi]->wkind == WEAPON_KIND_BALLISTIC &&
                r > gun_reach) gun_reach = r;
        }
    }
    if (approach_reach <= 0.0f) approach_reach = RTS_ATTACK_RANGE;   // unarmed: legacy standoff
    // ---- The hull turns ONLY when turning buys something ---------------------------------
    // Turrets aim themselves; the hull is not a gun sight. Rotation is commanded in exactly
    // two cases: the ship must BURN toward the target (main-drive thrust is nose-directed),
    // or NO mounted weapon's traverse arc covers the target from the current heading -- and
    // then only by the MINIMAL angle that unmasks the nearest arc (edge minus ROE_ARC_MARGIN),
    // so a broadside hull presents its flank instead of its bow. The old code steered nose-on
    // unconditionally and gated every shot on hull alignment, which left a converged side
    // turret silent while the ship pirouetted.
    f32 target_bearing = atan2f(-to_target.x, to_target.y); // heading convention: nose = +Y
    f32 plan_delta = 0.0f;   // minimal hull rotation that lets some weapon bear
    b8  have_plan  = FALSE;  // any weapon mounted at all (respecting the override)
    for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi) {
        if (!sh->mounts[hpi]) continue;
        if (sh->weapon_override >= 0 && hpi != sh->weapon_override) continue;
        const HardpointDef& hd = sh->hardpoints[hpi];
        f32 delta;
        if (hd.arc >= 2.0f * BS_PI - 1.0e-3f) {
            delta = 0.0f;                                    // full-circle turret: always bears
        } else {
            f32 inner = fmaxf(hd.arc * 0.5f - ROE_ARC_MARGIN, 0.0f);
            f32 err   = normalize_angle(target_bearing - hd.facing - sh->angle);
            delta = (fabsf(err) <= inner) ? 0.0f : err - (err > 0.0f ? inner : -inner);
        }
        if (!have_plan || fabsf(delta) < fabsf(plan_delta)) plan_delta = delta;
        have_plan = TRUE;
        if (plan_delta == 0.0f) break;                       // cannot beat "already bearing"
    }
    // Approach need (AGGRESSIVE, no move order, outside the standoff): decided here because
    // the heading choice depends on it. Close to a comfortable margin inside the longest
    // weapon's reach and hold there -- a ship with 4,000,000 units of missile reach should
    // engage from range, not fly to knife-fighting distance the way the old fixed
    // 10,000-unit standoff forced it to.
    f32 desired_dist = approach_reach * RTS_ATTACK_REACH_FRAC;
    f32 min_standoff = RTS_ATTACK_STOP_DIST + target_r + ship_r;
    if (desired_dist < min_standoff) desired_dist = min_standoff;
    f32 brake_dist = (speed > 0.0f) ? (speed * speed) / (2.0f * m.decel) : 0.0f;
    // An AGGRESSIVE ship that picked its OWN fight leaves its station to fight it -- the
    // stance's documented meaning. Without this, a standing move order pins the hull in
    // place while it tracks a target it never closes on: formation slots are the systematic
    // case (the separation post-pass can hold a ship NEAR its slot but outside update_move's
    // 60-unit arrival deadband forever, so the order never self-clears). Player-DESIGNATED
    // attack+move keeps the deliberate strafe-while-firing coexistence -- only auto-acquired
    // hunts override the station.
    if (auto_acquired && has_move_target && stance == FLEET_STANCE_AGGRESSIVE &&
        dist > desired_dist + brake_dist) {
        clear_move_target();
        action_log_push(s, "%s: breaking station to engage",
                        sh->vessel_name ? sh->vessel_name : "Ship");
    }
    b8 need_approach = (!has_move_target && stance == FLEET_STANCE_AGGRESSIVE &&
                        dist > desired_dist + brake_dist);
    f32 desired_heading;
    if (need_approach) {
        // Crab approach: hold the arc plan when it fits inside the burn cone around the
        // closing vector, else burn at the cone edge nearest it. The drive still closes at
        // >= cos(RTS_BURN_CONE) efficiency and a side mount stays unmasked the whole way.
        f32 plan_off = have_plan
            ? normalize_angle(sh->angle + plan_delta - target_bearing) : 0.0f;
        desired_heading = target_bearing + clampf(plan_off, -RTS_BURN_CONE, RTS_BURN_CONE);
    } else if (have_plan) {
        // In range (or holding station): unmask the nearest arc and stop -- ZERO rotation
        // when a weapon already bears from the current heading.
        desired_heading = sh->angle + plan_delta;
    } else {
        desired_heading = target_bearing;   // no weapons mounted: legacy nose-on tracking
    }
    f32 angle_diff = normalize_angle(desired_heading - sh->angle);
    f32 max_rot = m.max_turn * dt;
    if (fabsf(angle_diff) > 1.0e-4f) {
        // Angular ARRIVE, same as update_move's cruise turn: ease onto the goal heading over
        // the last few degrees rather than snapping to a dead stop the tick the error fits
        // in one step.
        f32 slew = fminf(m.max_turn, sqrtf(2.0f * m.turn_accel * fabsf(angle_diff)));
        f32 step = clampf(angle_diff, -slew * dt, slew * dt);
        sh->angle += step;
        // Thruster telemetry: this controller turns by writing the pose directly, so the
        // commanded turn is the applied rotation as a fraction of this tick's cap.
        if (max_rot > 0.0f) fl->turn_cmd += clampf(step / max_rot, -1.0f, 1.0f);
    }
    fl->angular_velocity = 0.0f;
    // Only approach the target if we have no separate move order. When both orders are set,
    // movement is handled by update_move and we just track/fire here.
    //
    // DEFENSIVE also declines to approach. That is the entire difference between it and
    // AGGRESSIVE: both fight, but a defensive ship fights from wherever its order put it rather
    // than closing to its own weapons' preferred range. It is what makes "hold this station and
    // shoot what comes" expressible without a second order type, and it is why a screening ship
    // set defensive stops wandering off the thing it was screening.
    if (!has_move_target && stance == FLEET_STANCE_AGGRESSIVE) {
        // The burn gate compares against the PLANNED heading, not the target bearing -- the
        // plan may be crabbed up to RTS_BURN_CONE off the closing vector, and the old nose-on
        // gate would never open for a crab.
        if (fabsf(angle_diff) < RTS_MOVE_FACE_ANGLE) {
            Vec2 heading = vec2_rotate(Vec2{ 0.0f, 1.0f }, sh->angle);
            // Only close distance when too far; never actively back away from the target.
            if (dist > desired_dist + brake_dist) {
                fl->velocity = vec2_add(fl->velocity, vec2_scale(heading, m.accel * dt));
                fl->thrust_cmd.y += 1.0f;   // telemetry: main engines, full burn
                // Cross-track damping: a crabbed burn builds velocity perpendicular to the
                // target line; bleed it at cruise-trim strength so the approach stays a
                // line, not a spiral. Reads as course-holding side jets, which it is.
                if (dist > 0.0001f) {
                    Vec2 tdir = vec2_scale(to_target, 1.0f / dist);
                    Vec2 perp = Vec2{ -tdir.y, tdir.x };
                    f32 lat  = vec2_dot(fl->velocity, perp);
                    f32 trim = m.accel * RTS_CRUISE_TRIM_FRAC * dt;
                    f32 corr = clampf(-lat, -trim, trim);
                    fl->velocity = vec2_add(fl->velocity, vec2_scale(perp, corr));
                    if (m.accel * dt > 0.0f) {
                        Vec2 corr_local = vec2_rotate(perp, -sh->angle);
                        fl->thrust_cmd = vec2_add(fl->thrust_cmd,
                            vec2_scale(corr_local, corr / (m.accel * dt)));
                    }
                }
            } else if (dist < desired_dist * RTS_KITE_FRAC && dist > 0.0001f) {
                // KITE: the target pushed well inside the preferred band -- back out on the
                // strafing controller while the guns stay unmasked (the arc plan above holds
                // the heading; no nose flip). Clamped to the same per-axis authority as the
                // precision regime, so retro and side jets read as a controlled fallback,
                // never a second drive. The zone between here and the standoff brakes only,
                // so kite and approach cannot oscillate.
                Vec2 tdir = vec2_scale(to_target, 1.0f / dist);
                Vec2 away = vec2_scale(tdir, -m.accel);
                Vec2 fwd2 = vec2_rotate(Vec2{ 0.0f, 1.0f }, sh->angle);
                Vec2 rgt2 = vec2_rotate(Vec2{ 1.0f, 0.0f }, sh->angle);
                f32 fwd_acc   = clampf(vec2_dot(away, fwd2), -m.decel, m.accel);
                f32 right_acc = clampf(vec2_dot(away, rgt2), -m.accel, m.accel);
                fl->thrust_cmd.y += (fwd_acc >= 0.0f) ? ((m.accel > 0.0f) ? fwd_acc / m.accel : 0.0f)
                                                      : ((m.decel > 0.0f) ? fwd_acc / m.decel : 0.0f);
                fl->thrust_cmd.x += (m.accel > 0.0f) ? right_acc / m.accel : 0.0f;
                fl->velocity = vec2_add(fl->velocity,
                    vec2_scale(vec2_add(vec2_scale(fwd2, fwd_acc), vec2_scale(rgt2, right_acc)), dt));
            } else if (speed > 0.0f) {
                fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -m.decel * dt / speed));
                // Telemetry: a velocity-opposing brake decomposes onto whichever local
                // thrusters point against the drift (|velocity| is frame-independent).
                fl->thrust_cmd = vec2_add(fl->thrust_cmd,
                    vec2_scale(vec2_rotate(fl->velocity, -sh->angle), -1.0f / speed));
            }
        } else if (speed > 0.0f) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -m.decel * 0.5f * dt / speed));
            fl->thrust_cmd = vec2_add(fl->thrust_cmd,
                vec2_scale(vec2_rotate(fl->velocity, -sh->angle), -0.5f / speed));
        }
    }
    // Fire when outside minimum range and within each weapon's OWN reach -- so a long-legged
    // launcher engages at its full authored range instead of being capped by a hardcoded
    // constant. The old HULL-alignment gate is gone: per-mount bearing is the real "aligned",
    // and the shared validator already enforces every mount's traverse arc AND barrel slew
    // convergence (WEAPON_FIRE_NO_BEARING / WEAPON_FIRE_SLEWING), so a converged side turret
    // fires regardless of where the nose points.
    //
    // EVERY autopilot-driven ship fires EVERYTHING under an attack order -- missiles and
    // ballistics alike. The player-vs-autopilot arbitration happens one level up: the hull the
    // player is hand-flying is skipped by update_autopilot entirely (auto_skip), and manual
    // gunnery is attached-only (detached, LMB is selection -- see game.cpp's fire gate), so
    // there is no cursor for these guns to fight. "Designate and it shoots" is what an RTS
    // attack order means.
    //
    // Note this reaches ONLY the player's own fleet: update_autopilot walks Fleet::m_ships, while
    // NPC agents fire through sim/ai_ship.cpp and the static enemy through combat_arena.cpp.
    // All three lead their shots the same way, so fleet gunners shoot like the NPCs do.
    // HOLD_FIRE keeps everything above -- the nose tracks, the turrets slew, the ship holds its
    // station -- and stops only at the trigger. That is deliberately a different thing from
    // PASSIVE: this ship is shadowing a target and ready, it is just not authorised to shoot.
    b8 fire_authorized = (stance != FLEET_STANCE_HOLD_FIRE);
    // Offensive capacitor floor (doctrine): below it the guns and launchers hold while the
    // turrets keep tracking -- the same shape as HOLD_FIRE, but temporary and automatic.
    // The defensive systems keep their own LOWER reserve (PD's 0.15), so the power budget
    // layers: offense yields first, the screens fight on.
    if (sh->cap_current < cap_fire_floor * sh->cap_max) fire_authorized = FALSE;
    b8 range_ok = (dist >= RTS_ATTACK_MIN_RANGE);
    b8 guided = (w && w->wkind == WEAPON_KIND_MISSILE && fire_authorized);
    // Missile release policy: HOLD never launches autonomously; CONSERVE spends ordnance
    // only where the guns cannot do the work. The manual trigger is never gated by either.
    if (missile_policy == MISSILE_HOLD) guided = FALSE;
    else if (missile_policy == MISSILE_CONSERVE && gun_reach > 0.0f && dist <= gun_reach)
        guided = FALSE;
    if (w && guided && range_ok && dist <= fire_reach) {
        {
            // Each shot leaves from its own hardpoint, honoring the slot's traverse arc.
            {
                HierPos2 fire_origin = ship_hardpoint_fire_origin(sh, whp);
                Vec2 aim_dir = weapon_lead_dir(fire_origin, target->origin, ce->velocity,
                                               w->projectile_speed(), to_target, dist);
                w->owner_faction_id = sh->faction_id;   // stamp attacker faction for hit attribution
                // One shared validator (arc / cooldown / reach / power / operational status),
                // the same one the manual fire path and the selection hub use. The capacitor is
                // spent only once it passes, so a weapon on cooldown never drains the bank.
                if (ship_weapon_fire_state(sh, whp, to_target, dist) == WEAPON_FIRE_READY &&
                    ship_try_spend_cap(sh, w->cap_cost()))
                    ship_hardpoint_fire(sh, whp, aim_dir, fl->velocity, &s->projectiles);
            }
        }
    }
    // Ballistic gunnery: a BROADSIDE, not just the bearing weapon -- every ballistic mount
    // whose traverse bears trains onto its own lead solution and fires through the shared
    // validator, so per-mount arc, slew convergence, cooldown, reach and capacitor all gate
    // exactly as they do for the manual trigger. Approach distance is unchanged
    // (approach_reach above); this only decides what shoots once the geometry is set.
    {
        // The shell inherits this hull's velocity (ship_hardpoint_fire passes fl->velocity), so
        // the intercept is driven by the RELATIVE velocity -- the same two-pass solve as the NPC
        // gunner in sim/ai_ship.cpp (AI_ATTACK), which is the marksmanship fleet AI should match.
        Vec2 rvel = vec2_sub(ce->velocity, fl->velocity);
        for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi) {
            Weapon* bw = sh->mounts[hpi];
            if (!bw || bw->wkind != WEAPON_KIND_BALLISTIC) continue;
            // The player's micro-selection narrows engagement to that one mount, same as whp.
            if (sh->weapon_override >= 0 && hpi != sh->weapon_override) continue;
            // MODE_FLAK mounts sit the broadside out entirely -- flak cannot hit hulls, and
            // aim-thrash between hull duty and screen duty was what starved the screen (see
            // the whp demotion above and sim/flak_screen.cpp). A screen-claimed AP mount is
            // on defense duty this tick and yields the same way.
            if (static_cast<BallisticWeapon*>(bw)->fire_mode == MODE_FLAK ||
                sh->flak_screen_claimed[hpi]) continue;
            // Per-mount geometry: each shot leaves from its own hardpoint, lead solved from there.
            HierPos2 fire_origin = ship_hardpoint_fire_origin(sh, hpi);
            Vec2 to_t = hierpos_diff(&target->origin, &fire_origin);
            f32  d    = vec2_length(to_t);
            Vec2 aim  = to_t;
            f32  pspd = bw->projectile_speed();
            if (pspd > 1.0f) {
                for (i32 it = 0; it < 2; ++it) {
                    f32 tof = vec2_length(aim) / pspd;
                    aim = vec2_add(to_t, vec2_scale(rvel, tof));
                }
            }
            if (!ship_hardpoint_can_aim(sh, hpi, aim)) continue;
            // Aim at the LEAD point, not the hull: the validator's slew gate compares the barrel
            // against the direction FIRED, so a mount trained on the hull with a wide lead angle
            // would report WEAPON_FIRE_SLEWING forever. This is also the aim that runs after the
            // engaging-mount track above, so for a ballistic whp the lead goal wins the frame.
            // Deliberately OUTSIDE the fire gates: turrets keep tracking under HOLD_FIRE and
            // while the nose is still swinging onto the target.
            ship_turret_aim_at(sh, hpi, aim);
            if (!fire_authorized || !range_ok) continue;
            bw->owner_faction_id = sh->faction_id;   // stamp attacker faction for hit attribution
            // One shared validator + committed spend + one shared spawner: the exact per-weapon
            // path the player's trigger loop walks (game.cpp), never a parallel spawn path.
            if (ship_weapon_fire_state(sh, hpi, aim, d) == WEAPON_FIRE_READY &&
                ship_try_spend_cap(sh, bw->cap_cost()))
                ship_hardpoint_fire(sh, hpi, aim, fl->velocity, &s->projectiles);
        }
    }
    f32 cur_speed = vec2_length(fl->velocity);
    f32 cap = ship_speed_cap(sh);
    if (cur_speed > cap) fl->velocity = vec2_scale(fl->velocity, cap / cur_speed);
}
// =====================================================================================
void FleetShip::apply_card_doctrine() {
    const ShipDef* d = ship.def;
    if (!d) return;
    if (d->doct_roe     != 0xFF)   roe            = d->doct_roe;
    if (d->doct_flak    != 0xFF)   flak_doctrine  = d->doct_flak;
    if (d->doct_missile != 0xFF)   missile_policy = d->doct_missile;
    if (d->doct_cap_floor >= 0.0f) cap_fire_floor = d->doct_cap_floor;
}
void FleetShip::clear_move_target() {
    has_move_target = FALSE;
    move_target = HierPos2{};
}
void FleetShip::clear_attack_target() {
    has_attack_target = FALSE;
    attack_target = nullptr;
    auto_acquired = FALSE;
}
void FleetShip::clear_escort_target() {
    has_escort_target = FALSE;
    escort_target = nullptr;
}
void FleetShip::clear_avoid_target() {
    has_avoid_target = FALSE;
    avoid_target = nullptr;
}
// Station-keeping distance for an escort, in multiples of the ESCORTEE's bounding radius. A
// multiple rather than a constant because a corvette shadowing a cruiser has to sit further out
// than one shadowing a drone simply to clear the hull.
static constexpr f32 ESCORT_STATION_MUL = 2.6f;
static constexpr f32 ESCORT_MIN_STATION = 400.0f;
void FleetShip::update_escort(f32 dt) {
    if (!has_escort_target) return;
    if (!escort_target || escort_target == &ship) { clear_escort_target(); return; }
    Ship* sh = &ship;
    const ShipMotion& m = sh->motion;
    Vec2 to_t = hierpos_diff(&escort_target->origin, &sh->origin);
    f32  dist = vec2_length(to_t);
    f32  station = ship_bounding_radius(escort_target) * ESCORT_STATION_MUL;
    if (station < ESCORT_MIN_STATION) station = ESCORT_MIN_STATION;
    // steering::standoff already expresses "approach when far, back off when close" with a
    // deadband, which is exactly the leash Starsector's escort orders describe -- reusing it
    // keeps escorts out of the oscillation a bare seek would produce at the ring.
    Vec2 desired = steering::standoff(to_t, dist, station, ship_speed_cap(sh));
    // NEAR the ring, face the escortee rather than the travel heading: a screening ship keeps
    // its guns and shields oriented on what it is protecting, not on whichever way
    // station-keeping happens to be nudging it that frame. FAR off the ring it is in a
    // catch-up cruise, so it faces its TRAVEL direction and rides the main drive instead --
    // a cross-system catch-up flown broadside reads absurd now that the thrusters are honest.
    // control_face, not apply_face: FleetShip::simulate integrates every fleet member's pose,
    // so the integrating form moved the ship twice per frame -- escorts flew at double speed.
    Vec2 face = (dist > station * 3.0f) ? desired : to_t;
    steering::control_face(sh, &flight, desired, face, m.accel, ship_speed_cap(sh), m.max_turn, dt);
}
// How far outside the threat's reach an avoiding ship tries to get. Slightly over 1 so it does
// not sit exactly on the boundary and flip between running and idling every frame.
static constexpr f32 AVOID_CLEAR_MUL = 1.25f;
void FleetShip::update_avoid(f32 dt) {
    if (!has_avoid_target) return;
    if (!avoid_target) { clear_avoid_target(); return; }
    Ship* sh = &ship;
    const ShipMotion& m = sh->motion;
    Vec2 to_t = hierpos_diff(&avoid_target->origin, &sh->origin);
    f32  dist = vec2_length(to_t);
    // Clear range is the THREAT's longest TACTICAL range, not a fixed number, so avoiding a
    // sniper means running further than avoiding a knife-fighter -- the same
    // weapon_engage_range the HUD ring and the engagement logic use. Not flight reach: a
    // missile-armed threat would otherwise demand a 5,000,000-unit run before the order
    // ever cleared.
    f32 threat = 0.0f;
    for (i32 hpi = 0; hpi < avoid_target->hardpoint_count; ++hpi) {
        f32 r = weapon_engage_range(avoid_target->mounts[hpi]);
        if (r > threat) threat = r;
    }
    if (threat <= 0.0f) threat = RTS_ATTACK_RANGE;
    if (dist > threat * AVOID_CLEAR_MUL) { clear_avoid_target(); return; }  // clear: order done
    // Nose stays ON the threat while running: backing away facing the enemy keeps forward
    // weapons and the strongest arc pointed at it, which is what withdrawing under fire wants.
    Vec2 desired = steering::flee(to_t, ship_speed_cap(sh));
    // control_face for the same reason as update_escort: simulate owns pose integration.
    steering::control_face(sh, &flight, desired, to_t, m.accel, ship_speed_cap(sh), m.max_turn, dt);
}
// =====================================================================================
// Fleet
// =====================================================================================
Fleet::Fleet() : m_count(0), m_spawned(0), m_piloted_idx(0) {
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i) { m_aggressor_id[i] = 0; m_aggressor_timer[i] = 0.0f; }
}
void Fleet::init() {
    m_count = 0; m_spawned = 0; m_piloted_idx = 0;
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i) { m_aggressor_id[i] = 0; m_aggressor_timer[i] = 0.0f; }
}
void Fleet::set_count(i32 n) {
    if (n < 1) n = 1;
    if (n > m_spawned) n = m_spawned;
    if (n < 1) n = 1;
    // Clear state on slots that are about to become hidden so a later restore is clean.
    for (i32 i = n; i < m_count; ++i) {
        m_ships[i].selected = FALSE;
        m_ships[i].clear_move_target();
        m_ships[i].clear_attack_target();
    }
    m_count = n;
    if (m_piloted_idx >= m_count) m_piloted_idx = 0;
}
void Fleet::set_piloted(i32 idx) {
    if (idx < 0 || idx >= m_count) idx = 0;
    m_piloted_idx = idx;
    // A piloted ship stops following RTS orders.
    m_ships[idx].clear_move_target();
    m_ships[idx].clear_attack_target();
}
void Fleet::set_selected(i32 idx, b8 selected) {
    if (idx >= 0 && idx < m_count) m_ships[idx].selected = selected;
}
b8 Fleet::any_selected() const {
    for (i32 i = 0; i < m_count; ++i) if (m_ships[i].selected) return TRUE;
    return FALSE;
}
FleetShip& Fleet::add() {
    if (m_count >= FLEET_MAX_SHIPS) return m_ships[0];
    FleetShip& fs = m_ships[m_count++];
    fs.ship_type = SHIP_TYPE_DRONE;
    fs.selected = FALSE;
    fs.has_move_target = FALSE;
    fs.has_attack_target = FALSE;
    fs.jump_capable = TRUE;
    fs.jump_radius = JUMP_RADIUS_DEFAULT;
    // AGGRESSIVE is the default because it is the pre-stance behaviour: engage and close. Any
    // other default would silently change how every existing escort fights.
    fs.stance = FLEET_STANCE_AGGRESSIVE;
    // WEAPONS FREE is the default deliberately: a warship that watches its fleet get shot
    // without responding is the bug this field exists to fix. ROE_HOLD reproduces the old
    // designation-only behaviour for players who want full manual control.
    fs.roe = ROE_WEAPONS_FREE;
    // AUTO for the same reason: a missile screen that waits for permission is not a screen.
    fs.flak_doctrine = FLAK_AUTO;
    fs.missile_policy = MISSILE_FREE;  // legacy launch behaviour; CONSERVE/HOLD are opt-in
    fs.cap_fire_floor = 0.25f;         // offense yields first; defenses spend to the 0.15 reserve
    fs.auto_acquired = FALSE;
    fs.has_escort_target = FALSE;
    fs.escort_target     = nullptr;
    fs.has_avoid_target  = FALSE;
    fs.avoid_target      = nullptr;
    fs.move_target = HierPos2{};
    fs.attack_target = nullptr;
    fs.flight = ShipFlight{};   // velocity, spin and thruster telemetry all start at rest
    if (m_count > m_spawned) m_spawned = m_count;
    return fs;
}
ShipFlight* Fleet::flight_for_ship(const Ship* ship) {
    for (i32 i = 0; i < m_count; ++i) {
        if (&m_ships[i].ship == ship) return &m_ships[i].flight;
    }
    return nullptr;
}
// =====================================================================================
void Fleet::clear_selection() {
    for (i32 i = 0; i < m_count; ++i) m_ships[i].selected = FALSE;
}
i32 Fleet::select_in_box(HierPos2 p0, HierPos2 p1) {
    // Work in a frame relative to p0 so the comparison stays precise far from the origin.
    Vec2 c1 = hierpos_diff(&p1, &p0);
    f32 min_x = fminf(0.0f, c1.x), max_x = fmaxf(0.0f, c1.x);
    f32 min_y = fminf(0.0f, c1.y), max_y = fmaxf(0.0f, c1.y);
    i32 n = 0;
    for (i32 i = 0; i < m_count; ++i) {
        Vec2 o = hierpos_diff(&m_ships[i].ship.origin, &p0);
        b8 inside = (o.x >= min_x && o.x <= max_x && o.y >= min_y && o.y <= max_y);
        m_ships[i].selected = inside;
        if (inside) ++n;
    }
    return n;
}
b8 Fleet::select_at_point(HierPos2 world) {
    i32 hit = -1;
    f32 best = 0.0f;
    for (i32 i = 0; i < m_count; ++i) {
        f32 r = ship_bounding_radius(&m_ships[i].ship);
        f32 d = vec2_length(hierpos_diff(&world, &m_ships[i].ship.origin));
        if (d <= r && (hit < 0 || d < best)) { hit = i; best = d; }
    }
    for (i32 i = 0; i < m_count; ++i) m_ships[i].selected = (i == hit);
    return (hit >= 0) ? TRUE : FALSE;
}
i32 Fleet::selected_count() const {
    i32 n = 0;
    for (i32 i = 0; i < m_count; ++i) if (m_ships[i].selected) ++n;
    return n;
}
i32 Fleet::first_selected() const {
    for (i32 i = 0; i < m_count; ++i) if (m_ships[i].selected) return i;
    return -1;
}
// =====================================================================================
void Fleet::order_move(HierPos2 target) {
    // Collect the selected ships.
    i32 sel[FLEET_MAX_SHIPS];
    i32 n = 0;
    f32 max_r = 0.0f;
    // Accumulate the centroid in a frame relative to the target so it stays precise far from
    // the galaxy origin.
    Vec2 centroid_rel = Vec2{ 0.0f, 0.0f };
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        sel[n++] = i;
        centroid_rel = vec2_add(centroid_rel, hierpos_diff(&m_ships[i].ship.origin, &target));
        f32 r = ship_bounding_radius(&m_ships[i].ship);
        if (r > max_r) max_r = r;
    }
    if (n == 0) return;
    centroid_rel = vec2_scale(centroid_rel, 1.0f / (f32)n);
    if (n == 1) {
        // A move supersedes escort/avoid exactly as they supersede it (the position orders are
        // mutually exclusive; leaving one set would mask it behind update_move until arrival,
        // then silently resume it -- and the roster would label the ship with the stale order).
        m_ships[sel[0]].clear_escort_target();
        m_ships[sel[0]].clear_avoid_target();
        m_ships[sel[0]].has_move_target = TRUE;
        m_ships[sel[0]].move_target = target;
        return;
    }
    // Forward axis = centroid -> target = -centroid_rel; right axis = perpendicular.
    Vec2 forward = vec2_scale(centroid_rel, -1.0f);
    f32 flen = vec2_length(forward);
    forward = (flen > 0.0001f) ? vec2_scale(forward, 1.0f / flen) : Vec2{ 0.0f, 1.0f };
    Vec2 right = Vec2{ forward.y, -forward.x };
    f32 spacing = max_r * FORMATION_SPACING_MUL;
    if (spacing < FORMATION_MIN_SPACING) spacing = FORMATION_MIN_SPACING;
    // Centered grid: cols = ceil(sqrt(n)), rows = ceil(n/cols).
    i32 cols = (i32)ceilf(sqrtf((f32)n));
    if (cols < 1) cols = 1;
    i32 rows = (n + cols - 1) / cols;
    for (i32 k = 0; k < n; ++k) {
        i32 col = k % cols;
        i32 row = k / cols;
        f32 ox = ((f32)col - (cols - 1) * 0.5f) * spacing;
        f32 oy = ((f32)row - (rows - 1) * 0.5f) * spacing;
        Vec2 off = vec2_add(vec2_scale(right, ox), vec2_scale(forward, oy));
        m_ships[sel[k]].clear_escort_target();  // position orders are mutually exclusive
        m_ships[sel[k]].clear_avoid_target();
        m_ships[sel[k]].has_move_target = TRUE;
        m_ships[sel[k]].move_target = hierpos_add_vec2(&target, off);
        // Note: attack_target is intentionally preserved so move+attack can coexist.
    }
}
void Fleet::order_attack(Ship* target) {
    if (!target) return;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        m_ships[i].has_attack_target = TRUE;
        m_ships[i].attack_target = target;
        m_ships[i].auto_acquired = FALSE;   // a designation outranks the ROE pass
    }
}
// =====================================================================================
b8 Fleet::selected_min_jump(i32* out_center_idx, f32* out_radius) const {
    i32 center = -1;
    f32 min_r = 0.0f;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected || !m_ships[i].jump_capable) continue;
        if (center < 0 || m_ships[i].jump_radius < min_r) {
            center = i;
            min_r = m_ships[i].jump_radius;
        }
    }
    if (center < 0) return FALSE;
    if (out_center_idx) *out_center_idx = center;
    if (out_radius)     *out_radius = min_r;
    return TRUE;
}
void Fleet::jump_selected(i32 center_idx, HierPos2 point) {
    if (center_idx < 0 || center_idx >= m_count) return;
    HierPos2 center_origin = m_ships[center_idx].ship.origin;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected || !m_ships[i].jump_capable) continue;
        // Preserve each ship's offset from the center ship so the formation lands intact.
        Vec2 offset = hierpos_diff(&m_ships[i].ship.origin, &center_origin);
        m_ships[i].ship.origin = hierpos_add_vec2(&point, offset);
        m_ships[i].clear_move_target();
        m_ships[i].clear_attack_target();
        // Arrive at rest: no drift, and no thruster flame carried through the jump.
        m_ships[i].flight = ShipFlight{};
    }
}
void Fleet::set_all_jump_radius(f32 radius) {
    for (i32 i = 0; i < m_count; ++i) m_ships[i].jump_radius = radius;
}
void Fleet::clear_order(i32 idx) {
    if (idx < 0 || idx >= m_count) return;
    m_ships[idx].clear_move_target();
    m_ships[idx].clear_attack_target();
    m_ships[idx].clear_escort_target();
    m_ships[idx].clear_avoid_target();
}
void Fleet::set_selected_stance(u8 stance) {
    if (stance > FLEET_STANCE_HOLD_FIRE) return;   // unknown value: leave the fleet alone
    for (i32 i = 0; i < m_count; ++i)
        if (m_ships[i].selected) m_ships[i].stance = stance;
}
void Fleet::set_selected_roe(u8 roe) {
    if (roe > ROE_HOLD) return;   // unknown value: leave the fleet alone
    for (i32 i = 0; i < m_count; ++i)
        if (m_ships[i].selected) m_ships[i].roe = roe;
}
void Fleet::set_selected_flak_doctrine(u8 doctrine) {
    if (doctrine > FLAK_MANUAL) return;
    for (i32 i = 0; i < m_count; ++i)
        if (m_ships[i].selected) m_ships[i].flak_doctrine = doctrine;
}
void Fleet::set_selected_missile_policy(u8 policy) {
    if (policy > MISSILE_HOLD) return;
    for (i32 i = 0; i < m_count; ++i)
        if (m_ships[i].selected) m_ships[i].missile_policy = policy;
}
void Fleet::set_selected_cap_floor(f32 floor) {
    floor = clampf(floor, 0.0f, 0.9f);
    for (i32 i = 0; i < m_count; ++i)
        if (m_ships[i].selected) m_ships[i].cap_fire_floor = floor;
}
// ---- Return-fire aggression memory ----------------------------------------------------
void Fleet::note_aggression(i16 faction_id) {
    // Refresh an existing live entry first.
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i)
        if (m_aggressor_timer[i] > 0.0f && m_aggressor_id[i] == faction_id) {
            m_aggressor_timer[i] = ROE_AGGRESSION_S;
            return;
        }
    // New entry: take the first free slot, else evict the one closest to expiry.
    i32 slot = 0;
    f32 lowest = m_aggressor_timer[0];
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i) {
        if (m_aggressor_timer[i] <= 0.0f) { slot = i; break; }
        if (m_aggressor_timer[i] < lowest) { lowest = m_aggressor_timer[i]; slot = i; }
    }
    m_aggressor_id[slot]    = faction_id;
    m_aggressor_timer[slot] = ROE_AGGRESSION_S;
}
b8 Fleet::is_aggressor(i16 faction_id) const {
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i)
        if (m_aggressor_timer[i] > 0.0f && m_aggressor_id[i] == faction_id) return TRUE;
    return FALSE;
}
void Fleet::order_escort(Ship* target) {
    if (!target) return;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        if (&m_ships[i].ship == target) continue;   // a ship cannot escort itself
        // Escort supersedes a move order: both drive position, and leaving a stale destination
        // set would have update_move fighting update_escort every frame.
        m_ships[i].clear_move_target();
        m_ships[i].clear_avoid_target();
        m_ships[i].has_escort_target = TRUE;
        m_ships[i].escort_target     = target;
    }
}
void Fleet::order_avoid(Ship* target) {
    if (!target) return;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        m_ships[i].clear_move_target();
        m_ships[i].clear_escort_target();
        m_ships[i].clear_attack_target();   // running from something you are shooting is incoherent
        m_ships[i].has_avoid_target = TRUE;
        m_ships[i].avoid_target     = target;
    }
}
void Fleet::order_rally() {
    if (m_count <= 0) return;
    order_escort(&m_ships[0].ship);
}
// =====================================================================================
// One ship's response to a too-close neighbor (`to_other` points at it). A ship with a live
// order gets an accel-scaled ADDITIVE nudge away -- its controller's closed loop absorbs the
// bias without losing the order. An orderless ship is DRIVEN toward the repulsion ramp instead:
// the ramp fades to zero at the ring edge, so the ship glides to rest just outside rather than
// coasting away forever (nothing else ever damps an idle ship's velocity). The piloted ship is
// never adjusted -- the player's stick is authoritative, its partner does all the yielding, the
// same immovability convention resolve_ship_collision applies to the enemy hull.
static void separation_adjust(FleetShip* fs, Vec2 to_other, f32 dist, f32 min_sep,
                              b8 piloted, f32 dt) {
    if (piloted || dist < 0.001f) return;   // coincident pair has no direction; collision owns it
    const ShipMotion& m = fs->ship.motion;
    b8 regulated = fs->has_move_target || fs->has_escort_target ||
                   fs->has_avoid_target || fs->has_attack_target;
    if (regulated) {
        f32  t    = 1.0f - dist / min_sep;  // penetration: 0 at the ring -> 1 at contact
        Vec2 away = vec2_scale(to_other, -1.0f / dist);
        fs->flight.velocity = vec2_add(fs->flight.velocity, vec2_scale(away, m.accel * t * dt));
        // Telemetry: the nudge is t x the accel cap, along `away` -- record it in local frame.
        fs->flight.thrust_cmd = vec2_add(fs->flight.thrust_cmd,
            vec2_scale(vec2_rotate(away, -fs->ship.angle), t));
    } else {
        Vec2 desired = steering::separation(to_other, dist, min_sep, ship_speed_cap(&fs->ship));
        Vec2 delta   = vec2_sub(desired, fs->flight.velocity);
        f32  step    = m.accel * dt;
        f32  dl      = vec2_length(delta);
        if (dl > step) delta = vec2_scale(delta, step / dl);
        fs->flight.velocity = vec2_add(fs->flight.velocity, delta);
        // Telemetry: applied velocity change as a fraction of this tick's accel budget.
        if (step > 0.0f)
            fs->flight.thrust_cmd = vec2_add(fs->flight.thrust_cmd,
                vec2_scale(vec2_rotate(delta, -fs->ship.angle), 1.0f / step));
    }
}
void Fleet::update_autopilot(game_state* s, f32 dt, i32 auto_skip) {
    // Return-fire grievances decay here: one owner for the timers, ticked once per frame.
    for (i32 i = 0; i < AGGRESSOR_MAX; ++i)
        if (m_aggressor_timer[i] > 0.0f) m_aggressor_timer[i] -= dt;
    for (i32 i = 0; i < m_count; ++i) {
        if (i == auto_skip) continue;
        // ROE self-acquisition first, so a fight it picks this tick is fought this tick.
        m_ships[i].update_engagement(s);
        if (m_ships[i].has_attack_target) m_ships[i].update_attack(s, dt);
        if (m_ships[i].has_move_target)     m_ships[i].update_move(dt);
        // Position orders are mutually exclusive by construction (each order_* clears the
        // others), so at most one of these three ever drives a ship in a frame. Avoid is
        // checked last because it is the one that ends itself: it clears its own order once
        // the ship is outside the threat's reach.
        else if (m_ships[i].has_escort_target) m_ships[i].update_escort(dt);
        else if (m_ships[i].has_avoid_target)  m_ships[i].update_avoid(dt);
    }
    // ---- Minimum separation post-pass: fleet ships never crowd inside each other's ring ----
    // Runs AFTER the order updates so it corrects the frame's final ordered velocities, and
    // mutates velocities only -- simulate_all owns integration, the same contract as every
    // other control path. Pairwise over at most FLEET_MAX_SHIPS ships, so the N^2 is trivial.
    for (i32 i = 0; i < m_count; ++i) {
        for (i32 j = i + 1; j < m_count; ++j) {
            Vec2 to_j = hierpos_diff(&m_ships[j].ship.origin, &m_ships[i].ship.origin);
            f32  dist = vec2_length(to_j);
            f32  min_sep = (ship_bounding_radius(&m_ships[i].ship) +
                            ship_bounding_radius(&m_ships[j].ship)) * FLEET_MIN_SEPARATION_MUL;
            if (dist >= min_sep) continue;
            separation_adjust(&m_ships[i], to_j,                    dist, min_sep, i == auto_skip, dt);
            separation_adjust(&m_ships[j], vec2_scale(to_j, -1.0f), dist, min_sep, j == auto_skip, dt);
        }
    }
}
void Fleet::simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx) {
    for (i32 i = 0; i < m_count; ++i) {
        b8 tc = (i == piloted_idx) ? piloted_turn_commanded : FALSE;
        m_ships[i].simulate(dt, tc);
    }
}
