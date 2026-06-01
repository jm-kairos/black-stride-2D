#pragma once

#include "defines.h"
#include "vulkan_types.h"
#include "containers/vector.h"

#include "platform/platform.h"

b8 vulkan_platform_create_surface(platform_state* plat_state, __VulkanContext* context);

void vulkan_platform_get_required_extension_names(Vector(const char*)& ext_names_vector);