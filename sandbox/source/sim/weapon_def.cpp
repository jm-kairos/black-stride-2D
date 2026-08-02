// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS globally).
#define _CRT_SECURE_NO_WARNINGS

#include "sim/weapon_def.h"

#include <core/logger.h>

#include <stdio.h>
#include <string.h>

static b8 weapon_kind_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "ballistic") == 0) { *out = WEAPON_KIND_BALLISTIC; return TRUE; }
    if (strcmp(tok, "missile")   == 0) { *out = WEAPON_KIND_MISSILE;   return TRUE; }
    return FALSE;
}

static b8 weapon_size_from_token(const char* tok, HardpointSize* out) {
    if (strcmp(tok, "S") == 0) { *out = HARDPOINT_SMALL;  return TRUE; }
    if (strcmp(tok, "M") == 0) { *out = HARDPOINT_MEDIUM; return TRUE; }
    if (strcmp(tok, "L") == 0) { *out = HARDPOINT_LARGE;  return TRUE; }
    return FALSE;
}

static void weapon_rstrip(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

// Parse one `.weapon` file into *out. Returns FALSE (with a warning) when the file is
// missing or lacks a valid id.
static b8 weapon_def_load(WeaponDef* out, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_WARN("weapon_def_load: could not open '%s'.", path);
        return FALSE;
    }
    *out = WeaponDef{};
    // Archetype-neutral defaults: a def that only names itself still shoots like the
    // baseline gauss cannon (see docs/POINT_DEFENSE_AND_MISSILES.md tuning).
    out->kind        = WEAPON_KIND_BALLISTIC;
    out->size        = HARDPOINT_MEDIUM;
    out->damage      = 15.0f;
    out->fire_rate   = 5.0f;
    out->reload      = 4.0f;
    out->proj_speed  = 12000.0f;
    out->proj_life   = 20.0f;
    out->proj_radius = 4.0f;
    out->cap_cost    = 4.0f;
    out->emission    = 0.6f;
    out->proj_hp     = 1.0f;
    out->price       = 0;
    out->tier        = 1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        weapon_rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char tok[32];
        if (sscanf(line, "id %31s", out->id) == 1) continue;
        if (strncmp(line, "name", 4) == 0) {
            const char* start = line + 4;
            while (*start == ' ' || *start == '\t') ++start;
            if (*start == '"') {
                ++start;
                char* end = (char*)strchr(start, '"');
                if (end) *end = '\0';
            }
            strncpy(out->name, start, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            continue;
        }
        if (sscanf(line, "kind %31s", tok) == 1) {
            if (!weapon_kind_from_token(tok, &out->kind))
                BS_LOG_WARN("weapon_def_load: unknown kind '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "size %31s", tok) == 1) {
            if (!weapon_size_from_token(tok, &out->size))
                BS_LOG_WARN("weapon_def_load: unknown size '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "icon %15s", out->icon) == 1) continue;
        if (sscanf(line, "damage %f",      &out->damage)      == 1) continue;
        if (sscanf(line, "fire_rate %f",   &out->fire_rate)   == 1) continue;
        if (sscanf(line, "reload %f",      &out->reload)      == 1) continue;
        if (sscanf(line, "proj_speed %f",  &out->proj_speed)  == 1) continue;
        if (sscanf(line, "proj_life %f",   &out->proj_life)   == 1) continue;
        if (sscanf(line, "proj_radius %f", &out->proj_radius) == 1) continue;
        if (sscanf(line, "cap_cost %f",    &out->cap_cost)    == 1) continue;
        if (sscanf(line, "emission %f",    &out->emission)    == 1) continue;
        if (sscanf(line, "proj_hp %f",     &out->proj_hp)     == 1) continue;
        if (sscanf(line, "price %d",       &out->price)       == 1) continue;
        if (sscanf(line, "tier %d",        &out->tier)        == 1) continue;
    }
    fclose(f);
    if (out->id[0] == '\0') {
        BS_LOG_WARN("weapon_def_load: '%s' is missing a valid id; skipped.", path);
        return FALSE;
    }
    if (out->name[0] == '\0') {
        strncpy(out->name, out->id, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    if (out->icon[0] == '\0') {
        strncpy(out->icon, (out->kind == WEAPON_KIND_MISSILE) ? "ic-missile" : "ic-cannon",
                sizeof(out->icon) - 1);
        out->icon[sizeof(out->icon) - 1] = '\0';
    }
    return TRUE;
}

b8 weapon_registry_load(WeaponRegistry* reg, const char* manifest_path) {
    if (!reg || !manifest_path) return FALSE;
    reg->count = 0;
    FILE* f = fopen(manifest_path, "rb");
    if (!f) {
        BS_LOG_ERROR("weapon_registry_load: could not open manifest '%s'.", manifest_path);
        return FALSE;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        weapon_rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (reg->count >= WEAPON_REGISTRY_MAX) {
            BS_LOG_WARN("weapon_registry_load: registry full (max %d); '%s' skipped.",
                        WEAPON_REGISTRY_MAX, line);
            continue;
        }
        if (weapon_def_load(&reg->defs[reg->count], line))
            reg->count++;
    }
    fclose(f);
    BS_LOG_INFO("weapon_registry_load: %d weapons loaded from '%s'.", reg->count, manifest_path);
    return TRUE;
}

const WeaponDef* weapon_registry_find(const WeaponRegistry* reg, const char* id) {
    if (!reg || !id) return nullptr;
    for (i32 i = 0; i < reg->count; ++i)
        if (strcmp(reg->defs[i].id, id) == 0) return &reg->defs[i];
    return nullptr;
}

Weapon* weapon_instantiate(const WeaponDef* def, VesselFaction owner) {
    if (!def) return nullptr;
    Weapon* w = nullptr;
    if (def->kind == WEAPON_KIND_MISSILE) {
        MissileLauncher* ml = new MissileLauncher(def->name, def->reload, def->proj_speed,
                                                  def->proj_life, def->proj_radius,
                                                  def->emission, def->proj_hp);
        ml->cap_cost_value = def->cap_cost;
        w = ml;
    } else {
        BallisticWeapon* bw = new BallisticWeapon(def->name, def->fire_rate, def->proj_speed,
                                                  def->proj_life, def->proj_radius, def->emission);
        bw->cap_cost_value = def->cap_cost;
        w = bw;
    }
    // name/icon point INTO the def's fixed-pool storage (never reallocated).
    w->icon          = def->icon;
    w->size          = (u8)def->size;
    w->damage        = def->damage;
    w->owner_faction = owner;
    return w;
}
