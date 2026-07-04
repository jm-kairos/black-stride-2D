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

    // ---- Virtual-quadtree LOD (precision-safe multi-scale starfield) --------------------
    // The camera center is passed as a HierPos2 split (integer cell + local offset) so the
    // shader can reduce coordinates per LOD level without the ~256-unit f32 snapping that a
    // single combined world Vec2 suffers billions of units from the origin.
    bs_math::Vec2 cam_cell;            // camera-center HierPos2 cell index (exact integers as f32)
    bs_math::Vec2 cam_local;           // camera-center local offset within its cell (world units)
    bs_math::Vec2 star_rel;            // hero-star position relative to camera center (world units)
    f32      base_cell;                // finest LOD cell size in world units (e.g. 64)
    f32      lod_factor;               // cell-size ratio between adjacent LOD levels (e.g. 4)
    f32      target_px;                // desired on-screen cell size in pixels (LOD selection)
    f32      lod_levels;               // number of LOD slots to accumulate (e.g. 4)
    f32      parallax_near;            // finest-level parallax 0..1 (1 == world-locked)
    f32      parallax_falloff;         // per-level slowdown ratio (coarser levels move slower)
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

// Parameters for a real-time procedural star SURFACE drawn via a custom GPU pipeline.
// Passed to renderer_draw_starsurface; the backend queues them and renders during end_frame
// (premult-over blend: the opaque photosphere disc occludes the scene, the corona glows).
typedef struct bs_starsurface_params
{
    bs_math::Vec2 screen_pos;        // star centre in screen pixels (top-left origin)
    bs_math::Vec2 world_pos;         // star centre in world space (main camera)
    bs_math::Vec2 aux_bloom_world_pos; // proxy world position matching the aux-pass camera
    f32      body_radius;            // photosphere disc radius in screen pixels
    f32      glow_radius;            // outer corona extent in screen pixels (quad half-size)
    bs_color color;                  // intrinsic star colour tint
    f32      time;                   // elapsed time for animation
    f32      visibility;             // 0..1 fade (sensor range, etc.)
    // Surface look tunables (editor-driven):
    f32      noise_scale;            // granulation cell frequency
    f32      flow_speed;             // convection animation speed
    f32      granule_contrast;       // granulation contrast around mid-grey
    f32      hotspot_gain;           // emissive bright-spot strength
    f32      sunspot_density;        // dark-spot coverage
    f32      limb_darkening;         // limb darkening exponent
    f32      brightness;             // overall surface brightness
    f32      corona_strength;        // outer glow intensity
    f32      dark_radius;            // radius (in body radii) of the opaque black surround
    u32      layer;                  // render sort key (e.g. LAYER_CELESTIAL)
    u16      fb_w;                   // viewport width in pixels
    u16      fb_h;                   // viewport height in pixels
    b8       aux_bloom;              // when TRUE, emit a proxy sprite for the aux-bloom/streak pass
} bs_starsurface_params;

// Available heat-map color palettes.
typedef enum bs_heat_palette
{
    BS_HEAT_PALETTE_RAINBOW = 0,
    BS_HEAT_PALETTE_THERMAL,
    BS_HEAT_PALETTE_BLACKBODY,
    BS_HEAT_PALETTE_CUSTOM,
    BS_HEAT_PALETTE_COUNT,
} bs_heat_palette;

// Parameters for the single-pass procedural radiation heat map.
// Passed to renderer_draw_heat_map; the backend queues one fullscreen draw during end_frame.
#define BS_MAX_HEAT_SOURCES 256
typedef struct bs_heat_map_params
{
    bs_math::Vec2 camera_pos;        // world-space camera center
    f32           viewport_w;        // world-space viewport width
    f32           viewport_h;        // world-space viewport height
    f32           base_radius;       // detector base radius
    f32           heat_warp_strength; // world units of domain warp displacement
    f32           venn_sharpness;  // 0.0 = organic soft Venn boundary, 1.0 = hard-edged sensor circle
    f32           threshold;         // scalar field threshold
    f32           intensity;         // global opacity multiplier
    u32           source_count;      // number of valid entries in sources
    f32           heat_tail_length;  // max number of active trail points
    f32           heat_tail_fade;    // falloff exponent for trail emission
    f32           heat_signature_radius; // visual radius of the heat signature blob around emitters
    u32           palette;           // bs_heat_palette index
    f32           color_falloff_power; // power applied to normalized value before gradient lookup
    bs_color      color_low;         // custom edge color
    bs_color      color_high;        // custom center color
    bs_math::Vec2 sources[BS_MAX_HEAT_SOURCES]; // world-space positions
    f32           emissions[BS_MAX_HEAT_SOURCES]; // 0..1 emission per source
    b8            is_detector[BS_MAX_HEAT_SOURCES]; // TRUE = player detector source, FALSE = heat emitter
} bs_heat_map_params;

// Parameters for a procedural alpha-blended nebula/dust cloud layer.
// Passed to renderer_draw_nebula; the backend stores one set of parameters and renders it
// as a fullscreen alpha-blended overlay during end_frame.
typedef struct bs_nebula_params
{
    Camera2D      cam;          // virtual camera (position * parallax, zoom * zoom_scale)
    f32           zoom_scale;   // layer depth-cue multiplier
    u32           layer_id;     // render sort key
    u32           seed;         // deterministic hash seed
    u16           fb_w;         // viewport width in pixels
    u16           fb_h;         // viewport height in pixels
    bs_math::Vec2 blur;         // reserved for future motion blur
    f32           intensity;     // global nebula opacity multiplier
    f32           dust_intensity;// dust cloud opacity multiplier
    // Editor-tunable colors for the nebula/dust shader.
    bs_color      gas_color_a;      // deep base gas
    bs_color      gas_color_b;      // mid gas
    bs_color      gas_color_c;      // bright highlight core
    bs_color      dust_color;       // dark dust silhouette
    f32           gas_brightness_mul; // extra luminance multiplier for gas
    f32           highlight_power;  // how much the bright core is emphasized
    f32           palette_shift;    // rotates the cosine palette index
    f32           swirl_strength;   // domain rotation strength
    f32           falloff_radius;   // controls radial/band fade
    f32           band_strength;    // how much Milky Way band falloff is applied
    f32           lod_target;       // world units per noise unit at zoom==1 (LOD feature scale)
    f32           parallax;         // 0..1 parallax factor (1 == world-locked, <1 == distant/slower)
    f32           biome_strength;   // 0..1 galaxy-wide biome modulation amount
    f32           biome_scale;      // world units per biome noise unit (region size)
    f32           biome_hue_spread; // 0..1 color-family rotation between biomes
    f32           zoom_detail;      // 0..1 extra fine detail emerging as you zoom in
    f32           zoom_saturation;  // 0..1 extra saturation/contrast as you zoom in
    bs_math::Vec2 cam_cell;   // camera-center HierPos2 cell index (exact integers as f32)
    bs_math::Vec2 cam_local;  // camera-center local offset within its cell (world units)
} bs_nebula_params;

