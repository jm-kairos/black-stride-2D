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
