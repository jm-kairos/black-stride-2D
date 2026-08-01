// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "sim/ship.h"
#include "sim/weapon.h"   // Weapon::ready (bearing-weapon selection)
#include "sim/module.h"   // ModuleDef (fit test + sensor stat composition)
#include <core/logger.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <new>
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
Weapon* ship_active_weapon(const Ship* ship) {
    if (!ship) return nullptr;
    if (ship->active_weapon_idx < 0 || ship->active_weapon_idx >= ship->hardpoint_count) return nullptr;
    return ship->mounts[ship->active_weapon_idx];
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
}
void ship_select_weapon_slot(Ship* ship, i32 n) {
    if (!ship || n < 0) return;
    i32 seen = 0;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (!ship->mounts[i]) continue;
        if (seen == n) { ship->active_weapon_idx = i; return; }
        ++seen;
    }
}
HierPos2 ship_hardpoint_fire_origin(const Ship* ship, i32 hp_index) {
    if (!ship) return HierPos2{};
    if (hp_index < 0 || hp_index >= ship->hardpoint_count)
        return ship_local_to_world(ship, ship->weapon_fire_offset_local);
    return ship_local_to_world(ship, ship->hardpoints[hp_index].pos_local);
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
static f32 wrap_pi(f32 a) {
    while (a >  BS_PI) a -= 2.0f * BS_PI;
    while (a < -BS_PI) a += 2.0f * BS_PI;
    return a;
}
// Traverse rate by slot size: big mounts swing slower (rad/s).
static f32 turret_slew_rate(HardpointSize s) {
    switch (s) {
        case HARDPOINT_SMALL:  return 3.5f;
        case HARDPOINT_MEDIUM: return 2.2f;
        case HARDPOINT_LARGE:  return 1.4f;
    }
    return 2.2f;
}
void ship_turret_aim_at(Ship* ship, i32 hp_index, Vec2 world_dir) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    if (world_dir.x == 0.0f && world_dir.y == 0.0f) return;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    // Ship convention: angle 0 = nose = +Y, so a world direction's heading is atan2(-x, y).
    f32 dir_angle = atan2f(-world_dir.x, world_dir.y);
    f32 rel = wrap_pi(dir_angle - ship->angle - hp.facing); // offset from the rest facing
    if (hp.arc < 2.0f * BS_PI - 1.0e-3f) {
        f32 half = hp.arc * 0.5f;
        rel = clampf(rel, -half, half); // pin at the traverse-arc edge, never through it
    }
    ship->mount_aim_goal[hp_index]    = hp.facing + rel; // ship-local goal angle
    ship->mount_aim_engaged[hp_index] = TRUE;
}
void ship_update_turrets(Ship* ship, f32 dt) {
    if (!ship || dt <= 0.0f) return;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        const HardpointDef& hp = ship->hardpoints[i];
        // Idle turrets return to the hardpoint's rest facing.
        f32 target = ship->mount_aim_engaged[i] ? ship->mount_aim_goal[i] : hp.facing;
        f32 diff   = wrap_pi(target - ship->mount_aim[i]);
        f32 step   = turret_slew_rate(hp.size) * dt;
        ship->mount_aim[i]         = wrap_pi(ship->mount_aim[i] + clampf(diff, -step, step));
        ship->mount_aim_engaged[i] = FALSE; // consumed; gameplay re-asserts every frame
    }
}
// Parse a '|'-joined module-kind list ("weapon", "weapon|defense", ...) into a
// MODULE_TYPE_* bitmask. Unknown kinds are skipped with a warning; returns 0 if
// nothing matched.
static u32 parse_module_mask(const char* list, const char* path) {
    u32 mask = 0;
    char buf[64];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* tok = buf;
    while (tok && *tok) {
        char* sep = strchr(tok, '|');
        if (sep) *sep = '\0';
        if      (_stricmp(tok, "weapon")  == 0) mask |= MODULE_TYPE_WEAPON;
        else if (_stricmp(tok, "defense") == 0) mask |= MODULE_TYPE_DEFENSE;
        else if (_stricmp(tok, "sensor")  == 0) mask |= MODULE_TYPE_SENSOR;
        else if (_stricmp(tok, "engine")  == 0) mask |= MODULE_TYPE_ENGINE;
        else if (_stricmp(tok, "utility") == 0) mask |= MODULE_TYPE_UTILITY;
        else BS_LOG_WARN("ship_load: unknown module kind '%s' in '%s'.", tok, path);
        tok = sep ? sep + 1 : nullptr;
    }
    return mask;
}
static b8 parse_hardpoint_size(const char* tok, HardpointSize* out) {
    if (_stricmp(tok, "S") == 0 || _stricmp(tok, "small")  == 0) { *out = HARDPOINT_SMALL;  return TRUE; }
    if (_stricmp(tok, "M") == 0 || _stricmp(tok, "medium") == 0) { *out = HARDPOINT_MEDIUM; return TRUE; }
    if (_stricmp(tok, "L") == 0 || _stricmp(tok, "large")  == 0) { *out = HARDPOINT_LARGE;  return TRUE; }
    return FALSE;
}
static i32 rstrip(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    return n;
}
b8 ship_load(Ship* out_ship, const char* path) {
    if (!out_ship || !path) {
        BS_LOG_ERROR("ship_load: null ship or path.");
        return FALSE;
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_ERROR("ship_load: could not open '%s'.", path);
        return FALSE;
    }
    new (out_ship) Ship();
    out_ship->world_scale  = 1.0f;
    out_ship->origin       = HierPos2{};
    out_ship->angle        = 0.0f;
    out_ship->faction      = VESSEL_NEUTRAL;
    out_ship->vessel_name  = "Unnamed Vessel";
    out_ship->collider_count = 0;
    out_ship->size_local   = Vec2{ 0.0f, 0.0f };
    out_ship->weapon_fire_offset_local = Vec2{ 0.0f, 0.0f };
    out_ship->sensors      = SensorSuite{};
    out_ship->sensors_base = SensorSuite{};
    out_ship->point_defense = DefenseLaser{};
    b8 class_authored = FALSE;
    out_ship->size_class = SHIP_CLASS_DRONE;
    out_ship->hardpoint_count = 0;
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) out_ship->mounts[i] = nullptr;
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) out_ship->module_mounts[i] = nullptr;
    out_ship->active_weapon_idx    = -1;
    out_ship->weapon_stash_count   = 0;
    out_ship->module_stash_count   = 0;
    out_ship->point_defense_mount  = -1;
    ship_visual_clear(&out_ship->visual);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (strncmp(line, "name", 4) == 0) {
            char* start = line + 4;
            while (*start == ' ' || *start == '\t') ++start;
            if (*start == '"') { ++start; char* end = strchr(start, '"'); if (end) *end = '\0'; }
            out_ship->vessel_name = start;
            continue;
        }
        int faction_id;
        if (sscanf(line, "faction %d", &faction_id) == 1) {
            if (faction_id >= 0 && faction_id < 5) out_ship->faction = (VesselFaction)faction_id;
            continue;
        }
        f32 ws;
        if (sscanf(line, "world_scale %f", &ws) == 1 && ws > 0.0f) {
            out_ship->world_scale = ws;
            continue;
        }
        f32 sw, sh;
        if (sscanf(line, "size %f %f", &sw, &sh) == 2) {
            out_ship->size_local = Vec2{ sw, sh };
            out_ship->visual.size_local = Vec2{ sw, sh };
            continue;
        }
        char classbuf[16];
        if (sscanf(line, "class %15s", classbuf) == 1) {
            b8 matched = FALSE;
            for (i32 c = 0; c < SHIP_CLASS_COUNT; ++c) {
                if (_stricmp(classbuf, ship_size_class_name((ShipSizeClass)c)) == 0) {
                    out_ship->size_class = (ShipSizeClass)c;
                    class_authored = TRUE;
                    matched = TRUE;
                    break;
                }
            }
            if (!matched)
                BS_LOG_WARN("ship_load: unknown class '%s' in '%s' (deriving from size).", classbuf, path);
            continue;
        }
        char kindbuf[16];
        if (sscanf(line, "layer %15s", kindbuf) == 1) {
            if (out_ship->visual.layer_count >= VIS_MAX_LAYERS) continue;
            VisualLayer& l = out_ship->visual.layers[out_ship->visual.layer_count];
            l = VisualLayer{};
            if (strcmp(kindbuf, "sprite") == 0) {
                char tex[128]; f32 ox, oy, ppu; u32 z, roof;
                if (sscanf(line, "layer sprite %127s %f %f %f %u %u", tex, &ox, &oy, &ppu, &z, &roof) == 6) {
                    l.kind = VIS_LAYER_SPRITE;
                    strncpy(l.texture_path, tex, sizeof(l.texture_path) - 1);
                    l.texture_path[sizeof(l.texture_path) - 1] = '\0';
                    l.offset_local = Vec2{ ox, oy };
                    l.px_per_unit = (ppu > 0.0f) ? ppu : 1.0f;
                    l.z = z; l.roof_only = roof ? TRUE : FALSE;
                    out_ship->visual.layer_count++;
                    out_ship->visual.has_sprite = TRUE;
                }
            } else if (strcmp(kindbuf, "mapped") == 0) {
                char diffuse[128], normal[128], depth[128], position[128]; f32 ox, oy, ppu; u32 z, roof;
                if (sscanf(line, "layer mapped %127s %127s %127s %127s %f %f %f %u %u",
                           diffuse, normal, depth, position, &ox, &oy, &ppu, &z, &roof) == 9) {
                    l.kind = VIS_LAYER_MAPPED;
                    strncpy(l.texture_path, diffuse, sizeof(l.texture_path) - 1);
                    l.texture_path[sizeof(l.texture_path) - 1] = '\0';
                    strncpy(l.normal_path, normal, sizeof(l.normal_path) - 1);
                    l.normal_path[sizeof(l.normal_path) - 1] = '\0';
                    strncpy(l.depth_path, depth, sizeof(l.depth_path) - 1);
                    l.depth_path[sizeof(l.depth_path) - 1] = '\0';
                    strncpy(l.position_path, position, sizeof(l.position_path) - 1);
                    l.position_path[sizeof(l.position_path) - 1] = '\0';
                    l.offset_local = Vec2{ ox, oy };
                    l.px_per_unit = (ppu > 0.0f) ? ppu : 1.0f;
                    l.z = z; l.roof_only = roof ? TRUE : FALSE;
                    out_ship->visual.layer_count++;
                    out_ship->visual.has_sprite = TRUE;
                }
            }
            continue;
        }
        f32 cx, cy;
        if (sscanf(line, "collider %f %f", &cx, &cy) == 2) {
            if (out_ship->collider_count < SHIP_MAX_COLLIDER_VERTS)
                out_ship->collider_verts[out_ship->collider_count++] = Vec2{ cx, cy };
            continue;
        }
        // hardpoint <id> <accepts> <size> <x> <y> <facing_deg> <arc_deg>
        // Ship-local coords: same art-texel space as the collider (+Y = nose).
        char hp_id[32], hp_accepts[32], hp_size[16];
        f32 hx, hy, hfacing, harc;
        if (sscanf(line, "hardpoint %31s %31s %15s %f %f %f %f",
                   hp_id, hp_accepts, hp_size, &hx, &hy, &hfacing, &harc) == 7) {
            if (out_ship->hardpoint_count >= SHIP_MAX_HARDPOINTS) {
                BS_LOG_WARN("ship_load: too many hardpoints in '%s' (max %d); '%s' skipped.",
                            path, SHIP_MAX_HARDPOINTS, hp_id);
                continue;
            }
            u32 mask = parse_module_mask(hp_accepts, path);
            HardpointSize sz;
            if (mask == 0 || !parse_hardpoint_size(hp_size, &sz)) {
                BS_LOG_WARN("ship_load: invalid hardpoint '%s' in '%s' (accepts='%s' size='%s').",
                            hp_id, path, hp_accepts, hp_size);
                continue;
            }
            HardpointDef& hp = out_ship->hardpoints[out_ship->hardpoint_count++];
            hp = HardpointDef{};
            strncpy(hp.id, hp_id, sizeof(hp.id) - 1);
            hp.id[sizeof(hp.id) - 1] = '\0';
            hp.accepts   = mask;
            hp.size      = sz;
            hp.pos_local = Vec2{ hx, hy };
            hp.facing    = hfacing * BS_DEG2RAD;
            hp.arc       = harc * BS_DEG2RAD;
            continue;
        }
    }
    fclose(f);
    out_ship->visual.size_local = vec2_scale(out_ship->visual.size_local, out_ship->world_scale);
    if (out_ship->collider_count == 0 && out_ship->size_local.x > 0.0f && out_ship->size_local.y > 0.0f) {
        f32 hw = out_ship->size_local.x * 0.5f;
        f32 hh = out_ship->size_local.y * 0.5f;
        out_ship->collider_verts[0] = Vec2{ -hw, -hh };
        out_ship->collider_verts[1] = Vec2{  hw, -hh };
        out_ship->collider_verts[2] = Vec2{  hw,  hh };
        out_ship->collider_verts[3] = Vec2{ -hw,  hh };
        out_ship->collider_count = 4;
    }
    if (out_ship->visual.layer_count <= 0) {
        BS_LOG_ERROR("ship_load: no visual layers in '%s'.", path);
        return FALSE;
    }
    // Legacy hulls without an authored skeleton get one universal center mount so weapons
    // and the point-defense can still be fitted (e.g. enemy_ship.ship).
    if (out_ship->hardpoint_count == 0) {
        HardpointDef& hp = out_ship->hardpoints[0];
        hp = HardpointDef{};
        strncpy(hp.id, "mount", sizeof(hp.id) - 1);
        hp.accepts   = MODULE_TYPE_WEAPON | MODULE_TYPE_DEFENSE;
        hp.size      = HARDPOINT_MEDIUM;
        hp.pos_local = out_ship->weapon_fire_offset_local;
        hp.facing    = 0.0f;
        hp.arc       = 2.0f * BS_PI;
        out_ship->hardpoint_count = 1;
    }
    // Turrets start parked at their rest facing (Phase 5 traverse state).
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) {
        f32 rest = (i < out_ship->hardpoint_count) ? out_ship->hardpoints[i].facing : 0.0f;
        out_ship->mount_aim[i]         = rest;
        out_ship->mount_aim_goal[i]    = rest;
        out_ship->mount_aim_engaged[i] = FALSE;
    }
    // Resolve the size class (unless authored) from the WORLD hull length, then bake the
    // class's motion tuning into the ship so piloting/autopilot read per-ship parameters.
    f32 hull_length = out_ship->size_local.y * out_ship->world_scale;
    if (!class_authored)
        out_ship->size_class = ship_size_class_from_length(hull_length);
    out_ship->motion = ship_motion_for_class(out_ship->size_class);
    ship_recompute_stats(out_ship); // sensors = baseline (no modules mounted at load)
    BS_LOG_INFO("ship_load: '%s' -> %s, %d verts, %d layers, %d hardpoints, %s-class (%.0f units).",
                path, out_ship->vessel_name, out_ship->collider_count, out_ship->visual.layer_count,
                out_ship->hardpoint_count, ship_size_class_name(out_ship->size_class), hull_length);
    return TRUE;
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
