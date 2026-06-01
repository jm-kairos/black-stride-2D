#include "vulkan_swapchain.h"

#include "core/logger.h"
#include "core/memory/ibx_memory.h"
#include "vulkan_device.h"
#include "vulkan_image.h"

#include "math/math_utils.h"

void create(__VulkanContext *context, 
    u32 width, 
    u32 height, 
    __VulkanSwapchain *swapchain);

void destroy(
    __VulkanContext *context, 
    __VulkanSwapchain *swapchain);

void vulkan_swapchain_create(
    __VulkanContext *context, 
    u32 width, 
    u32 height, 
    __VulkanSwapchain *out_swapchain)
{
    create(context, width, height, out_swapchain);
}

void vulkan_swapchain_recreate(
    __VulkanContext *context, 
    u32 width, 
    u32 height, 
    __VulkanSwapchain *swapchain)
{
    destroy(context, swapchain);
    create(context, width, height, swapchain);
}

void vulkan_swapchain_destroy(
    __VulkanContext *context, 
    __VulkanSwapchain *swapchain)
{
    destroy(context, swapchain);
}

b8 vulkan_swapchain_acquire_next_image_index(
    __VulkanContext *context, 
    __VulkanSwapchain *swapchain, 
    u64 timeout_ns,
    VkSemaphore image_available_semaphore, 
    VkFence fence, 
    u32 *out_image_index)
{
    // Returns out_image_index.
    VkResult result = vkAcquireNextImageKHR(
        context->device.logical_device, 
        swapchain->handle,
        timeout_ns, // If it does not acquire a next image within timeout_ns, return an error.
        image_available_semaphore, // Semaphore are used to sync GPU operations with other GPU operations.
        fence, // Fences are used to sync between app and operations happening on GPU.
        out_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        vulkan_swapchain_recreate(
            context, 
            context->framebuffer_width,
            context->framebuffer_height,
            swapchain);
        return FALSE;
    }else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        IBX_LOG_FATAL("Failed to acquire swapchain image.")
        return FALSE;
    }

    return TRUE;  
}

b8 vulkan_swapchain_present(
    __VulkanContext *context, 
    __VulkanSwapchain *swapchain, 
    VkQueue graphics_queue, 
    VkQueue present_queue, 
    VkSemaphore render_complete_semaphore, 
    u32 present_image_index)
{
    VkPresentInfoKHR present_info;
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext = 0;
    present_info.waitSemaphoreCount = 1; // We need to wait for the operation bound to render_complete_semaphore to complete
    present_info.pWaitSemaphores = &render_complete_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain->handle;
    present_info.pImageIndices = &present_image_index;
    present_info.pResults = 0;

    VkResult result = vkQueuePresentKHR(present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        // Swapchain is out of date, suboptimal or a framebuffer resize has occured. Trigger swapchain recreation.
        vulkan_swapchain_recreate(
            context, 
            context->framebuffer_width,
            context->framebuffer_height,
            swapchain);
    }else if(result != VK_SUCCESS){
        IBX_LOG_FATAL("Failed to present swapchain image.");
        return FALSE;
    }
    
    return TRUE;
}

void create(
    __VulkanContext *context, 
    u32 width, 
    u32 height, 
    __VulkanSwapchain *swapchain)
{
    VkExtent2D swapchain_extent = { width, height };
    swapchain->max_frames_in_flight = 2;

    // Choose a swap surface format.
    const size_t formats_count = context->device.swapchain_support_info.formats.size();
    b8 found = FALSE;
    for (size_t i = 0; i < formats_count; ++i)
    {
        VkSurfaceFormatKHR format = context->device.swapchain_support_info.formats[i];
        if (format.format == VK_FORMAT_B8G8R8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            swapchain->image_format = format;
            found = TRUE;
            break;
        }
    }

    if (!found)
    {
        swapchain->image_format = context->device.swapchain_support_info.formats[0];
    }

    const size_t present_modes_count = context->device.swapchain_support_info.present_modes.size();
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (size_t i = 0; i < present_modes_count; ++i)
    {
        VkPresentModeKHR mode = context->device.swapchain_support_info.present_modes[i];
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            present_mode = mode;
            break;
        }
    }  

    vulkan_physical_device_query_swapchain_support(
        context->device.chosen_gpu_device,
        context->surface,
        &context->device.swapchain_support_info);

    // Swapchain extent
    if (context->device.swapchain_support_info.capabilities.currentExtent.width != UINT32_MAX)
    {
        swapchain_extent = context->device.swapchain_support_info.capabilities.currentExtent;
    }
    
    VkExtent2D min = context->device.swapchain_support_info.capabilities.minImageExtent;
    VkExtent2D max = context->device.swapchain_support_info.capabilities.maxImageExtent;

    swapchain_extent.width = ibx_math::clamp(swapchain_extent.width, min.width, max.width);
    swapchain_extent.height = ibx_math::clamp(swapchain_extent.height, min.height, max.height);

    u32 image_count = context->device.swapchain_support_info.capabilities.minImageCount + 1;
    if (context->device.swapchain_support_info.capabilities.maxImageCount > 0 && image_count > context->device.swapchain_support_info.capabilities.maxImageCount)
    {
        image_count = context->device.swapchain_support_info.capabilities.maxImageCount;
    }

    // Swapchain create info.
    VkSwapchainCreateInfoKHR swapchain_create_info;

    // Set up the queue family indices.
    if (context->device.graphics_queue_index != context->device.present_queue_index)
    {
        u32 queue_family_indices[2] = { 
            context->device.graphics_queue_index, 
            context->device.present_queue_index 
        };
        swapchain_create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_create_info.queueFamilyIndexCount = 2;
        swapchain_create_info.pQueueFamilyIndices = queue_family_indices;
    } else {
        swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_create_info.queueFamilyIndexCount = 0;
        swapchain_create_info.pQueueFamilyIndices = 0;
    }
    
    swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_create_info.pNext = nullptr;
    swapchain_create_info.flags = 0;
    swapchain_create_info.surface = context->surface;
    swapchain_create_info.minImageCount = image_count;
    swapchain_create_info.imageFormat = swapchain->image_format.format;
    swapchain_create_info.imageColorSpace = swapchain->image_format.colorSpace;
    swapchain_create_info.imageExtent = swapchain_extent;
    swapchain_create_info.imageArrayLayers = 1;
    swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_create_info.preTransform = context->device.swapchain_support_info.capabilities.currentTransform;
    swapchain_create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_create_info.presentMode = present_mode;
    swapchain_create_info.clipped = VK_TRUE;
    swapchain_create_info.oldSwapchain = 0;

    VK_EVALUATE(vkCreateSwapchainKHR(
        context->device.logical_device,
        &swapchain_create_info,
        context->allocator,
        &swapchain->handle));

    context->current_frame = 0;
    
    swapchain->image_count = 0;
    VK_EVALUATE(vkGetSwapchainImagesKHR(
        context->device.logical_device,
        swapchain->handle,
        &swapchain->image_count,
        0));
    
    if (!swapchain->images)
    {
        swapchain->images = (VkImage*)ibx_memory_allocator(sizeof(VkImage) * swapchain->image_count, MEMORY_TAG_RENDERER);
    }

    if (!swapchain->views)
    {
        swapchain->views = (VkImageView*)ibx_memory_allocator(sizeof(VkImageView) * swapchain->image_count, MEMORY_TAG_RENDERER);
    }

    VK_EVALUATE(vkGetSwapchainImagesKHR(
        context->device.logical_device,
        swapchain->handle,
        &swapchain->image_count,
        swapchain->images));

    // Create image views for the swapchain images.
    for (u32 i = 0; i < swapchain->image_count; ++i)
    {
        VkImageViewCreateInfo create_info;
        create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        create_info.pNext = 0;
        create_info.flags = 0;
        create_info.image = swapchain->images[i];
        create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create_info.format = swapchain->image_format.format;
        create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        create_info.subresourceRange.baseMipLevel = 0;
        create_info.subresourceRange.levelCount = 1;
        create_info.subresourceRange.baseArrayLayer = 0;
        create_info.subresourceRange.layerCount = 1;

        VK_EVALUATE(vkCreateImageView(
            context->device.logical_device,
            &create_info,
            context->allocator,
            &swapchain->views[i]));
    }

    // Create image resource for the depth buffer.
    if(!vulkan_device_detect_depth_format(&context->device))
    {
        context->device.depth_format = VK_FORMAT_UNDEFINED;
        IBX_LOG_FATAL("Failed to find a supported depth format.")
    }

    // Create the depth image.
    vulkan_image_create(
        context,
        VK_IMAGE_TYPE_2D,
        swapchain_extent.width,
        swapchain_extent.height,
        context->device.depth_format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        TRUE,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        &swapchain->depth_attachment);

    IBX_LOG_INFO("Swapchain created.")
}

void destroy(
    __VulkanContext *context, 
    __VulkanSwapchain *swapchain)
{
    vulkan_image_destroy(context, &swapchain->depth_attachment);

    // Only destory the views, not the images, since the images are owned by the swapchain and will be destroyed when the swapchain is destroyed.
    for (u32 i = 0; i < swapchain->image_count; ++i)
    {
        vkDestroyImageView(context->device.logical_device, swapchain->views[i], context->allocator);
    }

    vkDestroySwapchainKHR(context->device.logical_device, swapchain->handle, context->allocator);
}
