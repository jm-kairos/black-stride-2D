#include "renderer_client.h"

#include "renderer_server.h"

#include "core/logger.h"
#include "core/memory/ibx_memory.h"

// Server render context.
static THIS_RENDERER_SERVER_PTR renderer_server = { };

b8 renderer_initialize(const char* app_name, platform_state* plat_state){
    renderer_server = (RendererServer*)ibx_memory_allocator(sizeof(RendererServer), MEMORY_TAG_RENDERER);
    
    // TODO: this shall be configurable
    renderer_server_create(RENDERER_SERVER_TYPE_VULKAN, plat_state, renderer_server);
    
    renderer_server->frame_number = 0;
    
    if (!renderer_server->initialize(renderer_server, app_name, plat_state))
    {
        IBX_LOG_FATAL("Renderer server failed to initialize. Shutting down.")
        return FALSE;
    }

    return TRUE;
}

void renderer_terminate(){
    renderer_server->terminate(renderer_server);
    if (renderer_server)
    {
        ibx_memory_free(renderer_server, sizeof(RendererServer), MEMORY_TAG_RENDERER);
        renderer_server = (RendererServer*)0;
    }
}

b8 renderer_begin_frame(f32 dt){
    return renderer_server->begin_frame(renderer_server, dt);
}

b8 renderer_end_frame(f32 dt){
    b8 result = renderer_server->end_frame(renderer_server, dt);
    renderer_server->frame_number++;
    return result;
}

b8 renderer_draw_frame(RenderPacket* rp){
    if (renderer_begin_frame(rp->dt))
    {
        // End the fame. If it failed, it is likely unrecoverable.
        b8 result = renderer_end_frame(rp->dt);
        if (!result)
        {
            IBX_LOG_ERROR("renderer_end_frame failed. Application shutting down...")
            return FALSE;
        }
        
    }
    
    return TRUE;
}

void renderer_on_resize(u16 width, u16 height);