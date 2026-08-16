// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS globally).
#define _CRT_SECURE_NO_WARNINGS

#include "sim/module.h"

#include <core/logger.h>

#include <stdio.h>
#include <string.h>

// Single module-kind token -> MODULE_TYPE_* bit (a module IS one kind; hardpoints may
// accept several, so only their accepts-mask uses '|' syntax).
static u32 module_type_from_token(const char* tok) {
    if (strcmp(tok, "weapon")  == 0) return MODULE_TYPE_WEAPON;
    if (strcmp(tok, "defense") == 0) return MODULE_TYPE_DEFENSE;
    if (strcmp(tok, "sensor")  == 0) return MODULE_TYPE_SENSOR;
    if (strcmp(tok, "engine")  == 0) return MODULE_TYPE_ENGINE;
    if (strcmp(tok, "utility") == 0) return MODULE_TYPE_UTILITY;
    return 0;
}

static b8 module_size_from_token(const char* tok, HardpointSize* out) {
    if (strcmp(tok, "S") == 0) { *out = HARDPOINT_SMALL;  return TRUE; }
    if (strcmp(tok, "M") == 0) { *out = HARDPOINT_MEDIUM; return TRUE; }
    if (strcmp(tok, "L") == 0) { *out = HARDPOINT_LARGE;  return TRUE; }
    return FALSE;
}

static void module_rstrip(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

// Parse one `.module` file into *out. Returns FALSE (with a warning) when the file is
// missing or lacks a valid id/type.
static b8 module_def_load(ModuleDef* out, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_WARN("module_def_load: could not open '%s'.", path);
        return FALSE;
    }
    *out = ModuleDef{};
    out->size = HARDPOINT_SMALL;
    out->sensor_mult[0] = out->sensor_mult[1] = out->sensor_mult[2] = 1.0f;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        module_rstrip(line);
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
        if (sscanf(line, "type %31s", tok) == 1) {
            out->type = module_type_from_token(tok);
            if (out->type == 0)
                BS_LOG_WARN("module_def_load: unknown type '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "size %31s", tok) == 1) {
            if (!module_size_from_token(tok, &out->size))
                BS_LOG_WARN("module_def_load: unknown size '%s' in '%s'.", tok, path);
            continue;
        }
        if (sscanf(line, "glyph %7s", out->glyph) == 1) continue;
        if (sscanf(line, "icon %15s", out->icon) == 1) continue;
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
        f32 m0, m1, m2;
        if (sscanf(line, "sensor_mult %f %f %f", &m0, &m1, &m2) == 3) {
            out->sensor_mult[0] = m0;
            out->sensor_mult[1] = m1;
            out->sensor_mult[2] = m2;
            continue;
        }
    }
    fclose(f);
    if (out->id[0] == '\0' || out->type == 0) {
        BS_LOG_WARN("module_def_load: '%s' is missing a valid id/type; skipped.", path);
        return FALSE;
    }
    if (out->name[0] == '\0') {
        strncpy(out->name, out->id, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
    }
    if (out->glyph[0] == '\0') {
        out->glyph[0] = out->name[0];
        out->glyph[1] = '\0';
    }
    if (out->icon[0] == '\0') {
        // Default emblem by module kind (only sensors exist today; others fall back to glyph text).
        if (out->type == MODULE_TYPE_SENSOR) {
            strncpy(out->icon, "ic-sensor", sizeof(out->icon) - 1);
            out->icon[sizeof(out->icon) - 1] = '\0';
        }
    }
    return TRUE;
}

b8 module_registry_load(ModuleRegistry* reg, const char* manifest_path) {
    if (!reg || !manifest_path) return FALSE;
    reg->count = 0;
    FILE* f = fopen(manifest_path, "rb");
    if (!f) {
        BS_LOG_ERROR("module_registry_load: could not open manifest '%s'.", manifest_path);
        return FALSE;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        module_rstrip(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (reg->count >= MODULE_REGISTRY_MAX) {
            BS_LOG_WARN("module_registry_load: registry full (max %d); '%s' skipped.",
                        MODULE_REGISTRY_MAX, line);
            continue;
        }
        if (module_def_load(&reg->defs[reg->count], line))
            reg->count++;
    }
    fclose(f);
    BS_LOG_INFO("module_registry_load: %d modules loaded from '%s'.", reg->count, manifest_path);
    return TRUE;
}

const ModuleDef* module_registry_find(const ModuleRegistry* reg, const char* id) {
    if (!reg || !id) return nullptr;
    for (i32 i = 0; i < reg->count; ++i)
        if (strcmp(reg->defs[i].id, id) == 0) return &reg->defs[i];
    return nullptr;
}
