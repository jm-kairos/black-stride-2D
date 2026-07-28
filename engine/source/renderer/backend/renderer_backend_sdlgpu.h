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
b8         sdlgpu_backend_update_texture(struct renderer_backend* backend, bs_texture texture, const u8* pixels, u32 width, u32 height);
void       sdlgpu_backend_destroy_texture(struct renderer_backend* backend, bs_texture texture);
void       sdlgpu_backend_set_camera(struct renderer_backend* backend, Camera2D camera);
void       sdlgpu_backend_draw_sprite(struct renderer_backend* backend, const bs_sprite* sprite);
void       sdlgpu_backend_draw_mapped_sprite(struct renderer_backend* backend, const bs_mapped_sprite* sprite);
void       sdlgpu_backend_draw_starfield(struct renderer_backend* backend, const bs_starfield_params* params);
void       sdlgpu_backend_draw_sunburst(struct renderer_backend* backend, const bs_sunburst_params* params);
void       sdlgpu_backend_draw_starsurface(struct renderer_backend* backend, const bs_starsurface_params* params);
void       sdlgpu_backend_draw_planetsurface(struct renderer_backend* backend, const bs_planetsurface_params* params);
void       sdlgpu_backend_draw_heat_map(struct renderer_backend* backend, const bs_heat_map_params* params);
void       sdlgpu_backend_draw_nebula(struct renderer_backend* backend, const bs_nebula_params* params);

// 2D point lights: stored backend-side, packed and pushed as a fragment uniform per draw-run in
// end_frame. `ambient` is the scene-global floor; `unlit_layer` is the sprite-layer threshold at/
// above which runs render fullbright (HUD/UI). Lights beyond the backend cap are dropped.
void       sdlgpu_backend_set_lights(struct renderer_backend* backend, const bs_light2d* lights,
                                     u32 count, bs_color ambient, u32 unlit_layer);

void       sdlgpu_backend_set_glow_params(struct renderer_backend* backend, const bs_glow_params* params);

void       sdlgpu_backend_set_bloom_enabled(struct renderer_backend* backend, b8 enabled);
void       sdlgpu_backend_set_bloom_params(struct renderer_backend* backend, f32 threshold, f32 intensity);

void       sdlgpu_backend_set_streak_enabled(struct renderer_backend* backend, b8 enabled);
void       sdlgpu_backend_set_streak_params(struct renderer_backend* backend, f32 angle, f32 length);
void       sdlgpu_backend_set_streak_intensity(struct renderer_backend* backend, f32 intensity);
void       sdlgpu_backend_set_streak_source(struct renderer_backend* backend, bs_math::Vec2 screen_pos);
void       sdlgpu_backend_set_streak_flare_intensity(struct renderer_backend* backend, f32 intensity);
void       sdlgpu_backend_set_aux_bloom_mode(struct renderer_backend* backend, b8 enabled);

// Phase 4: frame statistics (snapshotted at end of end_frame).
void       sdlgpu_backend_get_frame_stats(struct renderer_backend* backend, bs_frame_stats* out_stats);

// Runtime swapchain present mode: FALSE = VSYNC (default), TRUE = IMMEDIATE (uncapped, for
// GPU-cost profiling). Returns FALSE and stays on VSYNC if IMMEDIATE is unsupported or the
// swapchain reconfigure fails.
b8         sdlgpu_backend_set_present_mode(struct renderer_backend* backend, b8 immediate);

// Present-cost breakdown of the previous end_frame (ms): swapchain-acquire block vs submit.
void       sdlgpu_backend_get_present_timing(struct renderer_backend* backend, f32* out_acquire_ms, f32* out_submit_ms);
