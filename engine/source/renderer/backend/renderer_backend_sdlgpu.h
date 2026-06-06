#pragma once

#include "defines.h"
#include "renderer/renderer_types.h"

struct renderer_backend;
struct PlatformState;

// =====================================================================================
// SDL3 GPU backend entry points. Declared here so the factory (renderer_backend.cpp) can
// reference them WITHOUT pulling in <SDL3/SDL_gpu.h>. The implementations — and the only
// inclusion of the SDL GPU header in the whole engine — live in renderer_backend_sdlgpu.cpp.
// =====================================================================================

b8   sdlgpu_backend_initialize(struct renderer_backend* backend, const char* app_name, struct PlatformState* plat);
void sdlgpu_backend_shutdown(struct renderer_backend* backend);
void sdlgpu_backend_on_resize(struct renderer_backend* backend, u16 width, u16 height);
b8   sdlgpu_backend_begin_frame(struct renderer_backend* backend, f32 dt);
b8   sdlgpu_backend_end_frame(struct renderer_backend* backend, f32 dt);
void sdlgpu_backend_set_clear_color(struct renderer_backend* backend, bs_color color);

// Phase 3: textures, camera, sprite batch.
bs_texture sdlgpu_backend_create_texture(struct renderer_backend* backend, const u8* pixels, u32 width, u32 height);
void       sdlgpu_backend_destroy_texture(struct renderer_backend* backend, bs_texture texture);
void       sdlgpu_backend_set_camera(struct renderer_backend* backend, Camera2D camera);
void       sdlgpu_backend_draw_sprite(struct renderer_backend* backend, const bs_sprite* sprite);

// Phase 4: frame statistics (snapshotted at end of end_frame).
void       sdlgpu_backend_get_frame_stats(struct renderer_backend* backend, bs_frame_stats* out_stats);
