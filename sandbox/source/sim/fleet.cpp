#include "sim/fleet.h"
#include "game.h"
#include "sim/weapon.h"
#include <math/math_utils.h>
using namespace bs_math;
// =====================================================================================
// Autopilot tuning (mirrors the legacy RTS order constants).
// =====================================================================================
static constexpr f32 RTS_MOVE_ARRIVE_DIST = 60.0f;   // units
static constexpr f32 RTS_MOVE_STOP_SPEED  = 15.0f;   // units/s
static constexpr f32 RTS_MOVE_FACE_ANGLE  = 0.3f;    // rad; must face target within this to thrust
static constexpr f32 RTS_ATTACK_RANGE      = 10000.0f; // desired weapon engagement range
static constexpr f32 RTS_ATTACK_FACE_ANGLE = 0.25f;  // rad: must face target within this to fire
static constexpr f32 RTS_ATTACK_STOP_DIST  = 120.0f; // keep this distance from target
static constexpr f32 RTS_ATTACK_MIN_RANGE  = 150.0f; // do not fire closer than this
// Strafing movement controller: how aggressively the ship corrects its velocity toward the
// desired arrival velocity. Projected onto local thrusters (forward/back + lateral).
static constexpr f32 RTS_STRAFE_GAIN = 3.0f;
// Formation layout.
static constexpr f32 FORMATION_SPACING_MUL = 2.5f;   // x max bounding radius
static constexpr f32 FORMATION_MIN_SPACING = 400.0f; // world units floor
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
// FleetShip
// =====================================================================================
void FleetShip::simulate(f32 dt, b8 turn_commanded) {
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    // Auto-stabilize spin toward zero whenever no turn is commanded. Bleeds at the turn-accel
    // rate, mirroring the A/D ramp, so a released turn coasts down the same way it spun up.
    if (!turn_commanded) {
        f32 drop = SHIP_TURN_ACCEL * dt;
        if (fl->angular_velocity > 0.0f)      fl->angular_velocity = (fl->angular_velocity > drop) ? fl->angular_velocity - drop : 0.0f;
        else if (fl->angular_velocity < 0.0f) fl->angular_velocity = (fl->angular_velocity < -drop) ? fl->angular_velocity + drop : 0.0f;
    }
    // Clamp linear + angular speed to their caps.
    f32 spd = vec2_length(fl->velocity);
    if (spd > SHIP_MAX_SPEED) fl->velocity = vec2_scale(fl->velocity, SHIP_MAX_SPEED / spd);
    fl->angular_velocity = clampf(fl->angular_velocity, -SHIP_MAX_TURN, SHIP_MAX_TURN);
    // Integrate the rigid-body pose.
    sh->origin = hierpos_add_vec2(&sh->origin, vec2_scale(fl->velocity, dt));
    sh->angle += fl->angular_velocity * dt;
}
// =====================================================================================
void FleetShip::update_move(f32 dt) {
    if (!has_move_target) return;
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    Vec2 to_target = hierpos_diff(&move_target, &sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Arrival: close enough and nearly stopped.
    if (dist < RTS_MOVE_ARRIVE_DIST && speed < RTS_MOVE_STOP_SPEED) {
        clear_move_target();
        fl->velocity = Vec2{ 0.0f, 0.0f };
        return;
    }
    // Desired velocity: toward the target, capped by the distance we can still brake from.
    Vec2 dir = (dist > 0.0001f) ? vec2_scale(to_target, 1.0f / dist) : Vec2{ 0.0f, 0.0f };
    f32 desired_speed = (dist > 0.0001f) ? fminf(SHIP_MAX_SPEED, sqrtf(2.0f * SHIP_DECEL * dist)) : 0.0f;
    Vec2 desired_vel = vec2_scale(dir, desired_speed);
    Vec2 vel_err = vec2_sub(desired_vel, fl->velocity);
    // Desired acceleration to correct velocity error, then project onto local thrusters.
    Vec2 desired_acc = vec2_scale(vel_err, RTS_STRAFE_GAIN);
    Vec2 fwd   = vec2_rotate(Vec2{ 0.0f, 1.0f }, sh->angle);
    Vec2 right = vec2_rotate(Vec2{ 1.0f, 0.0f }, sh->angle);
    f32 fwd_acc  = vec2_dot(desired_acc, fwd);
    f32 right_acc = vec2_dot(desired_acc, right);
    if (fwd_acc > 0.0f) fwd_acc = fminf(fwd_acc, SHIP_ACCEL);
    else                fwd_acc = fmaxf(fwd_acc, -SHIP_DECEL);
    right_acc = clampf(right_acc, -SHIP_ACCEL, SHIP_ACCEL);
    Vec2 acc = vec2_add(vec2_scale(fwd, fwd_acc), vec2_scale(right, right_acc));
    fl->velocity = vec2_add(fl->velocity, vec2_scale(acc, dt));
    f32 cur_speed = vec2_length(fl->velocity);
    if (cur_speed > SHIP_MAX_SPEED) fl->velocity = vec2_scale(fl->velocity, SHIP_MAX_SPEED / cur_speed);
}
// =====================================================================================
void FleetShip::update_attack(game_state* s, f32 dt) {
    if (!has_attack_target) return;
    if (!attack_target) { clear_attack_target(); return; }
    CombatEntity* ce = find_combat_entity_for_ship(s, attack_target);
    if (!ce || !ce->active || !ce->ship) { clear_attack_target(); return; }
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    if (ce->faction == sh->faction) { clear_attack_target(); return; } // no longer hostile
    Ship* target   = ce->ship;
    f32 ship_r     = ship_bounding_radius(sh);
    f32 target_r   = ship_bounding_radius(target);
    Vec2 to_target = hierpos_diff(&target->origin, &sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Rotate nose toward the target. Heading convention: ship angle 0 => nose +Y.
    f32 desired_angle = atan2f(-to_target.x, to_target.y);
    f32 angle_diff = normalize_angle(desired_angle - sh->angle);
    f32 max_rot = SHIP_MAX_TURN * dt;
    sh->angle += clampf(angle_diff, -max_rot, max_rot);
    fl->angular_velocity = 0.0f;
    // Only approach the target if we have no separate move order. When both orders are set,
    // movement is handled by update_move and we just track/fire here.
    if (!has_move_target) {
        if (fabsf(angle_diff) < RTS_MOVE_FACE_ANGLE) {
            Vec2 heading = vec2_rotate(Vec2{ 0.0f, 1.0f }, sh->angle);
            f32 brake_dist = (speed > 0.0f) ? (speed * speed) / (2.0f * SHIP_DECEL) : 0.0f;
            f32 desired_dist = RTS_ATTACK_RANGE;
            f32 min_standoff = RTS_ATTACK_STOP_DIST + target_r + ship_r;
            if (desired_dist < min_standoff) desired_dist = min_standoff;
            // Only close distance when too far; never actively back away from the target.
            if (dist > desired_dist + brake_dist) {
                fl->velocity = vec2_add(fl->velocity, vec2_scale(heading, SHIP_ACCEL * dt));
            } else if (speed > 0.0f) {
                fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * dt / speed));
            }
        } else if (speed > 0.0f) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * 0.5f * dt / speed));
        }
    }
    // Fire when facing the target, inside max range, and outside minimum range.
    if (fabsf(angle_diff) < RTS_ATTACK_FACE_ANGLE && dist >= RTS_ATTACK_MIN_RANGE && dist <= RTS_ATTACK_RANGE * 1.5f) {
        if (sh->active_weapon_idx >= 0 && sh->active_weapon_idx < sh->weapon_count) {
            Weapon* w = sh->weapons[sh->active_weapon_idx];
            if (w) {
                HierPos2 fire_origin = ship_local_to_world(sh, sh->weapon_fire_offset_local);
                Vec2 aim_dir = to_target;
                Vec2 target_vel = ce->velocity;
                if ((target_vel.x != 0.0f || target_vel.y != 0.0f) && dist > 0.0001f) {
                    f32 proj_speed = w->projectile_speed();
                    if (proj_speed > 0.0001f) {
                        f32 lead_time = dist / proj_speed;
                        HierPos2 lead_pos = hierpos_add_vec2(&target->origin, vec2_scale(target_vel, lead_time));
                        aim_dir = hierpos_diff(&lead_pos, &fire_origin);
                    }
                }
                w->fire(fire_origin, aim_dir, fl->velocity, &s->projectiles);
            }
        }
    }
    f32 cur_speed = vec2_length(fl->velocity);
    if (cur_speed > SHIP_MAX_SPEED) fl->velocity = vec2_scale(fl->velocity, SHIP_MAX_SPEED / cur_speed);
}
// =====================================================================================
void FleetShip::clear_move_target() {
    has_move_target = FALSE;
    move_target = HierPos2{};
}
void FleetShip::clear_attack_target() {
    has_attack_target = FALSE;
    attack_target = nullptr;
}
// =====================================================================================
// Fleet
// =====================================================================================
Fleet::Fleet() : m_count(0), m_spawned(0), m_piloted_idx(0) {}
void Fleet::init() { m_count = 0; m_spawned = 0; m_piloted_idx = 0; }
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
    fs.move_target = HierPos2{};
    fs.attack_target = nullptr;
    fs.flight.velocity = Vec2{ 0.0f, 0.0f };
    fs.flight.angular_velocity = 0.0f;
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
        m_ships[i].flight.velocity = Vec2{ 0.0f, 0.0f };
    }
}
void Fleet::set_all_jump_radius(f32 radius) {
    for (i32 i = 0; i < m_count; ++i) m_ships[i].jump_radius = radius;
}
void Fleet::clear_order(i32 idx) {
    if (idx < 0 || idx >= m_count) return;
    m_ships[idx].clear_move_target();
    m_ships[idx].clear_attack_target();
}
// =====================================================================================
void Fleet::update_autopilot(game_state* s, f32 dt, i32 piloted_idx) {
    for (i32 i = 0; i < m_count; ++i) {
        if (i == piloted_idx) continue;
        if (m_ships[i].has_attack_target) m_ships[i].update_attack(s, dt);
        if (m_ships[i].has_move_target)     m_ships[i].update_move(dt);
    }
}
void Fleet::simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx) {
    for (i32 i = 0; i < m_count; ++i) {
        b8 tc = (i == piloted_idx) ? piloted_turn_commanded : FALSE;
        m_ships[i].simulate(dt, tc);
    }
}
