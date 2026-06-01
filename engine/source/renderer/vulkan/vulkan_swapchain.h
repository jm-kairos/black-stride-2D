#pragma once

#include "vulkan_types.h"

void vulkan_swapchain_create(
    __VulkanContext* context,
    u32 width,
    u32 height,
    __VulkanSwapchain* out_swapchain);

void vulkan_swapchain_recreate(
    __VulkanContext* context,
    u32 width,
    u32 height,
    __VulkanSwapchain* swapchain);

void vulkan_swapchain_destroy(
    __VulkanContext* context,
    __VulkanSwapchain* swapchain);

b8 vulkan_swapchain_acquire_next_image_index(
    __VulkanContext* context,
    __VulkanSwapchain* swapchain,
    u64 timeout_ns,
    VkSemaphore image_available_semaphore,
    u32* out_image_index);

b8 vulkan_swapchain_present(
    __VulkanContext* context,
    __VulkanSwapchain* swapchain,
    VkQueue graphics_queue,
    VkQueue present_queue,
    VkSemaphore render_complete_semaphore,
    u32 present_image_index);