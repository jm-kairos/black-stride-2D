#pragma once

#include "defines.h"
#include "renderer/renderer_types.h"

struct PlatformState; // forward decl; renderer never sees platform internals.

// =====================================================================================
// Renderer frontend — the public, game- and application-facing API.
//
// Lifecycle ownership:
//   * renderer_initialize / renderer_shutdown are called by application.cpp.
//   * renderer_begin_frame / renderer_end_frame are driven by the APPLICATION LOOP,
//     not the game. The game's render(dt) runs between them and only submits draws
//     (renderer_draw_* — added in later phases).
//   * No SDL/backend types ever appear in this header.
// =====================================================================================

// Bring up the renderer and its backend (creates the GPU device, claims the window,
// configures the swapchain). Must be called AFTER the platform window exists and BEFORE
// the game's init() so the game can create GPU resources in its own init.
bs__api__ b8   renderer_initialize(const char* app_name, struct PlatformState* plat);

// Tear down the backend and release the device. Call before platform_terminate().
bs__api__ void renderer_shutdown();

// Notify the renderer that the window framebuffer size changed (swapchain recreation is
// handled internally by the backend on the next acquire).
bs__api__ void renderer_on_resize(u16 width, u16 height);

// Per-frame lifecycle. begin_frame acquires a command buffer + swapchain image and begins
// the render pass (clearing to the configured clear color). Returns FALSE if the frame
// should be skipped (e.g. window minimized => null swapchain texture); the caller must then
// NOT call end_frame for this frame. end_frame ends the pass, submits, and presents.
bs__api__ b8   renderer_begin_frame(f32 dt);
bs__api__ b8   renderer_end_frame(f32 dt);

// Set the color the swapchain is cleared to at the start of each frame.
bs__api__ void renderer_set_clear_color(bs_color color);

// -------------------------------------------------------------------------------------
// Phase 3 — textures, camera, and the 2D sprite batch.
// -------------------------------------------------------------------------------------

// Decode an image file (PNG) into an RGBA8 GPU texture and return its opaque handle. The path
// is relative to the working directory (assets are staged next to the exe). Returns a handle
// with id 0 (BS_INVALID_HANDLE) on failure. Create textures in the game's init(), not per frame.
bs__api__ bs_texture renderer_load_texture(const char* path);

// Create an RGBA8 GPU texture directly from CPU pixel bytes (tightly packed, 4 bytes/texel,
// top-left origin, `width*height*4` bytes). For procedurally generated textures — bitmap-font
// atlases, gradients, masks — that have no file on disk. Returns BS_INVALID_HANDLE on failure.
// Like renderer_load_texture, create these in the game's init(), not per frame.
bs__api__ bs_texture renderer_create_texture(const u8* pixels, u32 width, u32 height);

// Re-upload pixels to an existing texture created by renderer_create_texture. Width and height
// must match the original dimensions. Safe to call with an invalid handle; returns TRUE on success.
bs__api__ b8 renderer_update_texture(bs_texture texture, const u8* pixels, u32 width, u32 height);

// Release a texture created by renderer_load_texture. Safe to call with an invalid handle.
bs__api__ void renderer_destroy_texture(bs_texture texture);

// Set the camera used to build the view-projection for subsequent frames. The renderer keeps a
// copy; the game can mutate its own Camera2D freely and re-submit. If never called, the renderer
// uses a default camera (origin-centered, zoom 1).
bs__api__ void renderer_set_camera(Camera2D camera);

// Append a sprite to the current frame's batch. MUST be called between begin_frame and end_frame
// (i.e. from the game's render(dt)). The sprite is copied; no GPU work happens until end_frame
// flushes the batch (sort -> upload -> draw). Sprites with texture id 0 use the engine's 1x1
// white texture (solid-color fill via the tint).
bs__api__ void renderer_draw_sprite(const bs_sprite* sprite);

// Global alpha multiplier applied to every subsequent sprite/line/circle/quad tint until
// changed or the next frame (reset to 1.0 at begin_frame). Use it to cross-fade whole render
// passes (e.g. blending the arena and galaxy-map looks). Clamped to [0,1]. Does NOT affect
// sunbursts/starfields/nebula (those carry their own visibility params).
bs__api__ void renderer_set_draw_alpha(f32 alpha);
bs__api__ f32  renderer_get_draw_alpha();

// Append a 4-map mapped sprite to the current frame. Rendered inside the same scene pass as
// regular sprites so it participates in bloom. MUST be called between begin_frame and end_frame.
bs__api__ void renderer_draw_mapped_sprite(const bs_mapped_sprite* sprite);

// Draw a procedural parallax starfield layer using a custom GPU pipeline.
// `params` carries the per-layer virtual camera, seed, tile size, and density.
// The backend queues the parameters and renders during end_frame (no render pass
// is active when this is called from game_render).
bs__api__ void renderer_draw_starfield(const bs_starfield_params* params);

// Draw a procedural sunburst star using a custom GPU pipeline.
// `params` carries the screen-space position, radii, colour, and time.
// The backend queues the parameters and renders during end_frame.
bs__api__ void renderer_draw_sunburst(const bs_sunburst_params* params);

// Draw a real-time procedural star surface (animated photosphere + corona) using a custom
// GPU pipeline. The backend queues the parameters and renders during end_frame.
bs__api__ void renderer_draw_starsurface(const bs_starsurface_params* params);

// Draw a procedural radiation heat map using a custom GPU pipeline.
// The backend queues the parameters and renders as a fullscreen alpha-blended overlay during end_frame.
bs__api__ void renderer_draw_heat_map(const bs_heat_map_params* params);

// Draw a procedural alpha-blended nebula/dust cloud layer using a custom GPU pipeline.
// The backend stores the parameters and renders them as a fullscreen alpha-blended overlay during end_frame.
bs__api__ void renderer_draw_nebula(const bs_nebula_params* params);

// Set the movable 2D point lights applied per-pixel by the sprite shader. The renderer copies the
// list; re-submit each frame (or whenever it changes). `count == 0` renders the scene fullbright
// (lighting is opt-in). `ambient` is the scene-global floor added before accumulating lights;
// sprite layers >= `unlit_layer` render fullbright (keep HUD/UI readable). Lights beyond the
// backend cap are dropped.
bs__api__ void renderer_set_lights(const bs_light2d* lights, u32 count, bs_color ambient, u32 unlit_layer);

// Set tunable glow parameters for the sprite fragment shader's procedural glow / heat effects.
// The renderer copies the struct; re-submit each frame or when it changes. Passing NULL
// resets to defaults. Affects all sprites that use `custom.x > 0` (bullets, thrusters, etc.).
bs__api__ void renderer_set_glow_params(const bs_glow_params* params);

// Enable or disable the full-scene HDR bloom post-process. When enabled, the renderer renders
// the scene to an offscreen target, extracts bright pixels, blurs them, and composites back.
bs__api__ void renderer_set_bloom_enabled(b8 enabled);

// Tune the bloom threshold (luminance above which pixels contribute to bloom) and intensity
// (multiplier on the blurred bloom when composited back). Both default to reasonable values.
bs__api__ void renderer_set_bloom_params(f32 threshold, f32 intensity);

// Anamorphic streak post-process (aux-bloom path).
bs__api__ void renderer_set_streak_enabled(b8 enabled);
bs__api__ void renderer_set_streak_params(f32 angle, f32 length);
bs__api__ void renderer_set_streak_intensity(f32 intensity);
bs__api__ void renderer_set_streak_source(bs_math::Vec2 screen_pos);
bs__api__ void renderer_set_streak_flare_intensity(f32 intensity);
bs__api__ void renderer_set_aux_bloom_mode(b8 enabled);

// -------------------------------------------------------------------------------------
// Phase 4 — debug / vector layer + frame stats.
//
// These are convenience helpers built ON TOP of the sprite batch: every primitive is emitted
// as one or more oriented quads using the engine's 1x1 white texture, tinted to `color`. This
// reuses the proven, blended, layer-sorted sprite path exactly (no separate GPU line pipeline,
// which would be hostage to unreliable cross-backend line-width clamping). Thickness is in
// world units (pixels at zoom 1). All must be called between begin_frame and end_frame.
//
// `layer` follows sprite layering: lower draws first (behind). Debug vectors typically use a
// high layer so they sit on top of game sprites.
// -------------------------------------------------------------------------------------

// A single thick line segment from a to b (world space), as one rotated quad.
bs__api__ void renderer_draw_line(bs_math::Vec2 a, bs_math::Vec2 b, f32 thickness,
                                  bs_color color, u32 layer);

// A filled axis-aligned rectangle centered at `center` with `size` (world units).
bs__api__ void renderer_draw_quad(bs_math::Vec2 center, bs_math::Vec2 size,
                                  bs_color color, u32 layer);

// A rectangle OUTLINE (4 thick line segments) centered at `center` with `size`.
bs__api__ void renderer_draw_rect_outline(bs_math::Vec2 center, bs_math::Vec2 size,
                                          f32 thickness, bs_color color, u32 layer);

// A circle OUTLINE approximated by `segments` line chords (clamped to a sane range). Good for
// orbits, weapon ranges, sensor/aggro radii, hitboxes.
bs__api__ void renderer_draw_circle(bs_math::Vec2 center, f32 radius, u32 segments,
                                    f32 thickness, bs_color color, u32 layer);

// A world-space grid spanning [min,max] with the given cell spacing — handy spatial reference
// for a top-down sandbox. Emitted as thin lines on `layer`.
bs__api__ void renderer_draw_grid(bs_math::Vec2 min, bs_math::Vec2 max, f32 spacing,
                                  f32 thickness, bs_color color, u32 layer);

// Snapshot of the most recently completed frame's draw statistics (see bs_frame_stats).
bs__api__ bs_frame_stats renderer_get_frame_stats();

// Frame CPU timing (milliseconds), reported by the application loop each frame:
//   render_ms  = time spent in the game's render(dt)
//   present_ms = time spent in end_frame (submit + present, includes the VSYNC wait)
// The game/sandbox can read these to display a CPU-vs-present breakdown.
bs__api__ void renderer_report_frame_timing(f32 render_ms, f32 present_ms);
bs__api__ void renderer_get_frame_timing(f32* out_render_ms, f32* out_present_ms);

// Runtime swapchain present mode. FALSE = VSYNC (default, capped to refresh), TRUE = IMMEDIATE
// (uncapped) for GPU-cost profiling. When IMMEDIATE is active the application loop skips its
// software frame cap so present_ms reflects GPU submit+execute time. Falls back to VSYNC if the
// driver does not support IMMEDIATE.
bs__api__ void renderer_set_present_mode(b8 immediate);
bs__api__ b8   renderer_is_present_immediate();
