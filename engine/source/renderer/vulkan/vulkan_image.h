#pragma once

#include "vulkan_types.h"

void vulkan_image_create(
    __VulkanContext* context,
    VkImageType type,
    u32 width,
    u32 height,
    VkFormat format,
    VkImageTiling tiling,
    VkImageUsageFlags usage,
    VkMemoryPropertyFlags memory_properties,
    b32 create_view,
    VkImageAspectFlags aspect_flags,
    __VulkanImage* out_image);

void vulkan_image_view_create(
    __VulkanContext* context,
    VkFormat format,
    __VulkanImage* image,
    VkImageAspectFlags aspect_flags);

void vulkan_image_destroy(__VulkanContext* context, __VulkanImage* image);