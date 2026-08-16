// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS globally).
#define _CRT_SECURE_NO_WARNINGS

#include "sim/weapon_def.h"

#include <core/logger.h>
#include <renderer/renderer.h>  // renderer_load_texture (mount-art resolution)

#include <stdio.h>
#include <string.h>

static b8 weapon_kind_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "ballistic") == 0) { *out = WEAPON_KIND_BALLISTIC; return TRUE; }
    if (strcmp(tok, "missile")   == 0) { *out = WEAPON_KIND_MISSILE;   return TRUE; }
    return FALSE;
}

static b8 weapon_muzzle_pattern_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "sequential") == 0) { *out = MUZZLE_SEQUENTIAL; return TRUE; }
    if (strcmp(tok, "salvo")      == 0) { *out = MUZZLE_SALVO;      return TRUE; }
    return FALSE;
}

static b8 weapon_vfx_family_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "shell")    == 0) { *out = VFX_SHELL;    return TRUE; }
    if (strcmp(tok, "slug")     == 0) { *out = VFX_SLUG;     return TRUE; }
    if (strcmp(tok, "ordnance") == 0) { *out = VFX_ORDNANCE; return TRUE; }
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
    // 0 = unauthored: ballistics resolve to their flight reach, missiles to the clamped
    // default -- both inside weapon_engage_range, the one place that decision lives.
    out->engage_range = 0.0f;
    // Cold-launch defaults: every missile ejects and coasts unless its card says otherwise
    // (ignition_delay 0 authors the legacy hot launch). Ballistics ignore both.
    out->eject_speed    = 300.0f;
    out->ignition_delay = 0.5f;
    out->price       = 0;
    out->tier        = 1;
    // 0xFF means "not authored". VFX_SHELL is 0, so a zeroed struct is indistinguishable from
    // an explicit `vfx_family shell` -- and the default has to be resolved from `kind`, which
    // may appear anywhere in the file. Hence a sentinel now and the real default after parsing.
    out->vfx_family  = 0xFF;
    // Mount-art geometry defaults, only ever read when a `mount_art` path is present. The
    // 3.0 height matches the procedural turret's own axial footprint (base plate through
    // muzzle block, ~2.95 half-extents), so art and rectangles occupy the same slot.
    out->mount_art_w     = 1.85f;
    out->mount_art_h     = 3.00f;
    out->mount_art_pivot = 0.0f;
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
        if (strncmp(line, "desc", 4) == 0) {
            const char* start = line + 4;
            while (*start == ' ' || *start == '\t') ++start;
            if (*start == '"') {
                ++start;
                char* end = (char*)strchr(start, '"');
                if (end) *end = '\0';
            }
            strncpy(out->desc, start, sizeof(out->desc) - 1);
            out->desc[sizeof(out->desc) - 1] = '\0';
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
        // The two suffixed mount_art keys MUST be tested before the bare one: "mount_art %s"
        // happily matches "mount_art_size ..." and would capture "_size" as the path.
        if (sscanf(line, "mount_art_size %f %f", &out->mount_art_w, &out->mount_art_h) == 2) continue;
        if (sscanf(line, "mount_art_pivot %f", &out->mount_art_pivot) == 1) continue;
        if (sscanf(line, "mount_art %127s", out->mount_art) == 1) continue;
        // Suffixed key first, as with mount_art above. Here it is only convention -- "%f"
        // cannot eat "_pattern" the way "%s" ate "_size" -- but the fall-through for an
        // unmatched line is silent, so keeping the order uniform is what stops the next
        // `muzzle_*` key from being quietly swallowed.
        if (sscanf(line, "vfx_family %31s", tok) == 1) {
            if (!weapon_vfx_family_from_token(tok, &out->vfx_family))
                BS_LOG_WARN("weapon_def_load: unknown vfx_family '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "muzzle_pattern %31s", tok) == 1) {
            if (!weapon_muzzle_pattern_from_token(tok, &out->muzzle_pattern))
                BS_LOG_WARN("weapon_def_load: unknown muzzle_pattern '%s' in '%s'.", tok, path);
            continue;
        }
        {   // Repeatable: each `muzzle <right> <forward>` line appends one barrel.
            f32 mx = 0.0f, my = 0.0f;
            if (sscanf(line, "muzzle %f %f", &mx, &my) == 2) {
                if (out->muzzle_count < WEAPON_MAX_MUZZLES)
                    out->muzzles[out->muzzle_count++] = bs_math::Vec2{ mx, my };
                else
                    BS_LOG_WARN("weapon_def_load: '%s' authors more than %d muzzles; extra dropped.",
                                path, WEAPON_MAX_MUZZLES);
                continue;
            }
        }
        if (sscanf(line, "damage %f",      &out->damage)      == 1) continue;
        if (sscanf(line, "fire_rate %f",   &out->fire_rate)   == 1) continue;
        if (sscanf(line, "reload %f",      &out->reload)      == 1) continue;
        if (sscanf(line, "proj_speed %f",  &out->proj_speed)  == 1) continue;
        if (sscanf(line, "proj_life %f",   &out->proj_life)   == 1) continue;
        if (sscanf(line, "proj_radius %f", &out->proj_radius) == 1) continue;
        if (sscanf(line, "cap_cost %f",    &out->cap_cost)    == 1) continue;
        if (sscanf(line, "emission %f",    &out->emission)    == 1) continue;
        if (sscanf(line, "proj_hp %f",     &out->proj_hp)     == 1) continue;
        if (sscanf(line, "engage_range %f", &out->engage_range) == 1) continue;
        if (sscanf(line, "eject_speed %f",  &out->eject_speed)  == 1) continue;
        if (sscanf(line, "ignition_delay %f", &out->ignition_delay) == 1) continue;
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
    // Resolve the visual family last, once `kind` is final. Guided ordnance burns an engine and
    // detonates; a ballistic round is inert and punches through. That mapping is right for five
    // of the six catalog weapons with nothing authored -- only the railgun opts out.
    if (out->vfx_family == 0xFF)
        out->vfx_family = (out->kind == WEAPON_KIND_MISSILE) ? VFX_ORDNANCE : VFX_SHELL;
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

void weapon_registry_resolve_textures(WeaponRegistry* reg) {
    if (!reg) return;
    for (i32 i = 0; i < reg->count; ++i) {
        WeaponDef& d = reg->defs[i];
        if (d.mount_art[0] == '\0') continue;   // no art authored: keeps the procedural mount
        d.mount_art_tex = renderer_load_texture(d.mount_art);
        // A failed load leaves id at BS_INVALID_HANDLE, which the draw site tests before
        // committing to the sprite -- so the mount falls back to the rectangles rather than
        // rendering the engine's 1x1 white stand-in.
        if (!d.mount_art_tex.id)
            BS_LOG_WARN("weapon_registry_resolve_textures: '%s' failed to load mount art '%s'; "
                        "falling back to procedural art.", d.id, d.mount_art);
    }
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
        ml->eject_speed    = def->eject_speed;
        ml->ignition_delay = def->ignition_delay;
        w = ml;
    } else {
        BallisticWeapon* bw = new BallisticWeapon(def->name, def->fire_rate, def->proj_speed,
                                                  def->proj_life, def->proj_radius, def->emission);
        bw->cap_cost_value = def->cap_cost;
        w = bw;
    }
    // name/icon point INTO the def's fixed-pool storage (never reallocated); `def` gives
    // consumers (stat cards) access to the full stat block incl. desc/price/tier.
    w->def           = def;
    w->icon          = def->icon;
    w->size          = (u8)def->size;
    w->damage        = def->damage;
    w->owner_faction = owner;
    return w;
}
