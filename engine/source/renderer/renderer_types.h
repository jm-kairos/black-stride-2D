#pragma once

#include "defines.h"
#include "math/math_utils.h"

// =====================================================================================
// Public renderer types. This header MUST NOT include any backend headers (SDL3, etc.).
// The game programs against these opaque handles and POD structs only. The real backend
// objects live in engine-side pools indexed by the handle ids.
//
// Math types (Vec2/Mat4) come from math/math_utils.h (namespace bs_math) — that is engine
// math, not a backend, so it is allowed here. The seam rule only bans GPU/SDL headers.
// =====================================================================================

// Opaque resource handles. id 0 is reserved to mean "invalid". Internally the high bits
// may encode a generation counter to detect use-after-free of stale handles.
typedef struct bs_texture  { u32 id; } bs_texture;
typedef struct bs_shader   { u32 id; } bs_shader;
typedef struct bs_pipeline { u32 id; } bs_pipeline;
typedef struct bs_buffer   { u32 id; } bs_buffer;
typedef struct bs_sampler  { u32 id; } bs_sampler;

#define BS_INVALID_HANDLE 0

// Which concrete backend the renderer frontend should drive. Only SDL3 GPU exists for now,
// but the seam keeps the frontend/backend split honest and game code backend-agnostic.
typedef enum ERendererBackend
{
    RENDERER_BACKEND_SDL_GPU = 0
} ERendererBackend;

// Linear RGBA color, components in [0,1].
typedef struct bs_color
{
    f32 r;
    f32 g;
    f32 b;
    f32 a;
} bs_color;

// Blend modes the 2D pipelines support. The backend builds one graphics pipeline per mode
// (sharing the sprite shaders) and selects per draw-run from the sprite's blend field.
typedef enum EBlendMode
{
    BLEND_NONE = 0, // opaque; no blending
    BLEND_ALPHA,    // standard src-alpha / one-minus-src-alpha transparency
    BLEND_ADDITIVE, // src-alpha / one — glows, thruster plumes, energy FX
    BLEND_MULTIPLY, // dst-color / zero — shadows, darkening
    BLEND_MODE_COUNT
} EBlendMode;

// A normalized sub-rectangle of a texture (atlas UVs), components in [0,1]. {0,0,1,1} is the
// whole texture. Origin is top-left in texture space (y-down).
typedef struct bs_rect
{
    f32 x;
    f32 y;
    f32 w;
    f32 h;
} bs_rect;

// A 2D sprite draw command. POD; the game fills one and passes it to renderer_draw_sprite,
// which appends it to the CPU-side batch (no GPU work until end_frame). World units are pixels
// at camera zoom 1.0.
typedef struct bs_mapped_sprite
{
    bs_math::Vec2 position;      // world-space pivot
    bs_math::Vec2 size;          // world width/height before rotation
    bs_math::Vec2 origin;        // normalized pivot within the quad
    f32           rotation;      // radians, CCW
    bs_rect       uv;            // atlas sub-rect
    bs_color      tint;          // multiplier on sampled diffuse
    bs_texture    diffuse_map;   // albedo
    bs_texture    normal_map;    // normal map
    bs_texture    depth_map;     // depth/height map
    bs_texture    position_map;  // position map
    bs_math::Vec3 light_dir;     // normalized directional light direction in world space
    u32           layer;         // draw sort key
} bs_mapped_sprite;

// Tunable per-sprite shader glow parameters. Pushed to the GPU as a fragment uniform and
// interpreted by the sprite fragment shader for procedural bullet glow / heat effects.
typedef struct bs_glow_params
{
    f32 intensity;    // global glow multiplier (default 1.0)
    f32 falloff;        // radial Gaussian exponent (default 6.0)
    f32 head_mult;      // head bloom intensity multiplier (default 4.0)
    f32 head_falloff;   // head bloom Gaussian width (default 2.5)
    f32 head_range;     // UV threshold where head bloom begins (default 0.80)
    f32 distort_amp;    // heat distortion UV warp amplitude (default 0.08)
    f32 wave_speed;     // primary wave speed (default 15.0)
    f32 wave_freq;      // primary wave frequency along UV.y (default 8.0)
    f32 jitter_speed;   // secondary jitter wave speed (default 45.0)
    f32 jitter_freq;    // secondary jitter wave frequency (default 24.0)
    bs_color glow_tint; // base glow colour (default 1.0, 0.85, 0.5)
    bs_color temp_cool; // tail temperature colour (default 0.90, 0.15, 0.02)
    bs_color temp_warm; // mid temperature colour (default 1.0, 0.45, 0.05)
    bs_color temp_hot;  // head temperature colour (default 1.0, 0.98, 0.90)
} bs_glow_params;

typedef struct bs_sprite
{
    bs_math::Vec2 position; // world-space position of the pivot
    bs_math::Vec2 size;     // world width/height before rotation
    bs_math::Vec2 origin;   // normalized pivot within the quad; {0.5,0.5} = centered
    f32           rotation; // radians, counter-clockwise about the pivot
    bs_rect       uv;       // atlas sub-rect to sample ({0,0,1,1} = whole texture)
    bs_color      tint;     // multiplied with the sampled texel ({1,1,1,1} = unmodified)
    bs_color      custom;   // per-sprite shader parameters (interpreted by the fragment shader)
    bs_texture    texture;  // texture handle; id 0 => engine's 1x1 white texture (solid fill)
    EBlendMode    blend;    // blend mode for this sprite
    u32           layer;    // primary sort key; lower layers draw first (behind)
    const bs_glow_params* glow_override; // NULL => use global glow_params; else per-sprite override
} bs_sprite;

// Layers below this value are rendered through the bloom pipeline (game world sprites).
// Layers at or above this value are drawn directly to the swapchain after composite
// (UI, debug overlays, gizmos, HUD text) so they remain crisp and unblurred.
#define BS_LAYER_BLOOM_THRESHOLD 50

// One movable 2D point light, accumulated per-pixel by the sprite fragment shader. The game owns
// an editable list of these (POD) and submits them via renderer_set_lights; the backend packs them
// into a fragment uniform pushed per draw-run. Scene-global lighting params (ambient floor, the
// unlit-layer threshold) are passed alongside the list to renderer_set_lights, not stored per light.
typedef struct bs_light2d
{
    bs_math::Vec2 position;  // world-space light center
    f32           radius;    // world units; brightness falls to zero at this distance
    f32           intensity; // light color multiplier at the center (1.0 = full color)
    bs_color      color;     // light color (alpha unused)
    b8            enabled;   // FALSE => this light contributes nothing (kept in the list, toggled off)
} bs_light2d;

// Per-frame render statistics, snapshotted at the end of each end_frame. Useful for an
// on-screen/title-bar HUD while there is no text system yet. `sprite_count` includes every
// quad submitted (sprites AND debug-layer primitives, since debug lines/circles are quads).
// `draw_calls` is the number of GPU indexed-draw calls after (blend,texture) run-merging.
typedef struct bs_frame_stats
{
    u32 sprite_count;
    u32 draw_calls;
} bs_frame_stats;

// A world-space 2D camera. The renderer turns this into a view-projection matrix each frame
// using the live swapchain dimensions. At zoom 1.0, one world unit maps to one pixel and
// `position` sits at the center of the window.
typedef struct Camera2D
{
    bs_math::Vec2 position; // world point shown at screen center
    f32           zoom;     // >1 zooms in, <1 zooms out; must be > 0
    f32           rotation; // radians, counter-clockwise
} Camera2D;

// Opaque CPU-side data for a VBO-based starfield layer.
// Passed through bs_starfield_params so the backend can upload/draw.
typedef struct bs_starfield_layer_data
{
    const float* vertices;     // packed vertex data (interleaved: offset_xy, size, corner)
    u32          vertexCount;  // total float count in vertices array
    const int*   tileIndex;    // prefix-sum tile index (tileIndex.back() = star count)
    int          tileCols;     // tiles per row
    int          widthMod;     // fieldSize - 1 (for wrapping)
} bs_starfield_layer_data;

// Parameters for a procedural parallax starfield layer drawn via custom GPU pipeline.
// Passed to renderer_draw_starfield; the backend queues them and renders during end_frame.
typedef struct bs_starfield_params
{
    Camera2D cam;                      // virtual camera (position * parallax, zoom * zoom_scale)
    f32      zoom_scale;               // layer depth-cue multiplier
    u32      layer_id;                 // render sort key
    u32      seed;                     // deterministic hash seed
    f32      tile_size;                // world units per tile
    i32      stars_per_tile;
    u16      fb_w;                     // viewport width in pixels
    u16      fb_h;                     // viewport height in pixels
    const bs_starfield_layer_data* layer_data;  // CPU-side star/tile data (legacy VBO path)
    u32      pass_index;               // 0,1,2 for 3-pass fullness (Endless Sky style)
    bs_math::Vec2 blur;                // camera velocity for motion-blur streaks
    // Procedural starfield tunables (used when layer_data is null).
    f32      density;                  // cell fill rate 0..1
    f32      size_mul;                 // star size multiplier
    f32      brightness_mul;           // overall brightness multiplier

    // Star dazzle effect: per-layer apparent star position and falloff tunables.
    bs_math::Vec2 star_pos;            // star position in layer's parallax shader space
    f32      dazzle_inner;             // world units: fully suppressed inside
    f32      dazzle_outer;             // world units: no suppression outside
    f32      dazzle_intensity;         // 0..1 suppression strength
} bs_starfield_params;

// Parameters for a procedural sunburst star drawn via custom GPU pipeline.
// Passed to renderer_draw_sunburst; the backend queues them and renders during end_frame.
typedef struct bs_sunburst_params
{
    bs_math::Vec2 screen_pos;        // star centre in screen pixels (top-left origin)
    bs_math::Vec2 world_pos;         // star centre in world space (for 3D sprite and main camera)
    bs_math::Vec2 aux_bloom_world_pos; // proxy world position matching the aux-pass camera
    f32      body_radius;            // hot core radius in screen pixels
    f32      glow_radius;            // outer glow radius in screen pixels (typically 3-5x body_radius)
    bs_color color;                  // intrinsic star colour tint
    f32      time;                   // elapsed time for animation
    f32      visibility;             // 0..1 fade (sensor range, etc.)
    u32      layer;                  // render sort key (e.g. LAYER_CELESTIAL)
    u16      fb_w;                   // viewport width in pixels
    u16      fb_h;                   // viewport height in pixels
    b8       aux_bloom;              // when TRUE, emit a proxy sprite for the aux-bloom/streak pass
} bs_sunburst_params;
