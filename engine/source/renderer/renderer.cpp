#include "renderer/renderer.h"
#include "renderer/renderer_backend.h"
#include "renderer/camera2d.h"
#include "renderer/bs_imgui.h"

#include "core/logger.h"
#include "core/memory/bs_memory.h"

// stb_image: decode PNG bytes -> RGBA8. The implementation TU is stb_image_impl.cpp; here we
// include only the declarations. This keeps texture DECODE (CPU, no GPU) in the frontend and
// texture UPLOAD (GPU) in the backend, preserving the "only the backend includes SDL" rule.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <stdio.h> // fopen/fread to slurp the image file without pulling SDL into the frontend.
#include <math.h>  // atan2f/cosf/sinf for the Phase 4 debug/vector helpers.

// =====================================================================================
// Renderer frontend. Owns the backend vtable and forwards the public API to it. Holds no
// SDL/backend types — everything platform/GPU-specific lives behind renderer_backend.
// =====================================================================================

struct RendererState
{
    renderer_backend backend;
    bs_color         clear_color;
    Camera2D         camera;       // last camera set this frame; used to keep debug-line
                                   // thickness a constant SCREEN width regardless of zoom
    b8               frame_active; // TRUE between a successful begin_frame and its end_frame
    b8               initialized;
};

static RendererState state;

b8 renderer_initialize(const char* app_name, struct PlatformState* plat)
{
    if (state.initialized)
    {
        BS_LOG_ERROR("renderer_initialize called more than once !");
        return FALSE;
    }

    // Default clear color: deep "space black" with a faint blue tint.
    state.clear_color = bs_color{ 0.02f, 0.02f, 0.05f, 1.0f };
    state.frame_active = FALSE;

    if (!renderer_backend_create(RENDERER_BACKEND_SDL_GPU, &state.backend))
    {
        BS_LOG_FATAL("Failed to create renderer backend.");
        return FALSE;
    }

    if (!state.backend.initialize(&state.backend, app_name, plat))
    {
        BS_LOG_FATAL("Renderer backend failed to initialize.");
        renderer_backend_destroy(&state.backend);
        return FALSE;
    }

    state.backend.set_clear_color(&state.backend, state.clear_color);

    state.camera = camera2d_default();

    // Bring up Dear ImGui now that the GPU device exists and the window is claimed. A failure
    // here is non-fatal: the engine renders fine without the debug/UI overlay, so log and carry
    // on rather than tearing down a working renderer.
    if (!bs_imgui_initialize())
    {
        BS_LOG_WARN("Dear ImGui failed to initialize; continuing without the UI overlay.");
    }

    state.initialized = TRUE;
    BS_LOG_INFO("Renderer initialized (backend: SDL3 GPU).");
    return TRUE;
}

void renderer_shutdown()
{
    if (!state.initialized)
    {
        return;
    }

    // Tear down ImGui BEFORE the backend destroys the GPU device — the SDL_GPU ImGui backend
    // owns device-bound resources. bs_imgui_shutdown drains the GPU first and is a no-op if
    // ImGui never initialized.
    bs_imgui_shutdown();

    state.backend.shutdown(&state.backend);
    renderer_backend_destroy(&state.backend);
    state.initialized = FALSE;
    BS_LOG_INFO("Renderer shut down.");
}

void renderer_on_resize(u16 width, u16 height)
{
    if (!state.initialized)
    {
        return;
    }
    state.backend.on_resize(&state.backend, width, height);
}

b8 renderer_begin_frame(f32 dt)
{
    if (!state.initialized)
    {
        return FALSE;
    }

    if (!state.backend.begin_frame(&state.backend, dt))
    {
        // Frame skipped (e.g. window minimized). Caller must NOT call end_frame.
        state.frame_active = FALSE;
        return FALSE;
    }

    state.frame_active = TRUE;
    return TRUE;
}

b8 renderer_end_frame(f32 dt)
{
    if (!state.initialized || !state.frame_active)
    {
        return FALSE;
    }

    b8 result = state.backend.end_frame(&state.backend, dt);
    state.frame_active = FALSE;
    return result;
}

void renderer_set_clear_color(bs_color color)
{
    state.clear_color = color;
    if (state.initialized)
    {
        state.backend.set_clear_color(&state.backend, color);
    }
}

// -------------------------------------------------------------------------------------
// Phase 3 — textures, camera, sprite batch (frontend forwarding + CPU-side image decode).
// -------------------------------------------------------------------------------------

bs_texture renderer_load_texture(const char* path)
{
    bs_texture invalid = bs_texture{ BS_INVALID_HANDLE };

    if (!state.initialized || !path)
    {
        BS_LOG_ERROR("renderer_load_texture: renderer not initialized or null path.");
        return invalid;
    }

    // Slurp the whole file into memory (stdio keeps the frontend free of SDL).
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        BS_LOG_ERROR("renderer_load_texture: could not open '%s'.", path);
        return invalid;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0)
    {
        BS_LOG_ERROR("renderer_load_texture: '%s' is empty or unseekable.", path);
        fclose(f);
        return invalid;
    }

    u8* file_bytes = (u8*)bs_memory_allocator((u64)file_size, EMemoryTag::MEMORY_TAG_TEXTURE);
    u64 read = (u64)fread(file_bytes, 1, (size_t)file_size, f);
    fclose(f);
    if (read != (u64)file_size)
    {
        BS_LOG_ERROR("renderer_load_texture: short read on '%s' (%llu/%ld).", path, read, file_size);
        bs_memory_free(file_bytes, (u64)file_size, EMemoryTag::MEMORY_TAG_TEXTURE);
        return invalid;
    }

    // Decode to tightly-packed RGBA8 (4 channels forced), top-left origin.
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(file_bytes, (int)file_size, &w, &h, &channels, 4);
    bs_memory_free(file_bytes, (u64)file_size, EMemoryTag::MEMORY_TAG_TEXTURE);

    if (!pixels || w <= 0 || h <= 0)
    {
        BS_LOG_ERROR("renderer_load_texture: stb failed to decode '%s' (%s).", path, stbi_failure_reason());
        if (pixels) stbi_image_free(pixels);
        return invalid;
    }

    bs_texture tex = state.backend.create_texture(&state.backend, (const u8*)pixels, (u32)w, (u32)h);
    stbi_image_free(pixels);

    if (tex.id == BS_INVALID_HANDLE)
    {
        BS_LOG_ERROR("renderer_load_texture: backend failed to create GPU texture for '%s'.", path);
        return invalid;
    }

    BS_LOG_INFO("Loaded texture '%s' (%dx%d) -> handle %u.", path, w, h, tex.id);
    return tex;
}

bs_texture renderer_create_texture(const u8* pixels, u32 width, u32 height)
{
    bs_texture invalid = bs_texture{ BS_INVALID_HANDLE };

    if (!state.initialized || !pixels || width == 0 || height == 0)
    {
        BS_LOG_ERROR("renderer_create_texture: renderer not initialized or invalid args.");
        return invalid;
    }

    // Hand the raw RGBA bytes straight to the backend (it owns all GPU/SDL types). No decode
    // step — the caller already has tightly-packed RGBA8 pixels.
    bs_texture tex = state.backend.create_texture(&state.backend, pixels, width, height);
    if (tex.id == BS_INVALID_HANDLE)
    {
        BS_LOG_ERROR("renderer_create_texture: backend failed to create %ux%u GPU texture.", width, height);
        return invalid;
    }

    BS_LOG_INFO("Created texture (%ux%u) -> handle %u.", width, height, tex.id);
    return tex;
}

void renderer_destroy_texture(bs_texture texture)
{
    if (!state.initialized || texture.id == BS_INVALID_HANDLE)
    {
        return;
    }
    state.backend.destroy_texture(&state.backend, texture);
}

void renderer_set_camera(Camera2D camera)
{
    if (!state.initialized)
    {
        return;
    }
    state.camera = camera;
    state.backend.set_camera(&state.backend, camera);
}

void renderer_draw_sprite(const bs_sprite* sprite)
{
    if (!state.initialized || !state.frame_active || !sprite)
    {
        return;
    }
    state.backend.draw_sprite(&state.backend, sprite);
}

// -------------------------------------------------------------------------------------
// Phase 4 — debug / vector layer. Every primitive lowers to one or more white-texture
// quads pushed through the existing sprite batch, so blending, layering, and the camera
// transform all come for free and identically to game sprites.
// -------------------------------------------------------------------------------------

void renderer_draw_line(bs_math::Vec2 a, bs_math::Vec2 b, f32 thickness,
                        bs_color color, u32 layer)
{
    if (!state.initialized || !state.frame_active) return;

    bs_math::Vec2 d = bs_math::vec2_sub(b, a);
    f32 len = bs_math::vec2_length(d);
    if (len <= 0.0001f) return; // degenerate segment: nothing to draw

    // `thickness` is specified in SCREEN PIXELS so debug primitives keep a constant on-screen
    // width at any zoom. The quad lives in world space and is scaled by the camera zoom in the
    // view transform, so divide out the zoom here to cancel it (1 px thick stays 1 px thick).
    f32 zoom = (state.camera.zoom != 0.0f) ? state.camera.zoom : 1.0f;
    f32 world_thickness = thickness / zoom;

    // Center the quad on the segment midpoint; size = (length, thickness); rotate to the
    // segment's direction. origin {0.5,0.5} keeps rotation about the midpoint.
    bs_sprite s;
    s.position = bs_math::Vec2{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    s.size     = bs_math::Vec2{ len, world_thickness };
    s.origin   = bs_math::Vec2{ 0.5f, 0.5f };
    s.rotation = atan2f(d.y, d.x);
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = color;
    s.texture  = bs_texture{ BS_INVALID_HANDLE }; // white texture => solid color
    s.blend    = (color.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
    s.layer    = layer;
    state.backend.draw_sprite(&state.backend, &s);
}

void renderer_draw_quad(bs_math::Vec2 center, bs_math::Vec2 size,
                        bs_color color, u32 layer)
{
    if (!state.initialized || !state.frame_active) return;

    bs_sprite s;
    s.position = center;
    s.size     = size;
    s.origin   = bs_math::Vec2{ 0.5f, 0.5f };
    s.rotation = 0.0f;
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = color;
    s.texture  = bs_texture{ BS_INVALID_HANDLE };
    s.blend    = (color.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
    s.layer    = layer;
    state.backend.draw_sprite(&state.backend, &s);
}

void renderer_draw_rect_outline(bs_math::Vec2 center, bs_math::Vec2 size,
                                f32 thickness, bs_color color, u32 layer)
{
    if (!state.initialized || !state.frame_active) return;

    f32 hx = size.x * 0.5f;
    f32 hy = size.y * 0.5f;
    bs_math::Vec2 tl{ center.x - hx, center.y + hy };
    bs_math::Vec2 tr{ center.x + hx, center.y + hy };
    bs_math::Vec2 br{ center.x + hx, center.y - hy };
    bs_math::Vec2 bl{ center.x - hx, center.y - hy };
    renderer_draw_line(tl, tr, thickness, color, layer);
    renderer_draw_line(tr, br, thickness, color, layer);
    renderer_draw_line(br, bl, thickness, color, layer);
    renderer_draw_line(bl, tl, thickness, color, layer);
}

void renderer_draw_circle(bs_math::Vec2 center, f32 radius, u32 segments,
                          f32 thickness, bs_color color, u32 layer)
{
    if (!state.initialized || !state.frame_active) return;
    if (radius <= 0.0f) return;

    if (segments < 6u)  segments = 6u;
    if (segments > 256u) segments = 256u;

    const f32 two_pi = 2.0f * bs_math::BS_PI;
    bs_math::Vec2 prev{ center.x + radius, center.y };
    for (u32 i = 1; i <= segments; ++i)
    {
        f32 t = (f32)i / (f32)segments * two_pi;
        bs_math::Vec2 cur{ center.x + cosf(t) * radius, center.y + sinf(t) * radius };
        renderer_draw_line(prev, cur, thickness, color, layer);
        prev = cur;
    }
}

void renderer_draw_grid(bs_math::Vec2 min, bs_math::Vec2 max, f32 spacing,
                        f32 thickness, bs_color color, u32 layer)
{
    if (!state.initialized || !state.frame_active) return;
    if (spacing <= 0.0f || max.x <= min.x || max.y <= min.y) return;

    // Cap the line count so a tiny spacing over a huge range can't blow the batch.
    f32 span_x = max.x - min.x;
    f32 span_y = max.y - min.y;
    if ((span_x / spacing) > 4096.0f || (span_y / spacing) > 4096.0f) return;

    for (f32 x = min.x; x <= max.x + 0.5f; x += spacing)
        renderer_draw_line(bs_math::Vec2{ x, min.y }, bs_math::Vec2{ x, max.y },
                           thickness, color, layer);
    for (f32 y = min.y; y <= max.y + 0.5f; y += spacing)
        renderer_draw_line(bs_math::Vec2{ min.x, y }, bs_math::Vec2{ max.x, y },
                           thickness, color, layer);
}

bs_frame_stats renderer_get_frame_stats()
{
    bs_frame_stats stats = { 0, 0 };
    if (!state.initialized || !state.backend.get_frame_stats) return stats;
    state.backend.get_frame_stats(&state.backend, &stats);
    return stats;
}
