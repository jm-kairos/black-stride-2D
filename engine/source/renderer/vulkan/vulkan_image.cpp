#include "vulkan_image.h"

#include "vulkan_device.h"

#include "core/logger.h"
#include "core/memory/ibx_memory.h"

void vulkan_image_create(
    __VulkanContext *context, 
    VkImageType type, 
    u32 width, 
    u32 height, 
    VkFormat format, 
    VkImageTiling tiling, 
    VkImageUsageFlags usage, 
    VkMemoryPropertyFlags memory_prop_flags, 
    b32 create_view, 
    VkImageAspectFlags aspect_flags, 
    __VulkanImage *out_image)
{
    out_image->width = width;
    out_image->height = height;

    VkImageCreateInfo image_create_info;
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.pNext = 0;
    image_create_info.flags = 0;
    image_create_info.imageType = type;
    image_create_info.format = format;
    image_create_info.extent.width = width;
    image_create_info.extent.height = height;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 4;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = tiling;
    image_create_info.usage = usage;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.queueFamilyIndexCount = 0;
    image_create_info.pQueueFamilyIndices = 0;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_EVALUATE(vkCreateImage(
        context->device.logical_device, 
        &image_create_info, 
        context->allocator, 
        &out_image->handle));

    // Query memory requirements for this image.
    VkMemoryRequirements memory_requirements;
    vkGetImageMemoryRequirements(context->device.logical_device, out_image->handle, &memory_requirements);

    i32 memory_type = context->find_memory_index(memory_requirements.memoryTypeBits, memory_prop_flags);
    if (memory_type == -1)
    {
        IBX_LOG_ERROR("Required memory type not found for image allocation.");
    }

    // Allocate memory for the image.
    VkMemoryAllocateInfo memory_allocate_info;
    memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_allocate_info.pNext = 0;
    memory_allocate_info.allocationSize = memory_requirements.size;
    memory_allocate_info.memoryTypeIndex = (u32)memory_type;

    VK_EVALUATE(vkAllocateMemory(
        context->device.logical_device, 
        &memory_allocate_info, 
        context->allocator, 
        &out_image->memory));

    // Bind the memory.
    VK_EVALUATE(vkBindImageMemory(
        context->device.logical_device, 
        out_image->handle, 
        out_image->memory, 
        0)); // TODO: configurable memory offset.

    // Create view.
    if (create_view)
    {
        out_image->view = 0;
        vulkan_image_view_create(context, format, out_image, aspect_flags);
    }
}

void vulkan_image_view_create(
    __VulkanContext *context, 
    VkFormat format, 
    __VulkanImage *image, 
    VkImageAspectFlags aspect_flags)
{
    VkImageViewCreateInfo view_create_info;
    view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_create_info.pNext = 0;
    view_create_info.flags = 0;
    view_create_info.image = image->handle;
    view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_create_info.format = format;
    view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_create_info.subresourceRange.aspectMask = aspect_flags;
    view_create_info.subresourceRange.baseMipLevel = 0;
    view_create_info.subresourceRange.levelCount = 1;
    view_create_info.subresourceRange.baseArrayLayer = 0;
    view_create_info.subresourceRange.layerCount = 1;

    VK_EVALUATE(vkCreateImageView(
        context->device.logical_device, 
        &view_create_info, 
        context->allocator, 
        &image->view));
}

void vulkan_image_destroy(__VulkanContext *context, __VulkanImage *image)
{
    if (image->view)
    {
        vkDestroyImageView(context->device.logical_device, image->view, context->allocator);
        image->view = 0;
    }
    if (image->memory)
    {
        vkFreeMemory(context->device.logical_device, image->memory, context->allocator);
        image->memory = 0;
    }
    if (image->handle)
    {
        vkDestroyImage(context->device.logical_device, image->handle, context->allocator);
        image->handle = 0;
    }
}
