#include "renderer_server.h"

#include "vulkan/vulkan_server.h"

b8 renderer_server_create(ERendererServerTypes type, platform_state *plat_state, RendererServer *out_renderer_server)
{
    // Fill RendererServer* object.

    out_renderer_server->plat_stat = plat_state;

    if (type == RENDERER_SERVER_TYPE_VULKAN)
    {
        // Attribute vulkan_renderer_server_(...) functions to the function pointers of RendererServer.
        out_renderer_server->initialize = vulkan_renderer_server_initialize;
        out_renderer_server->terminate = vulkan_renderer_server_terminate;
        out_renderer_server->begin_frame = vulkan_renderer_server_begin_frame;
        out_renderer_server->end_frame = vulkan_renderer_server_end_frame;
        out_renderer_server->resized = vulkan_renderer_server_resized;

        return TRUE;
    }
    
    return FALSE;
}

void renderer_server_destroy(RendererServer *renderer_server)
{
    renderer_server->initialize = 0;
    renderer_server->terminate = 0;
    renderer_server->begin_frame = 0;
    renderer_server->end_frame = 0;
    renderer_server->resized = 0;
}
