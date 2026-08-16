// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS globally).
#define _CRT_SECURE_NO_WARNINGS

#include "sim/skill_def.h"

#include <core/logger.h>

#include <stdio.h>
#include <string.h>

static b8 skill_targeting_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "instant") == 0) { *out = SKILL_TARGET_INSTANT; return TRUE; }
    if (strcmp(tok, "point")   == 0) { *out = SKILL_TARGET_POINT;   return TRUE; }
    if (strcmp(tok, "entity")  == 0) { *out = SKILL_TARGET_ENTITY;  return TRUE; }
    return FALSE;
}

static b8 skill_effect_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "missile_volley") == 0) { *out = SKILL_EFFECT_MISSILE_VOLLEY; return TRUE; }
    return FALSE;
}

static b8 skill_scope_from_token(const char* tok, u8* out) {
    if (strcmp(tok, "selection") == 0) { *out = SKILL_SCOPE_SELECTION; return TRUE; }
    if (strcmp(tok, "fleet")     == 0) { *out = SKILL_SCOPE_FLEET;     return TRUE; }
    return FALSE;
}

static void skill_rstrip(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

// Parse one `.skill` file into *out. Returns FALSE (with a warning) when the file is
// missing or lacks a valid id.
static b8 skill_def_load(SkillDef* out, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_WARN("skill_def_load: could not open '%s'.", path);
        return FALSE;
    }
    *out = SkillDef{};
    // Defaults are the v1 skill's own numbers: a card that only names itself is a
    // selection-scoped, entity-targeted missile volley.
    out->targeting = SKILL_TARGET_ENTITY;
    out->effect    = SKILL_EFFECT_MISSILE_VOLLEY;
    out->scope     = SKILL_SCOPE_SELECTION;
    out->cooldown  = 45.0f;
    out->stagger   = 0.12f;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        skill_rstrip(line);
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
        if (sscanf(line, "icon %15s", out->icon) == 1) continue;
        if (sscanf(line, "targeting %31s", tok) == 1) {
            if (!skill_targeting_from_token(tok, &out->targeting))
                BS_LOG_WARN("skill_def_load: unknown targeting '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "effect %31s", tok) == 1) {
            if (!skill_effect_from_token(tok, &out->effect))
                BS_LOG_WARN("skill_def_load: unknown effect '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "scope %31s", tok) == 1) {
            if (!skill_scope_from_token(tok, &out->scope))
                BS_LOG_WARN("skill_def_load: unknown scope '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "cooldown %f", &out->cooldown) == 1) continue;
        if (sscanf(line, "stagger %f",  &out->stagger)  == 1) continue;
    }
    fclose(f);
    if (out->id[0] == '\0') {
        BS_LOG_WARN("skill_def_load: '%s' is missing a valid id; skipped.", path);
        return FALSE;
    }
    if (out->name[0] == '\0') {
        strncpy(out->name, out->id, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    if (out->icon[0] == '\0') {
        strncpy(out->icon, "ic-missile", sizeof(out->icon) - 1);
        out->icon[sizeof(out->icon) - 1] = '\0';
    }
    return TRUE;
}

b8 skill_registry_load(SkillRegistry* reg, const char* manifest_path) {
    if (!reg || !manifest_path) return FALSE;
    reg->count = 0;
    FILE* f = fopen(manifest_path, "rb");
    if (!f) {
        BS_LOG_ERROR("skill_registry_load: could not open manifest '%s'.", manifest_path);
        return FALSE;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        skill_rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (reg->count >= SKILL_REGISTRY_MAX) {
            BS_LOG_WARN("skill_registry_load: registry full (max %d); '%s' skipped.",
                        SKILL_REGISTRY_MAX, line);
            continue;
        }
        if (skill_def_load(&reg->defs[reg->count], line))
            reg->count++;
    }
    fclose(f);
    BS_LOG_INFO("skill_registry_load: %d skills loaded from '%s'.", reg->count, manifest_path);
    return TRUE;
}

const SkillDef* skill_registry_find(const SkillRegistry* reg, const char* id) {
    if (!reg || !id) return nullptr;
    for (i32 i = 0; i < reg->count; ++i)
        if (strcmp(reg->defs[i].id, id) == 0) return &reg->defs[i];
    return nullptr;
}
