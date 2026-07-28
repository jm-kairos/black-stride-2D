#pragma once

#include "defines.h"
#include "renderer/renderer_types.h"

struct PlatformState;

// =====================================================================================
// Internal backend interface (vtable). Each concrete backend (SDL3 GPU, and potentially
// Vulkan/D3D12 later) fills this function-pointer struct. The frontend (renderer.cpp)
// owns one instance and delegates to it. This header includes NO backend types.
// =====================================================================================

typedef struct renderer_backend
{
    // Opaque backend-owned state (e.g. the SDL GPU device, window, swapchain format).
    // Lives entirely inside the backend .cpp; the frontend never dereferences it.
    VOID_PTR internal_state;

    b8   (*initialize)(struct renderer_backend* backend, const char* app_name, struct PlatformState* plat);
    void (*shutdown)(struct renderer_backend* backend);

    void (*on_resize)(struct renderer_backend* backend, u16 width, u16 height);

    b8   (*begin_frame)(struct renderer_backend* backend, f32 dt);
    b8   (*end_frame)(struct renderer_backend* backend, f32 dt);

    void (*set_clear_color)(struct renderer_backend* backend, bs_color color);

    // ---- Phase 3: textures, camera, sprite batch ----

    // Create an RGBA8 GPU texture from tightly-packed pixels (w*h*4 bytes, row-major, top-left
    // origin). Returns an opaque handle (id 0 on failure). The frontend decodes the image file
    // (stb_image) and hands raw bytes here so this stays the only TU touching the GPU.
    bs_texture (*create_texture)(struct renderer_backend* backend, const u8* pixels, u32 width, u32 height);

    // Re-upload pixels to a texture previously returned by create_texture. Width and height must
    // match the original texture dimensions. Returns TRUE on success.
    b8 (*update_texture)(struct renderer_backend* backend, bs_texture texture, const u8* pixels, u32 width, u32 height);

    // Release a texture previously returned by create_texture. No-op on an invalid handle.
    void (*destroy_texture)(struct renderer_backend* backend, bs_texture texture);

    // Store the camera; the backend rebuilds the view-projection at draw time from the live
    // swapchain size so it stays correct across resizes.
    void (*set_camera)(struct renderer_backend* backend, Camera2D camera);

    // Append a sprite to the current frame's CPU-side batch. Flushed (sorted, uploaded, drawn)
    // in end_frame. Must be called only while a frame is active.
    void (*draw_sprite)(struct renderer_backend* backend, const bs_sprite* sprite);

    // Append a 4-map mapped sprite to the current frame. Rendered inside the scene pass.
    void (*draw_mapped_sprite)(struct renderer_backend* backend, const bs_mapped_sprite* sprite);

    // Draw a procedural parallax starfield layer via custom GPU pipeline.
    // Parameters are queued and rendered during end_frame.
    void (*draw_starfield)(struct renderer_backend* backend, const bs_starfield_params* params);

    // Draw a procedural sunburst star via custom GPU pipeline.
    // Parameters are queued and rendered during end_frame.
    void (*draw_sunburst)(struct renderer_backend* backend, const bs_sunburst_params* params);

    // Draw a real-time procedural star surface via custom GPU pipeline.
    // Parameters are queued and rendered during end_frame.
    void (*draw_starsurface)(struct renderer_backend* backend, const bs_starsurface_params* params);

    // Draw a real-time procedural planet (impostor sphere) via custom GPU pipeline.
    // Parameters are queued and rendered during end_frame.
    void (*draw_planetsurface)(struct renderer_backend* backend, const bs_planetsurface_params* params);

    // Draw a procedural radiation heat map via custom GPU pipeline.
    // Parameters are queued and rendered during end_frame.
    void (*draw_heat_map)(struct renderer_backend* backend, const bs_heat_map_params* params);

    // Draw a procedural alpha-blended nebula/dust cloud layer via custom GPU pipeline.
    // Parameters are stored and rendered during end_frame.
    void (*draw_nebula)(struct renderer_backend* backend, const bs_nebula_params* params);

    // Store the editable list of 2D point lights (+ scene-global ambient and the unlit-layer
    // threshold) used by the sprite fragment shader. The backend copies them and pushes the packed
    // block as a fragment uniform per draw-run in end_frame. Lights beyond the backend cap are
    // dropped. Passing count 0 renders the scene fullbright.
    void (*set_lights)(struct renderer_backend* backend, const bs_light2d* lights, u32 count,
                       bs_color ambient, u32 unlit_layer);

    // Store tunable glow parameters for the sprite fragment shader. Pushed alongside lights
    // as part of the per-draw-run fragment uniform. NULL resets to defaults.
    void (*set_glow_params)(struct renderer_backend* backend, const bs_glow_params* params);

    // Enable/disable the full-scene HDR bloom post-process.
    void (*set_bloom_enabled)(struct renderer_backend* backend, b8 enabled);

    // Tune bloom threshold and intensity.
    void (*set_bloom_params)(struct renderer_backend* backend, f32 threshold, f32 intensity);

    // Anamorphic streak post-process (aux-bloom path).
    void (*set_streak_enabled)(struct renderer_backend* backend, b8 enabled);
    void (*set_streak_params)(struct renderer_backend* backend, f32 angle, f32 length);
    void (*set_streak_intensity)(struct renderer_backend* backend, f32 intensity);
    void (*set_streak_source)(struct renderer_backend* backend, bs_math::Vec2 screen_pos);
    void (*set_streak_flare_intensity)(struct renderer_backend* backend, f32 intensity);
    void (*set_aux_bloom_mode)(struct renderer_backend* backend, b8 enabled);

    // ---- Phase 4: frame statistics ----

    // Write the most recently completed frame's stats (sprite/quad count, GPU draw calls) into
    // *out_stats. The backend snapshots these at the end of end_frame. May be NULL on a backend
    // that does not track stats; the frontend checks before calling.
    void (*get_frame_stats)(struct renderer_backend* backend, bs_frame_stats* out_stats);

    // Set the swapchain present mode at runtime (FALSE = VSYNC, TRUE = IMMEDIATE). Used for
    // GPU-cost profiling. Returns FALSE (and leaves the swapchain on VSYNC) when the driver
    // does not support IMMEDIATE or the swapchain reconfigure fails.
    b8 (*set_present_mode)(struct renderer_backend* backend, b8 immediate);

    // Breakdown of the previous end_frame's present cost (milliseconds):
    //   acquire_ms = blocking wait inside SDL_WaitAndAcquireGPUSwapchainTexture (VSYNC /
    //                swapchain-image starvation shows up HERE, not as GPU work)
    //   submit_ms  = SDL_SubmitGPUCommandBuffer
    // May be NULL on a backend that does not track it; the frontend checks before calling.
    void (*get_present_timing)(struct renderer_backend* backend, f32* out_acquire_ms, f32* out_submit_ms);
} renderer_backend;

// Factory: wire up the function pointers for the requested backend type.
// Returns TRUE on success. Does not allocate the device — that happens in initialize().
b8   renderer_backend_create(ERendererBackend type, renderer_backend* out_backend);

// Clears the function pointers (does not free the device — call shutdown() first).
void renderer_backend_destroy(renderer_backend* backend);
