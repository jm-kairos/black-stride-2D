#pragma once

#include "vulkan_types.h"

void vulkan_renderpass_create(
    __VulkanContext* context,
    ImageRenderArea render_area,
    ClearColor clear_color,
    f32 depth,
    u32 stencil,
    __pass::__VulkanRenderPass* out_render_pass);

void vulkan_renderpass_destroy(
    __VulkanContext* context,
    __pass::__VulkanRenderPass* render_pass);

void vulkan_renderpass_begin(
    __pass::__VulkanRenderPass* render_pass,
    __cmd_buffer::__VulkanCommandBuffer* command_buffer,
    VkFramebuffer frame_buffer);

void vulkan_renderpass_end(
    __pass::__VulkanRenderPass* render_pass,
    __cmd_buffer::__VulkanCommandBuffer* command_buffer);
