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

    // Release a texture previously returned by create_texture. No-op on an invalid handle.
    void (*destroy_texture)(struct renderer_backend* backend, bs_texture texture);

    // Store the camera; the backend rebuilds the view-projection at draw time from the live
    // swapchain size so it stays correct across resizes.
    void (*set_camera)(struct renderer_backend* backend, Camera2D camera);

    // Append a sprite to the current frame's CPU-side batch. Flushed (sorted, uploaded, drawn)
    // in end_frame. Must be called only while a frame is active.
    void (*draw_sprite)(struct renderer_backend* backend, const bs_sprite* sprite);

    // ---- Phase 4: frame statistics ----

    // Write the most recently completed frame's stats (sprite/quad count, GPU draw calls) into
    // *out_stats. The backend snapshots these at the end of end_frame. May be NULL on a backend
    // that does not track stats; the frontend checks before calling.
    void (*get_frame_stats)(struct renderer_backend* backend, bs_frame_stats* out_stats);
} renderer_backend;

// Factory: wire up the function pointers for the requested backend type.
// Returns TRUE on success. Does not allocate the device — that happens in initialize().
b8   renderer_backend_create(ERendererBackend type, renderer_backend* out_backend);

// Clears the function pointers (does not free the device — call shutdown() first).
void renderer_backend_destroy(renderer_backend* backend);
