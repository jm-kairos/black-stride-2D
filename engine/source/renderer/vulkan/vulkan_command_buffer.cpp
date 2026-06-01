#include "vulkan_command_buffer.h"

#include "core/memory/ibx_memory.h"

void vulkan_command_buffer_allocate(
    __VulkanContext *context, 
    VkCommandPool command_pool, 
    b8 is_primary, 
    __VulkanCommandBuffer *out_command_buffer)
{
    ibx_memory_zero(out_command_buffer, sizeof(out_command_buffer));

    VkCommandBufferAllocateInfo allocate_info;
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.pNext = 0;
    allocate_info.commandPool = command_pool;
    // A Seconday buffer is used within a primary command buffer and cannot be submitted to a queue on its own.
    allocate_info.level = is_primary ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocate_info.commandBufferCount = 1; // number of allocated command buffers.

    out_command_buffer->state = EState::NOT_ALLOCATED;

    VK_EVALUATE(vkAllocateCommandBuffers(
        context->device.logical_device, 
        &allocate_info, 
        &out_command_buffer->handle))
    
    out_command_buffer->state = EState::READY_FOR_RECORDING;
}

void vulkan_command_buffer_free_to_pool(
    __VulkanContext *context, 
    VkCommandPool command_pool, 
    __VulkanCommandBuffer *command_buffer)
{
    vkFreeCommandBuffers(
        context->device.logical_device, 
        command_pool, 
        1, 
        &command_buffer->handle);

    command_buffer->handle = VK_NULL_HANDLE;
    command_buffer->state = EState::NOT_ALLOCATED;
}

void vulkan_command_buffer_begin_recording( 
    __VulkanCommandBuffer *command_buffer, 
    b8 is_single_use,
    b8 is_render_pass_continue,
    b8 is_shared_use)
{
    VkCommandBufferBeginInfo begin_info;
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.pNext = 0;
    begin_info.flags = 0;

    if (is_single_use)
    {
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    }
    if (is_render_pass_continue)
    {
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    }
    if (is_shared_use)
    {
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    }

    VK_EVALUATE(vkBeginCommandBuffer(command_buffer->handle, &begin_info))

    command_buffer->state = EState::RECORDING;
}

void vulkan_command_buffer_end_recording(__VulkanCommandBuffer *command_buffer)
{
    VK_EVALUATE(vkEndCommandBuffer(command_buffer->handle))

    command_buffer->state = EState::RECORDING_ENDED;
}

void vulkan_command_buffer_update_submitted(__VulkanCommandBuffer *command_buffer)
{
    command_buffer->state = EState::SUBMITTED;
}

void vulkan_command_buffer_reset_to_ready(__VulkanCommandBuffer *command_buffer)
{
    command_buffer->state = EState::READY_FOR_RECORDING;
}

void vulkan_command_buffer_allocate_begin_disposible(
    __VulkanContext *context,
    VkCommandPool command_pool, 
    __VulkanCommandBuffer *out_command_buffer)
{
    vulkan_command_buffer_allocate(context, command_pool, TRUE, out_command_buffer);
    vulkan_command_buffer_begin_recording(out_command_buffer, TRUE, FALSE, FALSE);
}

void vulkan_command_buffer_end_submit_disposible(
    __VulkanContext *context, 
    VkCommandPool command_pool, 
    __VulkanCommandBuffer *out_command_buffer, 
    VkQueue queue)
{
    vulkan_command_buffer_end_recording(out_command_buffer);

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = 0;
    submit_info.waitSemaphoreCount = 0;
    submit_info.pWaitSemaphores = 0;
    submit_info.pWaitDstStageMask = 0;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &out_command_buffer->handle;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = 0;

    VK_EVALUATE(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE))

    // Wait for it to finish.
    VK_EVALUATE(vkQueueWaitIdle(queue))

    // Free the command buffer back to the pool.
    vulkan_command_buffer_free_to_pool(context, command_pool, out_command_buffer);
}
