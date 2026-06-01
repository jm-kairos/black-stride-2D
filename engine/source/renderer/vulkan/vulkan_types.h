#pragma once

#include "defines.h"
#include "core\asserts.h"

#include "containers/vector.h"

#include <vulkan/vulkan.h>

#include "math/math_utils.h"

typedef ibx_math::Vec4f32 ImageRenderArea;
typedef ibx_math::Vec4f32 ClearColor;
    
#define VK_EVALUATE(_result) { IBX_ASSERT(_result == VK_SUCCESS) }

struct __VulkanRenderPass;

struct __VulkanSwapchainSupportInfo{
    VkSurfaceCapabilitiesKHR capabilities;
    Vector(VkSurfaceFormatKHR) formats;
    Vector(VkPresentModeKHR) present_modes;
};
struct __VulkanDevice{
    VkPhysicalDevice chosen_gpu_device;
    VkDevice logical_device;
    __VulkanSwapchainSupportInfo swapchain_support_info;
    u32 graphics_queue_index;
    u32 present_queue_index;
    u32 transfer_queue_index;
    u32 compute_queue_index;
    // Queue handles.
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkQueue transfer_queue;
    VkQueue compute_queue;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceMemoryProperties memory;
    VkFormat depth_format;
};

struct __VulkanImage{
    VkImage handle;
    VkDeviceMemory memory;
    VkImageView view;
    u32 width;
    u32 height;
};

struct __VulkanSwapchain{
    VkSwapchainKHR handle;
    VkSurfaceFormatKHR image_format;
    u8 max_frames_in_flight;
    u32 image_count;
    VkImage* images;
    VkImageView* views;

    __VulkanImage depth_attachment;
};

namespace __cmd_buffer{

    typedef enum EVulkanCommandBufferState
    {
        READY_FOR_RECORDING,
        RECORDING,
        IN_RENDER_PASS,
        RECORDING_ENDED,
        SUBMITTED,
        NOT_ALLOCATED
    } EState;

    struct __VulkanCommandBuffer{
        VkCommandBuffer handle;
        EState state;
    };
}

namespace __pass{

    typedef enum EVulkanRenderPassState
    {
        READY_FOR_RECORDING,
        RECORDING,
        IN_RENDER_PASS,
        RECORDING_ENDED,
        SUBMITTED,
        NOT_ALLOCATED
    } EState;

    struct __VulkanRenderPass{
        VkRenderPass handle;
        ImageRenderArea render_area;
        ClearColor clear_color;
        f32 depth;
        u32 stencil;
        EState state;
    };
}

struct __VulkanContext{
    VkInstance instance;
    VkAllocationCallbacks* allocator;
    VkSurfaceKHR surface;
    __VulkanDevice device;
    u32 framebuffer_width;
    u32 framebuffer_height;
    __VulkanSwapchain swapchain;
    u32 image_index;
    u32 current_frame;
    b8 recreate_swapchain;
    __pass::__VulkanRenderPass main_render_pass;
    Vector(__cmd_buffer::__VulkanCommandBuffer) graphics_command_buffers;

    i32 (*find_memory_index)(u32 type_filter, u32 properties);
};