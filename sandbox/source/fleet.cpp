#include "fleet.h"
#include "game.h"
#include "weapon.h"
#include <math/math_utils.h>
using namespace bs_math;
// =====================================================================================
// Autopilot tuning (mirrors the legacy RTS order constants).
// =====================================================================================
static constexpr f32 RTS_MOVE_ARRIVE_DIST = 60.0f;   // units
static constexpr f32 RTS_MOVE_STOP_SPEED  = 15.0f;   // units/s
static constexpr f32 RTS_MOVE_FACE_ANGLE  = 0.3f;    // rad; must face target within this to thrust
static constexpr f32 RTS_ATTACK_RANGE      = 600.0f; // desired weapon engagement range
static constexpr f32 RTS_ATTACK_FACE_ANGLE = 0.25f;  // rad: must face target within this to fire
static constexpr f32 RTS_ATTACK_STOP_DIST  = 120.0f; // keep this distance from target
static constexpr f32 RTS_ATTACK_MIN_RANGE  = 150.0f; // do not fire closer than this
// Formation layout.
static constexpr f32 FORMATION_SPACING_MUL = 2.5f;   // x max bounding radius
static constexpr f32 FORMATION_MIN_SPACING = 120.0f; // world units floor
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
    sh->origin = vec2_add(sh->origin, vec2_scale(fl->velocity, dt));
    sh->angle += fl->angular_velocity * dt;
}
// =====================================================================================
void FleetShip::update_move(f32 dt) {
    if (order != FleetOrder::Move) return;
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    Vec2 to_target = vec2_sub(move_target, sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Arrival: close enough and nearly stopped.
    if (dist < RTS_MOVE_ARRIVE_DIST && speed < RTS_MOVE_STOP_SPEED) {
        order = FleetOrder::None;
        fl->velocity = Vec2{ 0.0f, 0.0f };
        return;
    }
    // Rotate toward the target. Heading convention: ship angle 0 => nose +X for atan2.
    f32 desired_angle = atan2f(to_target.y, to_target.x);
    f32 angle_diff = normalize_angle(desired_angle - sh->angle);
    f32 max_rot = SHIP_MAX_TURN * dt;
    sh->angle += clampf(angle_diff, -max_rot, max_rot);
    fl->angular_velocity = 0.0f; // autopilot sets the pose directly; avoid extra spin in simulate
    if (fabsf(angle_diff) < RTS_MOVE_FACE_ANGLE) {
        Vec2 heading = Vec2{ cosf(sh->angle), sinf(sh->angle) };
        f32 brake_dist = (speed > 0.0f) ? (speed * speed) / (2.0f * SHIP_DECEL) : 0.0f;
        if (dist > brake_dist) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(heading, SHIP_ACCEL * dt));
        } else if (speed > 0.0f) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * dt / speed));
        }
    } else if (speed > 0.0f) {
        // Not facing the target yet: bleed some speed so we don't overshoot while rotating.
        fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * 0.5f * dt / speed));
    }
    f32 cur_speed = vec2_length(fl->velocity);
    if (cur_speed > SHIP_MAX_SPEED) fl->velocity = vec2_scale(fl->velocity, SHIP_MAX_SPEED / cur_speed);
}
// =====================================================================================
void FleetShip::update_attack(game_state* s, f32 dt) {
    if (order != FleetOrder::Attack) return;
    if (!attack_target) { order = FleetOrder::None; return; }
    CombatEntity* ce = find_combat_entity_for_ship(s, attack_target);
    if (!ce || !ce->active || !ce->ship) { order = FleetOrder::None; attack_target = nullptr; return; }
    Ship*       sh = &ship;
    ShipFlight* fl = &flight;
    if (ce->faction == sh->faction) { order = FleetOrder::None; attack_target = nullptr; return; } // no longer hostile
    Ship* target   = ce->ship;
    f32 ship_r     = ship_bounding_radius(sh);
    f32 target_r   = ship_bounding_radius(target);
    Vec2 to_target = vec2_sub(target->origin, sh->origin);
    f32 dist  = vec2_length(to_target);
    f32 speed = vec2_length(fl->velocity);
    // Rotate toward target.
    f32 desired_angle = atan2f(to_target.y, to_target.x);
    f32 angle_diff = normalize_angle(desired_angle - sh->angle);
    f32 max_rot = SHIP_MAX_TURN * dt;
    sh->angle += clampf(angle_diff, -max_rot, max_rot);
    fl->angular_velocity = 0.0f;
    // Approach to engagement range, but stop outside collision range.
    if (fabsf(angle_diff) < RTS_MOVE_FACE_ANGLE) {
        Vec2 heading = Vec2{ cosf(sh->angle), sinf(sh->angle) };
        f32 brake_dist = (speed > 0.0f) ? (speed * speed) / (2.0f * SHIP_DECEL) : 0.0f;
        f32 desired_dist = RTS_ATTACK_RANGE;
        f32 min_standoff = RTS_ATTACK_STOP_DIST + target_r + ship_r;
        if (desired_dist < min_standoff) desired_dist = min_standoff;
        if (dist > desired_dist + brake_dist) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(heading, SHIP_ACCEL * dt));
        } else if (dist < desired_dist - brake_dist) {
            Vec2 away = (dist > 0.0001f) ? vec2_scale(to_target, -1.0f / dist) : Vec2{ -heading.x, -heading.y };
            fl->velocity = vec2_add(fl->velocity, vec2_scale(away, SHIP_DECEL * dt));
        } else if (speed > 0.0f) {
            fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * dt / speed));
        }
    } else if (speed > 0.0f) {
        fl->velocity = vec2_add(fl->velocity, vec2_scale(fl->velocity, -SHIP_DECEL * 0.5f * dt / speed));
    }
    // Fire when facing the target, inside max range, and outside minimum range.
    if (fabsf(angle_diff) < RTS_ATTACK_FACE_ANGLE && dist >= RTS_ATTACK_MIN_RANGE && dist <= RTS_ATTACK_RANGE * 1.5f) {
        if (sh->active_weapon_idx >= 0 && sh->active_weapon_idx < sh->weapon_count) {
            Weapon* w = sh->weapons[sh->active_weapon_idx];
            if (w) {
                Vec2 fire_origin = ship_local_to_world(sh, sh->weapon_fire_offset_local);
                Vec2 aim_dir = to_target;
                Vec2 target_vel = ce->velocity;
                if ((target_vel.x != 0.0f || target_vel.y != 0.0f) && dist > 0.0001f) {
                    f32 proj_speed = w->projectile_speed();
                    if (proj_speed > 0.0001f) {
                        f32 lead_time = dist / proj_speed;
                        Vec2 lead_pos = vec2_add(target->origin, vec2_scale(target_vel, lead_time));
                        aim_dir = vec2_sub(lead_pos, fire_origin);
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
// Fleet
// =====================================================================================
Fleet::Fleet() : m_count(0), m_piloted_idx(0) {}
void Fleet::init() { m_count = 0; m_piloted_idx = 0; }
void Fleet::set_piloted(i32 idx) {
    if (idx < 0 || idx >= m_count) idx = 0;
    m_piloted_idx = idx;
    // A piloted ship stops following RTS orders.
    m_ships[idx].order = FleetOrder::None;
    m_ships[idx].attack_target = nullptr;
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
    fs.selected = FALSE;
    fs.order = FleetOrder::None;
    fs.move_target = Vec2{ 0.0f, 0.0f };
    fs.attack_target = nullptr;
    fs.flight.velocity = Vec2{ 0.0f, 0.0f };
    fs.flight.angular_velocity = 0.0f;
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
i32 Fleet::select_in_box(Vec2 p0, Vec2 p1) {
    if (p0.x > p1.x) { f32 t = p0.x; p0.x = p1.x; p1.x = t; }
    if (p0.y > p1.y) { f32 t = p0.y; p0.y = p1.y; p1.y = t; }
    i32 n = 0;
    for (i32 i = 0; i < m_count; ++i) {
        Vec2 o = m_ships[i].ship.origin;
        b8 inside = (o.x >= p0.x && o.x <= p1.x && o.y >= p0.y && o.y <= p1.y);
        m_ships[i].selected = inside;
        if (inside) ++n;
    }
    return n;
}
b8 Fleet::select_at_point(Vec2 world) {
    i32 hit = -1;
    f32 best = 0.0f;
    for (i32 i = 0; i < m_count; ++i) {
        f32 r = ship_bounding_radius(&m_ships[i].ship);
        f32 d = vec2_length(vec2_sub(world, m_ships[i].ship.origin));
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
void Fleet::order_move(Vec2 target) {
    // Collect the selected ships.
    i32 sel[FLEET_MAX_SHIPS];
    i32 n = 0;
    f32 max_r = 0.0f;
    Vec2 centroid = Vec2{ 0.0f, 0.0f };
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        sel[n++] = i;
        centroid = vec2_add(centroid, m_ships[i].ship.origin);
        f32 r = ship_bounding_radius(&m_ships[i].ship);
        if (r > max_r) max_r = r;
    }
    if (n == 0) return;
    centroid = vec2_scale(centroid, 1.0f / (f32)n);
    if (n == 1) {
        m_ships[sel[0]].order = FleetOrder::Move;
        m_ships[sel[0]].move_target = target;
        return;
    }
    // Forward axis = centroid -> target; right axis = perpendicular.
    Vec2 forward = vec2_sub(target, centroid);
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
        Vec2 slot = vec2_add(target, vec2_add(vec2_scale(right, ox), vec2_scale(forward, oy)));
        m_ships[sel[k]].order = FleetOrder::Move;
        m_ships[sel[k]].move_target = slot;
        m_ships[sel[k]].attack_target = nullptr;
    }
}
void Fleet::order_attack(Ship* target) {
    if (!target) return;
    for (i32 i = 0; i < m_count; ++i) {
        if (!m_ships[i].selected) continue;
        m_ships[i].order = FleetOrder::Attack;
        m_ships[i].attack_target = target;
    }
}
void Fleet::clear_order(i32 idx) {
    if (idx < 0 || idx >= m_count) return;
    m_ships[idx].order = FleetOrder::None;
    m_ships[idx].attack_target = nullptr;
}
// =====================================================================================
void Fleet::update_autopilot(game_state* s, f32 dt, i32 piloted_idx) {
    for (i32 i = 0; i < m_count; ++i) {
        if (i == piloted_idx) continue;
        if (m_ships[i].order == FleetOrder::Move)        m_ships[i].update_move(dt);
        else if (m_ships[i].order == FleetOrder::Attack) m_ships[i].update_attack(s, dt);
    }
}
void Fleet::simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx) {
    for (i32 i = 0; i < m_count; ++i) {
        b8 tc = (i == piloted_idx) ? piloted_turn_commanded : FALSE;
        m_ships[i].simulate(dt, tc);
    }
}
