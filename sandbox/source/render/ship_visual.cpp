#define _CRT_SECURE_NO_WARNINGS
#include "render/ship_visual.h"
#include "sim/ship.h"
#include <core/logger.h>
#include <renderer/renderer.h>
#include <stdio.h>
#include <string.h>
using namespace bs_math;
void ship_visual_clear(ShipVisual* v) {
    if (!v) return;
    v->layer_count = 0;
    v->size_local = Vec2{ 0.0f, 0.0f };
    v->has_sprite = FALSE;
}
// Format (whitespace-tolerant, '#' comments):
//   size <w> <h>
//   layer sprite   <texpath> <offx> <offy> <px_per_unit> <z> <roof_only>
//   layer mapped   <diffuse> <normal> <depth> <position> <offx> <offy> <px_per_unit> <z> <roof_only>
static i32 rstrip_vis(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
    return n;
}
b8 ship_visual_load(ShipVisual* v, const char* path) {
    if (!v || !path) return FALSE;
    FILE* f = fopen(path, "rb");
    if (!f) return FALSE;
    ship_visual_clear(v);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        rstrip_vis(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        f32 w, h;
        if (sscanf(line, "size %f %f", &w, &h) == 2) { v->size_local = Vec2{ w, h }; continue; }
        char kindbuf[16];
        if (sscanf(line, "layer %15s", kindbuf) == 1) {
            if (v->layer_count >= VIS_MAX_LAYERS) continue;
            VisualLayer& l = v->layers[v->layer_count];
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
                    v->layer_count++;
                    v->has_sprite = TRUE;
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
                    v->layer_count++;
                    v->has_sprite = TRUE;
                }
            }
        }
    }
    fclose(f);
    if (v->layer_count <= 0) return FALSE;
    BS_LOG_INFO("ship_visual_load: '%s' -> %d layers (sprite=%d).",
                path, v->layer_count, (i32)v->has_sprite);
    return TRUE;
}
void ship_visual_resolve_textures(ShipVisual* v) {
    if (!v) return;
    for (i32 i = 0; i < v->layer_count; ++i) {
        VisualLayer& l = v->layers[i];
        if (l.texture_path[0] != '\0') {
            l.texture = renderer_load_texture(l.texture_path);
            if (!l.texture.id)
                BS_LOG_WARN("ship_visual: failed to load diffuse '%s'.", l.texture_path);
        }
        if (l.kind == VIS_LAYER_MAPPED) {
            l.normal_map  = renderer_load_texture(l.normal_path);
            l.depth_map   = renderer_load_texture(l.depth_path);
            l.position_map = renderer_load_texture(l.position_path);
            if (!l.normal_map.id)  BS_LOG_WARN("ship_visual: failed to load normal '%s'.", l.normal_path);
            if (!l.depth_map.id)   BS_LOG_WARN("ship_visual: failed to load depth '%s'.", l.depth_path);
            if (!l.position_map.id) BS_LOG_WARN("ship_visual: failed to load position '%s'.", l.position_path);
        }
    }
}
