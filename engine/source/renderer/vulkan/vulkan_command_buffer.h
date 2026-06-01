#pragma once

#include "vulkan_types.h"

using namespace __cmd_buffer;

void vulkan_command_buffer_allocate(
    __VulkanContext* context,
    VkCommandPool command_pool,
    b8 is_primary,
    __VulkanCommandBuffer* out_command_buffer);

void vulkan_command_buffer_free_to_pool(
    __VulkanContext* context,
    VkCommandPool command_pool,
    __VulkanCommandBuffer* command_buffer);

void vulkan_command_buffer_begin_recording( 
    __VulkanCommandBuffer *command_buffer, 
    b8 is_single_use,
    b8 is_render_pass_continue,
    b8 is_shared_use);

void vulkan_command_buffer_end_recording(__VulkanCommandBuffer *command_buffer);

void vulkan_command_buffer_update_submitted(__VulkanCommandBuffer *command_buffer);

void vulkan_command_buffer_reset_to_ready(__VulkanCommandBuffer *command_buffer);

void vulkan_command_buffer_allocate_begin_disposible(
    __VulkanContext* context,
    VkCommandPool command_pool,
    __VulkanCommandBuffer* out_command_buffer);

void vulkan_command_buffer_end_submit_disposible(
    __VulkanContext* context,
    VkCommandPool command_pool,
    __VulkanCommandBuffer* out_command_buffer,
    VkQueue queue);