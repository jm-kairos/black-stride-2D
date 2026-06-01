#pragma once

#include "vulkan_types.h"

#include "renderer/renderer_types.h"

b8 vulkan_renderer_server_initialize(THIS_RENDERER_SERVER_PTR renderer_server, const char* app_name, platform_state* plat_stat); 
void vulkan_renderer_server_terminate(THIS_RENDERER_SERVER_PTR renderer_server);
void vulkan_renderer_server_resized(THIS_RENDERER_SERVER_PTR renderer_server, u16 width, u16 height);
b8 vulkan_renderer_server_begin_frame(THIS_RENDERER_SERVER_PTR renderer_server, real dt);
b8 vulkan_renderer_server_end_frame(THIS_RENDERER_SERVER_PTR renderer_server, real dt);