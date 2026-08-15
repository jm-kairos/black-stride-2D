#include "renderer/renderer_backend.h"
#include "renderer/backend/renderer_backend_sdlgpu.h"

#include "core/logger.h"

// Factory that wires the vtable for the requested backend. Keeps the frontend free of any
// knowledge of concrete backends beyond the ERendererBackend enum.

b8 renderer_backend_create(ERendererBackend type, renderer_backend* out_backend)
{
    out_backend->internal_state = 0;

    switch (type)
    {
        case RENDERER_BACKEND_SDL_GPU:
            out_backend->initialize = sdlgpu_backend_initialize;
            out_backend->shutdown = sdlgpu_backend_shutdown;
            out_backend->on_resize = sdlgpu_backend_on_resize;
            out_backend->begin_frame = sdlgpu_backend_begin_frame;
            out_backend->end_frame = sdlgpu_backend_end_frame;
            out_backend->set_clear_color = sdlgpu_backend_set_clear_color;
            out_backend->create_texture = sdlgpu_backend_create_texture;
            out_backend->update_texture = sdlgpu_backend_update_texture;
            out_backend->destroy_texture = sdlgpu_backend_destroy_texture;
            out_backend->set_camera = sdlgpu_backend_set_camera;
            out_backend->draw_sprite = sdlgpu_backend_draw_sprite;
            out_backend->draw_mapped_sprite = sdlgpu_backend_draw_mapped_sprite;
            out_backend->portrait_begin = sdlgpu_backend_portrait_begin;
            out_backend->portrait_end = sdlgpu_backend_portrait_end;
            out_backend->thumb_begin = sdlgpu_backend_thumb_begin;
            out_backend->draw_starfield = sdlgpu_backend_draw_starfield;
            out_backend->draw_sunburst = sdlgpu_backend_draw_sunburst;
            out_backend->draw_starsurface = sdlgpu_backend_draw_starsurface;
            out_backend->draw_planetsurface = sdlgpu_backend_draw_planetsurface;
            out_backend->draw_heat_map = sdlgpu_backend_draw_heat_map;
            out_backend->draw_nebula = sdlgpu_backend_draw_nebula;
            out_backend->draw_godrays = sdlgpu_backend_draw_godrays;
            out_backend->set_lights = sdlgpu_backend_set_lights;
            out_backend->set_glow_params = sdlgpu_backend_set_glow_params;
            out_backend->set_bloom_enabled = sdlgpu_backend_set_bloom_enabled;
            out_backend->set_bloom_params = sdlgpu_backend_set_bloom_params;
            out_backend->set_streak_enabled = sdlgpu_backend_set_streak_enabled;
            out_backend->set_streak_params = sdlgpu_backend_set_streak_params;
            out_backend->set_streak_intensity = sdlgpu_backend_set_streak_intensity;
            out_backend->set_streak_source = sdlgpu_backend_set_streak_source;
            out_backend->set_streak_flare_intensity = sdlgpu_backend_set_streak_flare_intensity;
            out_backend->set_aux_bloom_mode = sdlgpu_backend_set_aux_bloom_mode;
            out_backend->get_frame_stats = sdlgpu_backend_get_frame_stats;
            out_backend->set_present_mode = sdlgpu_backend_set_present_mode;
            out_backend->get_present_timing = sdlgpu_backend_get_present_timing;
            return TRUE;
        default:
            BS_LOG_FATAL("renderer_backend_create: unknown backend type %d", (int)type);
            return FALSE;
    }
}

void renderer_backend_destroy(renderer_backend* backend)
{
    backend->initialize = 0;
    backend->shutdown = 0;
    backend->on_resize = 0;
    backend->begin_frame = 0;
    backend->end_frame = 0;
    backend->set_clear_color = 0;
    backend->create_texture = 0;
    backend->update_texture = 0;
    backend->destroy_texture = 0;
    backend->set_camera = 0;
    backend->draw_sprite = 0;
    backend->draw_mapped_sprite = 0;
    backend->portrait_begin = 0;
    backend->portrait_end = 0;
    backend->thumb_begin = 0;
    backend->draw_starfield = 0;
    backend->draw_sunburst = 0;
    backend->draw_starsurface = 0;
    backend->draw_planetsurface = 0;
    backend->draw_heat_map = 0;
    backend->draw_nebula = 0;
    backend->draw_godrays = 0;
    backend->set_lights = 0;
    backend->set_glow_params = 0;
    backend->set_bloom_enabled = 0;
    backend->set_bloom_params = 0;
    backend->set_streak_enabled = 0;
    backend->set_streak_params = 0;
    backend->set_streak_intensity = 0;
    backend->set_streak_source = 0;
    backend->set_streak_flare_intensity = 0;
    backend->set_aux_bloom_mode = 0;
    backend->get_frame_stats = 0;
    backend->set_present_mode = 0;
    backend->get_present_timing = 0;
    backend->internal_state = 0;
}
