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
            out_backend->initialize      = sdlgpu_backend_initialize;
            out_backend->shutdown        = sdlgpu_backend_shutdown;
            out_backend->on_resize       = sdlgpu_backend_on_resize;
            out_backend->begin_frame     = sdlgpu_backend_begin_frame;
            out_backend->end_frame       = sdlgpu_backend_end_frame;
            out_backend->set_clear_color = sdlgpu_backend_set_clear_color;
            out_backend->create_texture  = sdlgpu_backend_create_texture;
            out_backend->destroy_texture = sdlgpu_backend_destroy_texture;
            out_backend->set_camera      = sdlgpu_backend_set_camera;
            out_backend->draw_sprite     = sdlgpu_backend_draw_sprite;
            out_backend->get_frame_stats = sdlgpu_backend_get_frame_stats;
            return TRUE;
        default:
            BS_LOG_FATAL("renderer_backend_create: unknown backend type %d", (int)type);
            return FALSE;
    }
}

void renderer_backend_destroy(renderer_backend* backend)
{
    backend->initialize      = 0;
    backend->shutdown        = 0;
    backend->on_resize       = 0;
    backend->begin_frame     = 0;
    backend->end_frame       = 0;
    backend->set_clear_color = 0;
    backend->create_texture  = 0;
    backend->destroy_texture = 0;
    backend->set_camera      = 0;
    backend->draw_sprite     = 0;
    backend->get_frame_stats = 0;
    backend->internal_state  = 0;
}
