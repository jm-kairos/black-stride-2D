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
typedef struct bs_sprite
{
    bs_math::Vec2 position; // world-space position of the pivot
    bs_math::Vec2 size;     // world width/height before rotation
    bs_math::Vec2 origin;   // normalized pivot within the quad; {0.5,0.5} = centered
    f32           rotation; // radians, counter-clockwise about the pivot
    bs_rect       uv;       // atlas sub-rect to sample ({0,0,1,1} = whole texture)
    bs_color      tint;     // multiplied with the sampled texel ({1,1,1,1} = unmodified)
    bs_texture    texture;  // texture handle; id 0 => engine's 1x1 white texture (solid fill)
    EBlendMode    blend;    // blend mode for this sprite
    u32           layer;    // primary sort key; lower layers draw first (behind)
} bs_sprite;

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
