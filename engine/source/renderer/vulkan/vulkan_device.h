#pragma once

#include "vulkan_types.h"

void vulkan_physical_device_query_swapchain_support(
    VkPhysicalDevice device,
    VkSurfaceKHR surface, 
    __VulkanSwapchainSupportInfo* out_swapchain_support);

b8 vulkan_device_create(__VulkanContext* context);

void vulkan_device_release(__VulkanContext* context);

b8 vulkan_device_detect_depth_format(__VulkanDevice* device);