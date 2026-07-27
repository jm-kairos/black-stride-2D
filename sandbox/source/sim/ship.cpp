// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "sim/ship.h"
#include <core/logger.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <new>
using namespace bs_math;
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
    out_ship->point_defense = DefenseLaser{};
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
    BS_LOG_INFO("ship_load: '%s' -> %s, %d verts, %d layers.",
                path, out_ship->vessel_name, out_ship->collider_count, out_ship->visual.layer_count);
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
