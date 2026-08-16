// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "sim/ship_def.h"
#include <core/logger.h>
#include <stdio.h>
#include <stdlib.h>   // strtof (speed_limits list parsing)
#include <string.h>
#include <math.h>
#include <new>
using namespace bs_math;

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
        else BS_LOG_WARN("ship_def_load: unknown module kind '%s' in '%s'.", tok, path);
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
// Copy a quoted-or-bare string value ("name", "desc") into a fixed buffer. `line`
// points AT the key; the value starts after it.
static void parse_string_value(char* line, i32 key_len, char* out, i32 out_size) {
    char* start = line + key_len;
    while (*start == ' ' || *start == '\t') ++start;
    if (*start == '"') { ++start; char* end = strchr(start, '"'); if (end) *end = '\0'; }
    strncpy(out, start, out_size - 1);
    out[out_size - 1] = '\0';
}

// Parse one `.ship` file into *out. Returns FALSE (with a log line saying why) when the
// file is missing, lacks a valid id, or authors no visual layers.
static b8 ship_def_load(ShipDef* out, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_WARN("ship_def_load: could not open '%s'.", path);
        return FALSE;
    }
    *out = ShipDef{};   // value-init: SensorSuite/HardpointDef defaults apply
    out->faction        = VESSEL_NEUTRAL;
    out->world_scale    = 1.0f;
    // Stat defaults are the values the game used before each line existed, so a card
    // that authors nothing behaves exactly like a pre-card hull.
    out->cap_max_base   = 100.0f;
    out->cap_regen_base = 8.0f;
    out->hull_max_hp    = 100.0f;
    out->size_class     = SHIP_CLASS_DRONE;
    out->tier           = 1;
    // Doctrine sentinels: a card that authors nothing keeps the runtime defaults.
    out->doct_roe       = 0xFF;
    out->doct_flak      = 0xFF;
    out->doct_missile   = 0xFF;
    out->doct_cap_floor = -1.0f;
    ship_visual_clear(&out->visual);
    b8 class_authored = FALSE;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (sscanf(line, "id %31s", out->id) == 1) continue;
        if (strncmp(line, "name", 4) == 0) {
            parse_string_value(line, 4, out->name, sizeof(out->name));
            continue;
        }
        if (strncmp(line, "desc", 4) == 0) {
            parse_string_value(line, 4, out->desc, sizeof(out->desc));
            continue;
        }
        int faction_id;
        if (sscanf(line, "faction %d", &faction_id) == 1) {
            if (faction_id >= 0 && faction_id < 5) out->faction = (VesselFaction)faction_id;
            continue;
        }
        f32 ws;
        if (sscanf(line, "world_scale %f", &ws) == 1 && ws > 0.0f) {
            out->world_scale = ws;
            continue;
        }
        f32 cmax, cregen;
        if (sscanf(line, "capacitor %f %f", &cmax, &cregen) == 2 && cmax > 0.0f && cregen >= 0.0f) {
            out->cap_max_base   = cmax;
            out->cap_regen_base = cregen;
            continue;
        }
        f32 hull;
        if (sscanf(line, "hull %f", &hull) == 1) {
            if (hull > 0.0f) { out->hull_max_hp = hull; out->hull_authored = TRUE; }
            else BS_LOG_WARN("ship_def_load: invalid hull '%f' in '%s' (must be > 0).", hull, path);
            continue;
        }
        // ---- Authored engagement doctrine (Phase 3) -- optional temperament lines --------
        char word[16];
        if (sscanf(line, "roe %15s", word) == 1) {
            if      (strcmp(word, "free")   == 0) out->doct_roe = 0;
            else if (strcmp(word, "return") == 0) out->doct_roe = 1;
            else if (strcmp(word, "hold")   == 0) out->doct_roe = 2;
            else BS_LOG_WARN("ship_def_load: unknown roe '%s' in '%s' (free|return|hold).", word, path);
            continue;
        }
        if (sscanf(line, "flak %15s", word) == 1) {
            if      (strcmp(word, "auto")   == 0) out->doct_flak = 0;
            else if (strcmp(word, "manual") == 0) out->doct_flak = 1;
            else BS_LOG_WARN("ship_def_load: unknown flak '%s' in '%s' (auto|manual).", word, path);
            continue;
        }
        if (sscanf(line, "missiles %15s", word) == 1) {
            if      (strcmp(word, "free")     == 0) out->doct_missile = 0;
            else if (strcmp(word, "conserve") == 0) out->doct_missile = 1;
            else if (strcmp(word, "hold")     == 0) out->doct_missile = 2;
            else BS_LOG_WARN("ship_def_load: unknown missiles '%s' in '%s' (free|conserve|hold).", word, path);
            continue;
        }
        f32 cfl;
        if (sscanf(line, "cap_floor %f", &cfl) == 1) {
            if (cfl >= 0.0f && cfl <= 0.9f) out->doct_cap_floor = cfl;
            else BS_LOG_WARN("ship_def_load: invalid cap_floor '%f' in '%s' (0..0.9).", cfl, path);
            continue;
        }
        ShipMotion mo;
        if (sscanf(line, "motion %f %f %f %f %f",
                   &mo.max_speed, &mo.accel, &mo.decel, &mo.turn_accel, &mo.max_turn) == 5) {
            if (mo.max_speed > 0.0f && mo.accel > 0.0f && mo.decel > 0.0f &&
                mo.turn_accel > 0.0f && mo.max_turn > 0.0f) {
                out->motion          = mo;
                out->motion_authored = TRUE;
            } else {
                BS_LOG_WARN("ship_def_load: invalid motion line in '%s' (all five values must be "
                            "> 0); using the class table.", path);
            }
            continue;
        }
        if (strncmp(line, "speed_limits", 12) == 0) {
            f32 v[SHIP_MAX_SPEED_LIMITS];
            i32 n = 0;
            const char* p = line + 12;
            char* endp = nullptr;
            while (n < SHIP_MAX_SPEED_LIMITS) {
                f32 val = strtof(p, &endp);
                if (endp == p) break;
                v[n++] = val;
                p = endp;
            }
            // 5..8 strictly ascending positive values; anything else keeps the derived list.
            b8 ok = (n >= SHIP_MIN_SPEED_LIMITS);
            for (i32 k = 0; ok && k < n; ++k)
                if (v[k] <= 0.0f || (k > 0 && v[k] <= v[k - 1])) ok = FALSE;
            if (ok) {
                for (i32 k = 0; k < n; ++k) out->speed_limits[k] = v[k];
                out->speed_limit_count = n;
            } else {
                BS_LOG_WARN("ship_def_load: invalid speed_limits line in '%s' (need %d..%d "
                            "strictly ascending values > 0); deriving from motion.",
                            path, SHIP_MIN_SPEED_LIMITS, SHIP_MAX_SPEED_LIMITS);
            }
            continue;
        }
        f32 l0, l1, l2;
        if (sscanf(line, "sensors %f %f %f", &l0, &l1, &l2) == 3) {
            // The suite is strictly ordered (l0 < l1 < l2) -- the same invariant the editor
            // enforces -- so a card cannot author an inverted suite into the game.
            if (l0 > 0.0f && l0 < l1 && l1 < l2) {
                out->sensors_base.layer0_radius = l0;
                out->sensors_base.layer1_radius = l1;
                out->sensors_base.layer2_radius = l2;
            } else {
                BS_LOG_WARN("ship_def_load: invalid sensors line in '%s' (need 0 < l0 < l1 < l2); "
                            "using the default suite.", path);
            }
            continue;
        }
        if (sscanf(line, "price %d", &out->price) == 1) continue;
        if (sscanf(line, "tier %d",  &out->tier)  == 1) continue;
        f32 sw, sh;
        if (sscanf(line, "size %f %f", &sw, &sh) == 2) {
            out->size_local        = Vec2{ sw, sh };
            out->visual.size_local = Vec2{ sw, sh };
            continue;
        }
        char classbuf[16];
        if (sscanf(line, "class %15s", classbuf) == 1) {
            b8 matched = FALSE;
            for (i32 c = 0; c < SHIP_CLASS_COUNT; ++c) {
                if (_stricmp(classbuf, ship_size_class_name((ShipSizeClass)c)) == 0) {
                    out->size_class = (ShipSizeClass)c;
                    class_authored = TRUE;
                    matched = TRUE;
                    break;
                }
            }
            if (!matched)
                BS_LOG_WARN("ship_def_load: unknown class '%s' in '%s' (deriving from size).", classbuf, path);
            continue;
        }
        char kindbuf[16];
        if (sscanf(line, "layer %15s", kindbuf) == 1) {
            if (out->visual.layer_count >= VIS_MAX_LAYERS) continue;
            VisualLayer& l = out->visual.layers[out->visual.layer_count];
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
                    out->visual.layer_count++;
                    out->visual.has_sprite = TRUE;
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
                    out->visual.layer_count++;
                    out->visual.has_sprite = TRUE;
                }
            }
            continue;
        }
        f32 cx, cy;
        if (sscanf(line, "collider %f %f", &cx, &cy) == 2) {
            if (out->collider_count < SHIP_MAX_COLLIDER_VERTS)
                out->collider_verts[out->collider_count++] = Vec2{ cx, cy };
            continue;
        }
        // hardpoint <id> <accepts> <size> <x> <y> <facing_deg> <arc_deg> [art_scale]
        // Ship-local coords: same art-texel space as the collider (+Y = nose).
        //
        // art_scale is OPTIONAL and trailing, so every existing 7-field line keeps working and
        // means exactly what it did. sscanf reports how many fields it converted, which is what
        // distinguishes "omitted" from "present"; an absent scale leaves the struct's own 1.0
        // default rather than a zero that would draw the mount at nothing.
        char hp_id[32], hp_accepts[32], hp_size[16];
        f32 hx, hy, hfacing, harc, hscale = 1.0f;
        i32 hp_fields = sscanf(line, "hardpoint %31s %31s %15s %f %f %f %f %f",
                               hp_id, hp_accepts, hp_size, &hx, &hy, &hfacing, &harc, &hscale);
        if (hp_fields == 7 || hp_fields == 8) {
            if (out->hardpoint_count >= SHIP_MAX_HARDPOINTS) {
                BS_LOG_WARN("ship_def_load: too many hardpoints in '%s' (max %d); '%s' skipped.",
                            path, SHIP_MAX_HARDPOINTS, hp_id);
                continue;
            }
            u32 mask = parse_module_mask(hp_accepts, path);
            HardpointSize sz;
            if (mask == 0 || !parse_hardpoint_size(hp_size, &sz)) {
                BS_LOG_WARN("ship_def_load: invalid hardpoint '%s' in '%s' (accepts='%s' size='%s').",
                            hp_id, path, hp_accepts, hp_size);
                continue;
            }
            HardpointDef& hp = out->hardpoints[out->hardpoint_count++];
            hp = HardpointDef{};
            strncpy(hp.id, hp_id, sizeof(hp.id) - 1);
            hp.id[sizeof(hp.id) - 1] = '\0';
            hp.accepts   = mask;
            hp.size      = sz;
            hp.pos_local = Vec2{ hx, hy };
            hp.facing    = hfacing * BS_DEG2RAD;
            hp.arc       = harc * BS_DEG2RAD;
            // Clamped rather than trusted: a zero or negative scale would make the mount
            // vanish, and a stray large one would swallow the hull.
            hp.art_scale = (hp_fields == 8) ? clampf(hscale, 0.05f, 4.0f) : 1.0f;
            continue;
        }
        // thruster <main|rcs> <x> <y> <facing_deg> [size]
        // Same art-texel space and facing convention as hardpoints; `facing` is the
        // direction the EXHAUST leaves the hull. The trailing size is optional exactly
        // like a hardpoint's art_scale, and clamped for the same reason.
        char th_kind[16];
        f32 tx, ty, tfacing, tsize = 1.0f;
        i32 th_fields = sscanf(line, "thruster %15s %f %f %f %f",
                               th_kind, &tx, &ty, &tfacing, &tsize);
        if (th_fields == 4 || th_fields == 5) {
            if (out->thruster_count >= SHIP_MAX_THRUSTERS) {
                BS_LOG_WARN("ship_def_load: too many thrusters in '%s' (max %d); skipped.",
                            path, SHIP_MAX_THRUSTERS);
                continue;
            }
            u8 kind;
            if      (_stricmp(th_kind, "main") == 0) kind = THRUSTER_MAIN;
            else if (_stricmp(th_kind, "rcs")  == 0) kind = THRUSTER_RCS;
            else {
                BS_LOG_WARN("ship_def_load: unknown thruster kind '%s' in '%s'; skipped.",
                            th_kind, path);
                continue;
            }
            ThrusterDef& th = out->thrusters[out->thruster_count++];
            th = ThrusterDef{};
            th.kind      = kind;
            th.pos_local = Vec2{ tx, ty };
            th.facing    = tfacing * BS_DEG2RAD;
            th.size      = (th_fields == 5) ? clampf(tsize, 0.05f, 4.0f) : 1.0f;
            continue;
        }
    }
    fclose(f);
    if (out->id[0] == '\0') {
        BS_LOG_WARN("ship_def_load: '%s' is missing a valid id; skipped.", path);
        return FALSE;
    }
    if (out->name[0] == '\0') {
        strncpy(out->name, out->id, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    out->visual.size_local = vec2_scale(out->visual.size_local, out->world_scale);
    if (out->collider_count == 0 && out->size_local.x > 0.0f && out->size_local.y > 0.0f) {
        f32 hw = out->size_local.x * 0.5f;
        f32 hh = out->size_local.y * 0.5f;
        out->collider_verts[0] = Vec2{ -hw, -hh };
        out->collider_verts[1] = Vec2{  hw, -hh };
        out->collider_verts[2] = Vec2{  hw,  hh };
        out->collider_verts[3] = Vec2{ -hw,  hh };
        out->collider_count = 4;
    }
    if (out->visual.layer_count <= 0) {
        BS_LOG_ERROR("ship_def_load: no visual layers in '%s'.", path);
        return FALSE;
    }
    // Legacy hulls without an authored skeleton get one universal center mount so weapons
    // and the point-defense can still be fitted (e.g. the raider).
    if (out->hardpoint_count == 0) {
        HardpointDef& hp = out->hardpoints[0];
        hp = HardpointDef{};
        strncpy(hp.id, "mount", sizeof(hp.id) - 1);
        hp.accepts   = MODULE_TYPE_WEAPON | MODULE_TYPE_DEFENSE;
        hp.size      = HARDPOINT_MEDIUM;
        hp.pos_local = Vec2{ 0.0f, 0.0f };
        hp.facing    = 0.0f;
        hp.arc       = 2.0f * BS_PI;
        out->hardpoint_count = 1;
    }
    // Resolve the size class (unless authored) from the WORLD hull length, then the motion
    // tuning (unless authored) from the class table, so instances copy final values.
    f32 hull_length = out->size_local.y * out->world_scale;
    if (!class_authored)
        out->size_class = ship_size_class_from_length(hull_length);
    if (!out->motion_authored)
        out->motion = ship_motion_for_class(out->size_class);
    // Speed-limit gears (unless authored) derive from the RESOLVED motion cap, so every
    // hull gets a governor without a card edit. The 1.0x gear equals motion.max_speed --
    // the instance's default selection -- so an untouched ship flies exactly as before.
    if (out->speed_limit_count == 0) {
        const f32 mul[SHIP_MIN_SPEED_LIMITS] = { 0.5f, 1.0f, 2.5f, 6.0f, 15.0f };
        for (i32 k = 0; k < SHIP_MIN_SPEED_LIMITS; ++k)
            out->speed_limits[k] = out->motion.max_speed * mul[k];
        out->speed_limit_count = SHIP_MIN_SPEED_LIMITS;
    }
    return TRUE;
}

b8 ship_registry_load(ShipRegistry* reg, const char* manifest_path) {
    if (!reg || !manifest_path) return FALSE;
    reg->count = 0;
    FILE* f = fopen(manifest_path, "rb");
    if (!f) {
        BS_LOG_ERROR("ship_registry_load: could not open manifest '%s'.", manifest_path);
        return FALSE;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (reg->count >= SHIP_REGISTRY_MAX) {
            BS_LOG_WARN("ship_registry_load: registry full (max %d); '%s' skipped.",
                        SHIP_REGISTRY_MAX, line);
            continue;
        }
        ShipDef* d = &reg->defs[reg->count];
        if (!ship_def_load(d, line)) continue;
        // Two files claiming one id would make ship_registry_find answer differently from
        // the manifest order; keep the first and say so.
        if (ship_registry_find(reg, d->id)) {
            BS_LOG_WARN("ship_registry_load: duplicate id '%s' from '%s'; keeping the first.",
                        d->id, line);
            continue;
        }
        reg->count++;
    }
    fclose(f);
    BS_LOG_INFO("ship_registry_load: %d hulls loaded from '%s'.", reg->count, manifest_path);
    return TRUE;
}

void ship_registry_resolve_textures(ShipRegistry* reg) {
    if (!reg) return;
    for (i32 i = 0; i < reg->count; ++i)
        ship_visual_resolve_textures(&reg->defs[i].visual);
}

const ShipDef* ship_registry_find(const ShipRegistry* reg, const char* id) {
    if (!reg || !id) return nullptr;
    for (i32 i = 0; i < reg->count; ++i)
        if (strcmp(reg->defs[i].id, id) == 0) return &reg->defs[i];
    return nullptr;
}

b8 ship_instantiate(const ShipDef* def, Ship* out_ship) {
    if (!def || !out_ship) {
        BS_LOG_ERROR("ship_instantiate: null def or ship.");
        return FALSE;
    }
    new (out_ship) Ship();
    out_ship->def          = def;
    out_ship->world_scale  = def->world_scale;
    out_ship->size_class   = def->size_class;
    out_ship->motion       = def->motion;
    // Default gear: the highest limit not exceeding the authored motion cap, so an
    // untouched instance keeps its pre-gear cruise speed until the player shifts.
    out_ship->speed_limit_sel = 0;
    for (i32 k = 0; k < def->speed_limit_count; ++k)
        if (def->speed_limits[k] <= def->motion.max_speed + 0.001f)
            out_ship->speed_limit_sel = k;
    out_ship->origin       = HierPos2{};
    out_ship->angle        = 0.0f;
    out_ship->faction      = def->faction;
    // Points into the registry's fixed-pool storage, which never reallocates and outlives
    // every ship (it lives in game_state) -- the same argument weapon name/icon make.
    out_ship->vessel_name  = def->name;
    out_ship->visual       = def->visual;   // resolved handles come along when the def has them
    out_ship->size_local   = def->size_local;
    out_ship->collider_count = def->collider_count;
    for (i32 i = 0; i < def->collider_count; ++i)
        out_ship->collider_verts[i] = def->collider_verts[i];
    out_ship->hardpoint_count = def->hardpoint_count;
    for (i32 i = 0; i < def->hardpoint_count; ++i)
        out_ship->hardpoints[i] = def->hardpoints[i];
    out_ship->weapon_fire_offset_local = Vec2{ 0.0f, 0.0f };
    out_ship->sensors      = def->sensors_base;
    out_ship->sensors_base = def->sensors_base;
    out_ship->cap_max_base   = def->cap_max_base;
    out_ship->cap_regen_base = def->cap_regen_base;
    out_ship->hull_max_hp    = def->hull_max_hp;
    out_ship->point_defense  = DefenseLaser{};
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) out_ship->mounts[i] = nullptr;
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) out_ship->module_mounts[i] = nullptr;
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) out_ship->mount_groups[i] = 0;
    out_ship->active_group        = 0;
    out_ship->weapon_override     = -1;   // "All": fire the active group (default behaviour)
    out_ship->active_weapon_idx   = -1;
    out_ship->weapon_stash_count  = 0;
    out_ship->module_stash_count  = 0;
    out_ship->point_defense_mount = -1;
    // Turrets start parked at their rest facing.
    for (i32 i = 0; i < SHIP_MAX_HARDPOINTS; ++i) {
        f32 rest = (i < out_ship->hardpoint_count) ? out_ship->hardpoints[i].facing : 0.0f;
        out_ship->mount_aim[i]         = rest;
        out_ship->mount_aim_goal[i]    = rest;
        out_ship->mount_aim_engaged[i] = FALSE;
    }
    ship_recompute_stats(out_ship); // sensors/capacitor = baseline (no modules mounted yet)
    out_ship->cap_current = out_ship->cap_max;   // spawn with a full bank
    BS_LOG_INFO("ship_instantiate: '%s' -> %s, %d verts, %d layers, %d hardpoints, %s-class "
                "(%.0f units, %.0f hull).",
                def->id, def->name, out_ship->collider_count, out_ship->visual.layer_count,
                out_ship->hardpoint_count, ship_size_class_name(out_ship->size_class),
                out_ship->size_local.y * out_ship->world_scale, out_ship->hull_max_hp);
    return TRUE;
}
