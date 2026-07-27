#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
// =====================================================================================
// Visual layer (decoupled presentation). What the player SEES — independent of nav/systems.
// A ship's appearance is an ordered list of sprite layers, each a textured quad placed in
// ship-local space. Authored directly in a .ship file or via a standalone .svis file.
// =====================================================================================
#define VIS_MAX_LAYERS 8
enum VisualLayerKind {
    VIS_LAYER_SPRITE = 0,
    VIS_LAYER_MAPPED,
};
struct VisualLayer {
    VisualLayerKind kind;           // sprite or 4-map mapped layer
    char         texture_path[128]; // art path — resolved to a texture at load time
    bs_texture   texture;           // resolved handle (0 until ship_visual_resolve_textures)
    // For mapped layers only:
    char         normal_path[128];
    char         depth_path[128];
    char         position_path[128];
    bs_texture   normal_map;
    bs_texture   depth_map;
    bs_texture   position_map;
    bs_math::Vec2 offset_local;     // ship-local placement of the art's center
    f32          px_per_unit;       // art pixels per ship-local unit
    u32          z;                 // draw order (lower first)
    b8           roof_only;         // drawn only as the global-mode roof silhouette
};
struct ShipVisual {
    VisualLayer   layers[VIS_MAX_LAYERS];
    i32           layer_count;
    bs_math::Vec2 size_local;       // overall ship-local footprint (w,h)
    b8            has_sprite;       // TRUE if any layer exists
};
// Reset to empty.
void ship_visual_clear(ShipVisual* v);
// Load an authored .svis file. Returns FALSE on IO/parse error.
b8 ship_visual_load(ShipVisual* v, const char* path);
// Resolve each layer's texture_path into a live handle. Call AFTER the renderer is
// initialized. Safe to call once at game init.
void ship_visual_resolve_textures(ShipVisual* v);
