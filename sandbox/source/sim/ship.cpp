#include "sim/ship.h"
#include "sim/ship_def.h" // ShipDef speed_limits (ship_speed_cap)
#include "sim/weapon.h"   // Weapon::ready (bearing-weapon selection)
#include "sim/weapon_def.h" // WeaponDef muzzles (ship_hardpoint_fire)
#include "sim/module.h"   // ModuleDef (fit test + sensor stat composition)
#include <core/logger.h>
#include <renderer/renderer.h>
#include <math.h>
using namespace bs_math;
// =====================================================================================
// Size-class motion tuning. DRONE mirrors the legacy SHIP_* globals; larger classes get
// progressively heavier: lower speed caps, weaker thrust, and much slower turning, so a
// cruiser feels like a capital ship instead of a strike craft.
// =====================================================================================
static const ShipMotion g_class_motion[SHIP_CLASS_COUNT] = {
    // max_speed  accel   decel  turn_accel max_turn
    {   800.0f,  600.0f, 400.0f,  12.0f,    3.00f }, // DRONE
    {   650.0f,  420.0f, 300.0f,   8.0f,    2.20f }, // CORVETTE
    {   500.0f,  280.0f, 200.0f,   5.0f,    1.40f }, // FRIGATE
    {   360.0f,  160.0f, 120.0f,   2.6f,    0.80f }, // DESTROYER
    {   240.0f,   90.0f,  70.0f,   1.4f,    0.45f }, // CRUISER
    {   150.0f,   50.0f,  40.0f,   0.7f,    0.25f }, // CAPITAL
};
static const char* g_class_names[SHIP_CLASS_COUNT] = {
    "Drone", "Corvette", "Frigate", "Destroyer", "Cruiser", "Capital",
};
ShipSizeClass ship_size_class_from_length(f32 hull_length_world) {
    if (hull_length_world <   60.0f) return SHIP_CLASS_DRONE;
    if (hull_length_world <  150.0f) return SHIP_CLASS_CORVETTE;
    if (hull_length_world <  350.0f) return SHIP_CLASS_FRIGATE;
    if (hull_length_world <  700.0f) return SHIP_CLASS_DESTROYER;
    if (hull_length_world < 1800.0f) return SHIP_CLASS_CRUISER;
    return SHIP_CLASS_CAPITAL;
}
const ShipMotion& ship_motion_for_class(ShipSizeClass c) {
    if ((i32)c < 0 || (i32)c >= SHIP_CLASS_COUNT) c = SHIP_CLASS_DRONE;
    return g_class_motion[c];
}
const char* ship_size_class_name(ShipSizeClass c) {
    if ((i32)c < 0 || (i32)c >= SHIP_CLASS_COUNT) return "Unknown";
    return g_class_names[c];
}
f32 ship_speed_cap(const Ship* ship) {
    if (!ship) return 0.0f;
    if (ship->def && ship->def->speed_limit_count > 0) {
        i32 sel = ship->speed_limit_sel;
        if (sel < 0) sel = 0;
        if (sel >= ship->def->speed_limit_count) sel = ship->def->speed_limit_count - 1;
        return ship->def->speed_limits[sel];
    }
    return ship->motion.max_speed;
}
f32 hardpoint_half_extent(HardpointSize s) {
    switch (s) {
        case HARDPOINT_SMALL:  return 35.0f;
        case HARDPOINT_MEDIUM: return 60.0f;
        case HARDPOINT_LARGE:  return 90.0f;
    }
    return 35.0f;
}
const char* hardpoint_size_name(HardpointSize s) {
    switch (s) {
        case HARDPOINT_SMALL:  return "S";
        case HARDPOINT_MEDIUM: return "M";
        case HARDPOINT_LARGE:  return "L";
    }
    return "?";
}
b8 hardpoint_accepts(const HardpointDef* hp, u32 module_type) {
    return (hp && (hp->accepts & module_type)) ? TRUE : FALSE;
}
i32 ship_first_free_hardpoint(const Ship* ship, u32 module_type) {
    if (!ship) return -1;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (!(ship->hardpoints[i].accepts & module_type)) continue;
        if (ship->mounts[i]) continue;
        if (ship->point_defense_mount == i) continue;
        if (ship->module_mounts[i]) continue;
        return i;
    }
    return -1;
}
b8 hardpoint_fits_module(const HardpointDef* hp, const ModuleDef* def) {
    if (!hp || !def) return FALSE;
    if (!(hp->accepts & def->type)) return FALSE;
    return (def->size <= hp->size) ? TRUE : FALSE;
}
b8 hardpoint_takes_weapon(const HardpointDef* hp, const Weapon* w) {
    if (!hp || !w) return FALSE;
    if (hp->accepts & MODULE_TYPE_WEAPON) return TRUE;
    return ((hp->accepts & MODULE_TYPE_MISSILE) && w->wkind == WEAPON_KIND_MISSILE) ? TRUE : FALSE;
}
void ship_recompute_stats(Ship* ship) {
    if (!ship) return;
    // Effective sensors = hull baseline x the product of every mounted sensor module's
    // per-layer multipliers (multiple arrays compose multiplicatively).
    ship->sensors = ship->sensors_base;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        const ModuleDef* m = ship->module_mounts[i];
        if (!m || m->type != MODULE_TYPE_SENSOR) continue;
        ship->sensors.layer0_radius *= m->sensor_mult[0];
        ship->sensors.layer1_radius *= m->sensor_mult[1];
        ship->sensors.layer2_radius *= m->sensor_mult[2];
    }
    // Effective capacitor = hull baseline (capacitor modules will multiply here later).
    ship->cap_max   = ship->cap_max_base;
    ship->cap_regen = ship->cap_regen_base;
    if (ship->cap_current > ship->cap_max) ship->cap_current = ship->cap_max;
}
b8 ship_try_spend_cap(Ship* ship, f32 cost) {
    if (!ship) return FALSE;
    if (cost <= 0.0f) return TRUE;
    if (ship->cap_current < cost) return FALSE;
    ship->cap_current -= cost;
    return TRUE;
}
void ship_capacitor_update(Ship* ship, f32 dt) {
    if (!ship || dt <= 0.0f) return;
    ship->cap_current += ship->cap_regen * dt;
    if (ship->cap_current > ship->cap_max) ship->cap_current = ship->cap_max;
}
b8 ship_hardpoint_in_group(const Ship* ship, i32 hp_index, i32 g) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return FALSE;
    if (g < 0 || g >= SHIP_WEAPON_GROUPS) return FALSE;
    if (!ship->mounts[hp_index]) return FALSE;
    return ((ship->mount_groups[hp_index] >> g) & 1) ? TRUE : FALSE;
}
void ship_select_weapon_group(Ship* ship, i32 g) {
    if (!ship || g < 0 || g >= SHIP_WEAPON_GROUPS) return;
    ship->active_group = (u8)g;
    // Choosing a group is an explicit return to group fire control: any single-weapon
    // micro-selection override is dropped.
    ship->weapon_override = -1;
    // Re-home the legacy active index onto the group's first member (kept for the HUD
    // weapon bar and other single-weapon consumers); left untouched when the group is empty.
    i32 first = ship_nth_group_weapon(ship, g, 0);
    if (first >= 0) ship->active_weapon_idx = first;
}
i32 ship_group_size(const Ship* ship, i32 g) {
    if (!ship) return 0;
    i32 n = 0;
    for (i32 i = 0; i < ship->hardpoint_count; ++i)
        if (ship_hardpoint_in_group(ship, i, g)) ++n;
    return n;
}
i32 ship_nth_group_weapon(const Ship* ship, i32 g, i32 n) {
    if (!ship || n < 0) return -1;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (!ship_hardpoint_in_group(ship, i, g)) continue;
        if (n-- == 0) return i;
    }
    return -1;
}
void ship_groups_sanitize(Ship* ship) {
    if (!ship) return;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (!ship->mounts[i])              ship->mount_groups[i] = 0;
        else if (!ship->mount_groups[i])   ship->mount_groups[i] = 1;   // default: group 1
    }
    // A micro-selection override pointing at a slot the loadout editor just emptied would
    // silently fire nothing; fall back to "All" instead.
    if (ship->weapon_override >= 0 &&
        (ship->weapon_override >= ship->hardpoint_count || !ship->mounts[ship->weapon_override]))
        ship->weapon_override = -1;
    // Likewise an override the fire-group matrix just took OUT of the active group. The hub
    // only offers active-group members, so such an override has no tile left to show it as
    // selected: one weapon would keep firing with nothing on screen saying which. Back to "All".
    if (ship->weapon_override >= 0 &&
        !ship_hardpoint_in_group(ship, ship->weapon_override, ship->active_group))
        ship->weapon_override = -1;
}
// ---- Weapon micro-selection (middle-mouse hub) -----------------------------------------
void ship_select_weapon_override(Ship* ship, i32 hp_index) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    if (!ship->mounts[hp_index]) return;
    ship->weapon_override  = (i8)hp_index;
    // Keep the legacy single-weapon consumers (HUD weapon bar, gizmos) pointing at what fires.
    ship->active_weapon_idx = hp_index;
}
void ship_clear_weapon_override(Ship* ship) {
    if (!ship) return;
    // Reselecting the active group both drops the override and re-homes active_weapon_idx.
    ship_select_weapon_group(ship, ship->active_group);
}
b8 ship_hardpoint_in_selection(const Ship* ship, i32 hp_index) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return FALSE;
    if (!ship->mounts[hp_index]) return FALSE;
    if (ship->weapon_override >= 0) return (hp_index == ship->weapon_override) ? TRUE : FALSE;
    return ship_hardpoint_in_group(ship, hp_index, ship->active_group);
}
// Wrap an angle into (-pi, pi]. Defined here rather than beside the turret code below because
// the fire validator needs it too, to measure how far a barrel still has to travel.
static f32 wrap_pi(f32 a) {
    while (a >  BS_PI) a -= 2.0f * BS_PI;
    while (a < -BS_PI) a += 2.0f * BS_PI;
    return a;
}
// ---- Per-weapon fire validation --------------------------------------------------------
WeaponFireState ship_weapon_fire_state(const Ship* ship, i32 hp_index, Vec2 world_dir, f32 dist) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return WEAPON_FIRE_DISABLED;
    const Weapon* w = ship->mounts[hp_index];
    if (!w || w->disabled) return WEAPON_FIRE_DISABLED;
    // Traverse arc first: a mount that cannot bear will never fire whatever else is true.
    if (!ship_hardpoint_can_aim(ship, hp_index, world_dir)) return WEAPON_FIRE_NO_BEARING;
    // Then whether the barrel has GOT there yet. The arc test above answers "can this mount ever
    // point at the target"; this one answers "does it, now". Without it a held trigger fired the
    // instant the target entered the arc, while mount_aim was still mid-slew -- and because
    // ship_muzzle_origin resolves the barrel against mount_aim while the shot flies along
    // world_dir, the round left the correct barrel tip at a visible angle to the barrel.
    //
    // The tolerance is a VISUAL one: it bounds how far the barrel may be off the shot, not how
    // far the shot may be off the target (the round still flies exactly along world_dir). ~3
    // degrees is imperceptible on the art and, at one frame's slew for a medium mount (2.2 rad/s
    // is 0.037 rad at 60 fps), loose enough that a turret tracking a moving cursor keeps firing
    // instead of stuttering every time the goal shifts.
    {
        const f32 AIM_TOL = 0.055f; // radians (~3.2 degrees)
        f32 goal = ship_hardpoint_aim_goal(ship, hp_index, world_dir);
        if (fabsf(wrap_pi(goal - ship->mount_aim[hp_index])) > AIM_TOL)
            return WEAPON_FIRE_SLEWING;
    }
    if (!w->ready()) return WEAPON_FIRE_RELOADING;
    // Range: weapon_effective_reach is the single source of truth the HUD ring also draws, so
    // the ship never opens fire at a distance the ring says it cannot cover. A weapon with no
    // usable stats (reach 0) is treated as unbounded rather than permanently out of range.
    if (dist >= 0.0f) {
        f32 reach = weapon_effective_reach(w);
        if (reach > 0.0f && dist > reach) return WEAPON_FIRE_OUT_OF_RANGE;
    }
    // Affordability WITHOUT spending: the caller commits via ship_try_spend_cap.
    f32 cost = w->cap_cost();
    if (cost > 0.0f && ship->cap_current < cost) return WEAPON_FIRE_STARVED;
    return WEAPON_FIRE_READY;
}
HierPos2 ship_hardpoint_fire_origin(const Ship* ship, i32 hp_index) {
    if (!ship) return HierPos2{};
    if (hp_index < 0 || hp_index >= ship->hardpoint_count)
        return ship_local_to_world(ship, ship->weapon_fire_offset_local);
    return ship_local_to_world(ship, ship->hardpoints[hp_index].pos_local);
}
f32 ship_hardpoint_unit(const Ship* ship, i32 hp_index) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return 0.0f;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    return hardpoint_half_extent(hp.size) * ship->world_scale * hp.art_scale;
}
HierPos2 ship_muzzle_origin(const Ship* ship, i32 hp_index, i32 muzzle_index) {
    HierPos2 pivot = ship_hardpoint_fire_origin(ship, hp_index);
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return pivot;
    const Weapon* w = ship->mounts[hp_index];
    const WeaponDef* def = w ? w->def : nullptr;
    const i32 n = def ? def->muzzle_count : 0;
    if (n <= 0 || muzzle_index < 0) return pivot;   // no barrels authored: the mount centre

    // Muzzle offsets are authored in the weapon's own frame, so resolve them against
    // mount_aim -- the SLEWED aim the turret art is drawn at -- not the direction being
    // fired along. The two differ while a turret is still traversing, and it is the art
    // the player is watching the shot leave.
    const f32  unit = ship_hardpoint_unit(ship, hp_index);
    const f32  aim  = ship->angle + ship->mount_aim[hp_index];
    const Vec2 fwd  = vec2_rotate(Vec2{ 0.0f, 1.0f }, aim);
    const Vec2 rgt  = vec2_rotate(Vec2{ 1.0f, 0.0f }, aim);
    const Vec2 m    = def->muzzles[muzzle_index % n];
    const Vec2 off  = vec2_add(vec2_scale(rgt, m.x * unit), vec2_scale(fwd, m.y * unit));
    return hierpos_add_vec2(&pivot, off);
}
void ship_hardpoint_fire(Ship* ship, i32 hp_index, Vec2 world_dir, Vec2 ship_velocity,
                         ProjectileSystem* projectiles) {
    if (!ship || !projectiles) return;
    if (hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    Weapon* w = ship->mounts[hp_index];
    if (!w) return;

    const WeaponDef* def = w->def;
    const i32 n = def ? def->muzzle_count : 0;
    if (n <= 0) {
        // No barrels authored: fire from the mount centre, exactly as before barrels existed.
        w->fire(ship_hardpoint_fire_origin(ship, hp_index), world_dir, ship_velocity, projectiles);
        return;
    }
    if (!w->ready()) return;   // gate ONCE, so every barrel of a salvo gets through

    const i32 first = (def->muzzle_pattern == MUZZLE_SALVO) ? 0 : (i32)(w->next_muzzle % n);
    const i32 count = (def->muzzle_pattern == MUZZLE_SALVO) ? n : 1;
    for (i32 k = 0; k < count; ++k)
        w->spawn_shot(ship_muzzle_origin(ship, hp_index, (first + k) % n),
                      world_dir, ship_velocity, projectiles);
    w->next_muzzle = (u8)((first + count) % n);
    w->begin_cooldown();
}
b8 ship_hardpoint_can_aim(const Ship* ship, i32 hp_index, Vec2 world_dir) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return TRUE;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    if (hp.arc >= 2.0f * BS_PI - 1.0e-3f) return TRUE;   // full-circle turret
    if (world_dir.x == 0.0f && world_dir.y == 0.0f) return FALSE;
    // Ship convention: angle 0 = nose = +Y, so a world direction's heading is atan2(-x, y).
    f32 dir_angle = atan2f(-world_dir.x, world_dir.y);
    f32 diff = dir_angle - (ship->angle + hp.facing);
    while (diff >  BS_PI) diff -= 2.0f * BS_PI;
    while (diff < -BS_PI) diff += 2.0f * BS_PI;
    return (fabsf(diff) <= hp.arc * 0.5f) ? TRUE : FALSE;
}
i32 ship_select_bearing_weapon(const Ship* ship, Vec2 world_dir) {
    if (!ship) return -1;
    i32 a = ship->active_weapon_idx;
    if (a >= 0 && a < ship->hardpoint_count && ship->mounts[a] &&
        ship->mounts[a]->ready() && ship_hardpoint_can_aim(ship, a, world_dir))
        return a;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (i == a) continue;
        if (!ship->mounts[i] || !ship->mounts[i]->ready()) continue;
        if (!ship_hardpoint_can_aim(ship, i, world_dir)) continue;
        return i;
    }
    return -1;
}
// ---- Turret traverse (Phase 5) ----------------------------------------------------------
// Traverse rate by slot size: big mounts swing slower (rad/s).
static f32 turret_slew_rate(HardpointSize s) {
    switch (s) {
        case HARDPOINT_SMALL:  return 3.5f;
        case HARDPOINT_MEDIUM: return 2.2f;
        case HARDPOINT_LARGE:  return 1.4f;
    }
    return 2.2f;
}
f32 ship_hardpoint_aim_goal(const Ship* ship, i32 hp_index, Vec2 world_dir) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return 0.0f;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    if (world_dir.x == 0.0f && world_dir.y == 0.0f) return hp.facing;
    // Ship convention: angle 0 = nose = +Y, so a world direction's heading is atan2(-x, y).
    f32 dir_angle = atan2f(-world_dir.x, world_dir.y);
    f32 rel = wrap_pi(dir_angle - ship->angle - hp.facing); // offset from the rest facing
    if (hp.arc < 2.0f * BS_PI - 1.0e-3f) {
        f32 half = hp.arc * 0.5f;
        rel = clampf(rel, -half, half); // pin at the traverse-arc edge, never through it
    }
    return hp.facing + rel; // ship-local goal angle
}
void ship_turret_aim_at(Ship* ship, i32 hp_index, Vec2 world_dir) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    if (world_dir.x == 0.0f && world_dir.y == 0.0f) return;
    ship->mount_aim_goal[hp_index]    = ship_hardpoint_aim_goal(ship, hp_index, world_dir);
    ship->mount_aim_engaged[hp_index] = TRUE;
}
void ship_update_turrets(Ship* ship, f32 dt) {
    if (!ship || dt <= 0.0f) return;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        const HardpointDef& hp = ship->hardpoints[i];
        // Idle turrets return to the hardpoint's rest facing.
        f32 target = ship->mount_aim_engaged[i] ? ship->mount_aim_goal[i] : hp.facing;
        f32 step   = turret_slew_rate(hp.size) * dt;
        if (hp.arc >= 2.0f * BS_PI - 1.0e-3f) {
            // Full-circle mount: no blocked wedge exists, so the shortest way round is both
            // legal and what the player expects. This is the point-defense case.
            f32 diff = wrap_pi(target - ship->mount_aim[i]);
            ship->mount_aim[i] = wrap_pi(ship->mount_aim[i] + clampf(diff, -step, step));
        } else {
            // Limited traverse: slew in ARC-RELATIVE space, taking the DIRECT difference
            // rather than the shortest way round. wrap_pi caps a turn at 180 degrees, so on
            // an arc WIDER than 180 the two edges are further apart through the arc than
            // around the back of it -- and the shortest route is then straight across the
            // blocked wedge, which looked like the turret spinning the wrong way. (The 180
            // degree bow mount never showed it: every legal pair is <= 180 apart, so the
            // short way is always the in-arc way.) Interpolating between two clamped
            // offsets can only pass through offsets between them, so the barrel is confined
            // to the arc by construction. An out-of-arc current angle -- a stale save, or
            // state left by the old path -- still walks back in rather than snapping.
            f32 half     = hp.arc * 0.5f;
            f32 cur_rel  = wrap_pi(ship->mount_aim[i] - hp.facing);
            f32 goal_rel = clampf(wrap_pi(target - hp.facing), -half, half);
            f32 new_rel  = cur_rel + clampf(goal_rel - cur_rel, -step, step);
            ship->mount_aim[i] = wrap_pi(hp.facing + new_rel);
        }
        ship->mount_aim_engaged[i] = FALSE; // consumed; gameplay re-asserts every frame
    }
}
const char* vessel_faction_name(VesselFaction f) {
    switch (f) {
        case VESSEL_PIRATE:     return "Pirate";
        case VESSEL_FEDERATION: return "Federation";
        case VESSEL_NEUTRAL:    return "Neutral";
        case VESSEL_STATION:    return "Station";
        case VESSEL_DERELICT:   return "Derelict";
        default:                return "Unknown";
    }
}
const char* vessel_faction_desc(VesselFaction f) {
    switch (f) {
        case VESSEL_PIRATE:     return "Hostile vessel - weapons hot.";
        case VESSEL_FEDERATION: return "Friendly military patrol.";
        case VESSEL_NEUTRAL:    return "Unaffiliated trader or traveller.";
        case VESSEL_STATION:    return "Orbital installation - no drive signature.";
        case VESSEL_DERELICT:   return "Abandoned hull - no life signs.";
        default:                return "No data available.";
    }
}
Vec2 ship_local_dir(const Ship* ship, Vec2 local) {
    return vec2_rotate(vec2_scale(local, ship->world_scale), ship->angle);
}
HierPos2 ship_local_to_world(const Ship* ship, Vec2 local) {
    return hierpos_add_vec2(&ship->origin, ship_local_dir(ship, local));
}
Vec2 ship_world_to_local(const Ship* ship, HierPos2 world) {
    Vec2 rel = hierpos_diff(&world, &ship->origin);
    return vec2_scale(vec2_rotate(rel, -ship->angle), 1.0f / ship->world_scale);
}
f32 ship_bounding_radius(const Ship* ship) {
    if (!ship || ship->collider_count <= 0) return 0.0f;
    f32 max_r2 = 0.0f;
    for (i32 i = 0; i < ship->collider_count; ++i) {
        f32 r2 = ship->collider_verts[i].x * ship->collider_verts[i].x
               + ship->collider_verts[i].y * ship->collider_verts[i].y;
        if (r2 > max_r2) max_r2 = r2;
    }
    return sqrtf(max_r2) * ship->world_scale;
}
b8 ships_overlap(const Ship* a, const Ship* b) {
    if (!a || !b) return FALSE;
    f32 d  = vec2_length(hierpos_diff(&a->origin, &b->origin));
    f32 rr = ship_bounding_radius(a) + ship_bounding_radius(b);
    return (d < rr) ? TRUE : FALSE;
}
static b8 sat_poly(const Vec2* a, i32 na, const Vec2* b, i32 nb, Vec2 axis, f32* out_overlap) {
    f32 a_min = 1.0e30f, a_max = -1.0e30f;
    for (i32 i = 0; i < na; ++i) {
        f32 p = a[i].x * axis.x + a[i].y * axis.y;
        if (p < a_min) a_min = p;
        if (p > a_max) a_max = p;
    }
    f32 b_min = 1.0e30f, b_max = -1.0e30f;
    for (i32 i = 0; i < nb; ++i) {
        f32 p = b[i].x * axis.x + b[i].y * axis.y;
        if (p < b_min) b_min = p;
        if (p > b_max) b_max = p;
    }
    if (a_max < b_min || b_max < a_min) return FALSE;
    f32 o0 = a_max - b_min;
    f32 o1 = b_max - a_min;
    *out_overlap = (o0 < o1) ? o0 : o1;
    return TRUE;
}
b8 ships_collide(const Ship* a, const Ship* b, Vec2* out_mtv) {
    if (!a || !b) return FALSE;
    if (!ships_overlap(a, b)) return FALSE;
    if (a->collider_count < 3 || b->collider_count < 3) return FALSE;
    // Work in a frame relative to a->origin: a's verts are direction-only offsets, b's verts
    // are offset by the small (a-relative) difference of the two origins. SAT overlap and the
    // resulting MTV direction/magnitude are frame-independent, so this is exact at any distance.
    Vec2 b_rel = hierpos_diff(&b->origin, &a->origin);
    Vec2 aw[SHIP_MAX_COLLIDER_VERTS];
    Vec2 bw[SHIP_MAX_COLLIDER_VERTS];
    for (i32 i = 0; i < a->collider_count; ++i) aw[i] = ship_local_dir(a, a->collider_verts[i]);
    for (i32 i = 0; i < b->collider_count; ++i) bw[i] = vec2_add(b_rel, ship_local_dir(b, b->collider_verts[i]));
    f32 min_overlap = 1.0e30f;
    Vec2 min_axis = Vec2{ 0.0f, 0.0f };
    for (i32 s = 0; s < 2; ++s) {
        const Vec2* poly = (s == 0) ? aw : bw;
        i32 n = (s == 0) ? a->collider_count : b->collider_count;
        for (i32 i = 0; i < n; ++i) {
            Vec2 edge = vec2_sub(poly[(i + 1) % n], poly[i]);
            Vec2 axis = vec2_normalized(Vec2{ -edge.y, edge.x });
            f32 overlap;
            if (!sat_poly(aw, a->collider_count, bw, b->collider_count, axis, &overlap))
                return FALSE;
            if (overlap < min_overlap) { min_overlap = overlap; min_axis = axis; }
        }
    }
    if (out_mtv) {
        f32 sign = (vec2_dot(b_rel, min_axis) > 0.0f) ? -1.0f : 1.0f;
        *out_mtv = vec2_scale(min_axis, sign * min_overlap);
    }
    return TRUE;
}
b8 ship_collider_corners(const Ship* ship, Vec2 out_corners[SHIP_MAX_COLLIDER_VERTS]) {
    if (!ship || ship->collider_count <= 0) return FALSE;
    for (i32 i = 0; i < ship->collider_count; ++i)
        out_corners[i] = ship_local_dir(ship, ship->collider_verts[i]);
    return TRUE;
}
