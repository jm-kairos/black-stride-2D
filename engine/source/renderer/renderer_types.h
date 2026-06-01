#pragma once

#include "defines.h"

#include "platform/platform.h"

// Supported renderer backends.
typedef enum ERendererServerTypes{
    RENDERER_SERVER_TYPE_VULKAN,
    RENDERER_SERVER_TYPE_OPENGL,
    RENDERER_SERVER_TYPE_DIRECTX
} ERendererServerTypes;

#define THIS_RENDERER_SERVER_PTR RendererServer*

struct RendererServer
{
    platform_state* plat_stat;
    u64 frame_number;

    b8 (*initialize)(THIS_RENDERER_SERVER_PTR, const char* app_name, platform_state* plat_stat); 
    void (*terminate)(THIS_RENDERER_SERVER_PTR);
    void (*resized)(THIS_RENDERER_SERVER_PTR, u16 width, u16 height);
    b8 (*begin_frame)(THIS_RENDERER_SERVER_PTR, real dt);
    b8 (*end_frame)(THIS_RENDERER_SERVER_PTR, real dt);
};

struct RenderPacket
{
    f32 dt;
};

