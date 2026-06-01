#pragma once

#include "renderer_types.h"
#include "renderer_server.h"

struct StaticMeshData;
struct platform_state;

b8 renderer_initialize(const char* app_name, platform_state* plat_state);
void renderer_terminate();

void renderer_on_resize(u16 width, u16 height);

b8 renderer_draw_frame(RenderPacket* rp);