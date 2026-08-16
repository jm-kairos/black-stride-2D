#include "sim/flak_screen.h"
#include "game.h"
#include "sim/weapon.h"
#include <math/math_utils.h>
#include <math.h>

using namespace bs_math;

// Wrap an angle into (-pi, pi].
static f32 flak_wrap_pi(f32 a) {
    while (a >  BS_PI) a -= 2.0f * BS_PI;
    while (a < -BS_PI) a += 2.0f * BS_PI;
    return a;
}

// Closed-form intercept: smallest t > 0 with |rel_pos + rel_vel*t| = shell_speed*t -- the
// time at which a shell fired NOW meets the ordnance; -1 when no such time exists (the
// ordnance outruns every firing solution from here). This replaces the two-pass fixed-point
// iterate (aim = pos + vel*tof) the broadside uses: that iterate is a contraction only
// while the target is SLOWER than the shell, which holds for hulls but never for flak -- a
// 0.6x shell (6k u/s on the autocannon) engaging a full-speed missile (10k u/s) sits at
// ratio 1.67, where the iterate DIVERGES. For a head-on missile two passes land pointing
// 180 degrees AWAY from the threat, can_aim rejected the reversed aim, and the screen
// silently sat out exactly the attacks it exists to stop.
static f32 flak_intercept_time(Vec2 rel_pos, Vec2 rel_vel, f32 shell_speed) {
    f32 a = vec2_dot(rel_vel, rel_vel) - shell_speed * shell_speed;
    f32 b = 2.0f * vec2_dot(rel_pos, rel_vel);
    f32 c = vec2_dot(rel_pos, rel_pos);
    if (fabsf(a) < 1.0e-3f)                        // equal speeds: quadratic degenerates
        return (b < -1.0e-6f) ? (-c / b) : -1.0f;  // catchable only while closing
    f32 disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return -1.0f;
    f32 sq = sqrtf(disc);
    f32 t0 = (-b - sq) / (2.0f * a);
    f32 t1 = (-b + sq) / (2.0f * a);
    f32 t  = 1.0e30f;
    if (t0 > 0.0f) t = t0;
    if (t1 > 0.0f && t1 < t) t = t1;
    return (t < 1.0e30f) ? t : -1.0f;
}

// Only ordnance this close to landing is worth a shell: impact time to the most-threatened
// fleet hull, seconds. Beyond it a threat is watched, not shot -- flak shells are cheap but
// the capacitor is not.
static constexpr f32 FLAK_HORIZON_S = 8.0f;
// Non-missile ordnance is engaged only when no missile threatens (score offset, same
// pattern as PD_PRI_GUIDED_FIRST): a shell is 1 damage on armour, a missile is a hit that
// PD may not stop alone.
static constexpr f32 FLAK_SHELL_PENALTY = 1.0e3f;
// Side discipline: a mount only engages threats within this half-angle of its authored
// FACING -- deliberately tighter than the raw traverse arc. The 210-degree arcs reach the
// far side of the ship through the stern, so without this a starboard cannon legally fired
// across its own hull at port-side missiles: reachable, but the wrong gun for the job (hull
// width added to the intercept, a huge traverse paid first, and its own side left open).
// Each mount screens ITS side; a threat outside every fit window is the PD layer's job.
static constexpr f32 FLAK_FIT_HALF = 1.75f;   // rad (~100 degrees)
// Among fitting threats, prefer the one nearest the mount's axis: one radian off-axis
// scores like two extra seconds of impact time, so port takes port and starboard takes
// starboard even when both technically fit.
static constexpr f32 FLAK_BEARING_WEIGHT = 2.0f;

void flak_screen_update(game_state* s, f32 dt) {
    if (!s) return;
    Fleet& fleet = s->fleet_state.fleet;

    // The ATTACHED pilot owns their own mounts' aim (cursor traverse); detached, every hull
    // screens -- the same auto_skip convention update_autopilot uses.
    b8  manually_piloting = !s->camera_state.free_camera_active;
    i32 skip = manually_piloting ? fleet.piloted_index() : -1;

    for (i32 i = 0; i < fleet.count(); ++i) {
        FleetShip& fs = fleet.at(i);
        Ship&      sh = fs.ship;
        // Claims are strictly per-tick: clear before anything can skip this hull, so a
        // mount the screen released always returns to offense duty next tick.
        for (i32 h = 0; h < sh.hardpoint_count; ++h) sh.flak_screen_claimed[h] = FALSE;
        if (dt <= 0.0f) continue;
        if (fs.flak_doctrine != FLAK_AUTO) continue;
        b8 cursor_owned_hull = (i == skip);   // attached pilot: cursor owns the FIRE SELECTION
        // Defensive capacitor floor: the same reserve PD honours. The screen stops ACQUIRING
        // below it (shells already in flight fly on), so flak throttles itself before it
        // starves the last-ditch laser.
        if (sh.cap_current < sh.point_defense.reserve_floor * sh.cap_max) continue;

        ShipFlight* fl    = fleet.flight_for_ship(&sh);
        Vec2        selfv = fl ? fl->velocity : Vec2{ 0.0f, 0.0f };

        for (i32 hpi = 0; hpi < sh.hardpoint_count; ++hpi) {
            Weapon* w = sh.mounts[hpi];
            if (!w || w->wkind != WEAPON_KIND_BALLISTIC) continue;
            BallisticWeapon* bw = static_cast<BallisticWeapon*>(w);
            // EVERY ballistic mount screens under FLAK_AUTO -- an AP cannon fires flak
            // rounds per-shot when ordnance threatens (defense preempts offense via the
            // claim below), while a MODE_FLAK mount is a DEDICATED screen (excluded from
            // hull duty entirely, watch pre-aim between volleys). The one carve-out: the
            // attached pilot's fire SELECTION belongs to the cursor, never the screen.
            b8 dedicated = (bw->fire_mode == MODE_FLAK);
            if (cursor_owned_hull && ship_hardpoint_in_selection(&sh, hpi)) continue;
            f32 reach = weapon_flak_reach(w);
            if (reach <= 0.0f) continue;
            const f32 shell_speed = w->projectile_speed() * FLAK_SPEED_MUL;
            // This mount's natural axis: its authored rest facing, in world heading terms.
            // Side discipline keys off it, not off the (much wider) traverse arc.
            const f32 mount_axis = sh.angle + sh.hardpoints[hpi].facing;

            // Best threat for THIS mount: hostile ordnance inside the shell's real flight
            // envelope from this hull, closing on ANY fleet member inside the horizon.
            // Missiles strictly outrank shells, then soonest impact. Flak never chases flak
            // (bursts are terminal, not threats).
            i32 best       = -1;
            f32 best_score = 1.0e30f;
            for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
                const Projectile& p = s->projectiles.pool[pi];
                if (!p.active) continue;
                if (p.faction_id == sh.faction_id) continue;
                if (p.kind == PROJ_FLAK) continue;
                Vec2 to_p = hierpos_diff(&p.position, &sh.origin, BS_HIERPOS_CELL_SIZE);
                if (vec2_length(to_p) > reach) continue;
                // Side discipline: only threats on THIS mount's side of the hull. The raw
                // arc reaches the far side through the stern; the fit window does not.
                f32 bearing  = atan2f(-to_p.x, to_p.y);   // heading convention: nose = +Y
                f32 off_axis = fabsf(flak_wrap_pi(bearing - mount_axis));
                if (off_axis > FLAK_FIT_HALF) continue;
                // Physical catch gate: the shell must be able to MEET this ordnance inside
                // its lifetime. Raw distance is not enough -- a crossing missile at 1.67x
                // shell speed can sit well inside reach yet be uncatchable, and locking one
                // wastes the mount while a killable threat goes unengaged.
                f32 t_int = flak_intercept_time(to_p, vec2_sub(p.velocity, selfv), shell_speed);
                if (t_int < 0.0f || t_int > FLAK_LIFETIME_S) continue;
                // Impact time to the most-threatened fleet hull: distance over closing
                // speed toward it; receding/tangential ordnance never scores.
                f32 tti = 1.0e30f;
                for (i32 j = 0; j < fleet.count(); ++j) {
                    Vec2 to_hull = hierpos_diff(&fleet.at(j).ship.origin, &p.position,
                                                BS_HIERPOS_CELL_SIZE);
                    f32 hd = vec2_length(to_hull);
                    if (hd < 1.0e-3f) { tti = 0.0f; break; }
                    f32 closing = (p.velocity.x * to_hull.x + p.velocity.y * to_hull.y) / hd;
                    if (closing > 1.0f) {
                        f32 t = hd / closing;
                        if (t < tti) tti = t;
                    }
                }
                if (tti > FLAK_HORIZON_S) continue;
                f32 score = tti + off_axis * FLAK_BEARING_WEIGHT
                          + ((p.kind != PROJ_MISSILE) ? FLAK_SHELL_PENALTY : 0.0f);
                if (score < best_score) { best_score = score; best = pi; }
            }
            if (best < 0) {
                // No ordnance inbound. An AP mount goes back to offense untouched; a
                // DEDICATED flak mount WATCHES the likely source instead of drooping to
                // the rest stop. Pre-aiming at the nearest hostile ship inside twice the
                // envelope means the first shell of the next volley is met with a barrel
                // already on axis -- an ordnance transit across the envelope lasts under a
                // second, which is less than a full traverse from rest.
                if (!dedicated) continue;
                f32   watch_d    = reach * 2.0f;
                Vec2  watch_dir  = Vec2{ 0.0f, 0.0f };
                b8    have_watch = FALSE;
                for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
                    const CombatEntity* ce = &s->combat_entities[ci];
                    if (!ce->active || !ce->ship) continue;
                    if (ce->faction_id == sh.faction_id) continue;
                    Vec2 to = hierpos_diff(&ce->position, &sh.origin, BS_HIERPOS_CELL_SIZE);
                    // Watch only ships on THIS mount's side -- the same fit window the
                    // engagement uses, so the barrel never parks somewhere it will not shoot.
                    f32 wb = atan2f(-to.x, to.y);
                    if (fabsf(flak_wrap_pi(wb - mount_axis)) > FLAK_FIT_HALF) continue;
                    f32 d = vec2_length(to);
                    if (d < watch_d) { watch_d = d; watch_dir = to; have_watch = TRUE; }
                }
                if (have_watch && ship_hardpoint_can_aim(&sh, hpi, watch_dir))
                    ship_turret_aim_at(&sh, hpi, watch_dir);
                continue;
            }

            // Exact intercept lead at the FLAK shell's own (reduced) speed. The shell
            // inherits this hull's velocity, so the solve runs on relative velocity. NOT
            // the broadside's two-pass iterate: that diverges against ordnance faster than
            // the shell (see flak_intercept_time), which flak targets always are.
            const Projectile& p = s->projectiles.pool[best];
            HierPos2 fire_origin = ship_hardpoint_fire_origin(&sh, hpi);
            Vec2 to_p = hierpos_diff(&p.position, &fire_origin, BS_HIERPOS_CELL_SIZE);
            Vec2 rvel = vec2_sub(p.velocity, selfv);
            // Re-solve from the MOUNT (the gate above solved from the hull origin); the
            // offset can shave a marginal solution, so a failure here just releases the mount.
            f32  t_hit = flak_intercept_time(to_p, rvel, shell_speed);
            if (t_hit < 0.0f) continue;
            Vec2 aim = vec2_add(to_p, vec2_scale(rvel, t_hit));
            if (!ship_hardpoint_can_aim(&sh, hpi, aim)) continue;

            // Defense preempts offense from here: the claim makes update_attack yield this
            // mount next tick (no engaging-mount track, no broadside), so the barrel is not
            // dragged back onto the hull target mid-intercept.
            sh.flak_screen_claimed[hpi] = TRUE;
            ship_turret_aim_at(&sh, hpi, aim);

            bw->owner_faction_id = sh.faction_id;
            // One shared validator (arc / slew convergence / cooldown / reach / power) and
            // one shared spawner -- the exact path the trigger loop and broadside walk. An
            // AP mount fires a FLAK round for this shot only (the mode is weapon state read
            // by spawn_shot, so it is swapped around the spawn and restored).
            if (ship_weapon_fire_state(&sh, hpi, aim, vec2_length(to_p)) == WEAPON_FIRE_READY &&
                ship_try_spend_cap(&sh, w->cap_cost())) {
                u8 saved_mode = bw->fire_mode;
                bw->fire_mode = MODE_FLAK;
                ship_hardpoint_fire(&sh, hpi, aim, selfv, &s->projectiles);
                bw->fire_mode = saved_mode;
            }
        }
    }
}
