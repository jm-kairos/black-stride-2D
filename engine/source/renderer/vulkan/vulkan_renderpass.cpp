#include "vulkan_renderpass.h"

#include "core/logger.h"  

#include "containers/array.h"
#include "containers/vector.h"

void vulkan_renderpass_create(
    __VulkanContext* context,
    ImageRenderArea render_area,
    ClearColor clear_color,
    f32 depth,
    u32 stencil,
    __pass::__VulkanRenderPass* out_render_pass)
{
    out_render_pass->render_area = render_area;
    out_render_pass->clear_color = clear_color;
    out_render_pass->depth = depth;
    out_render_pass->stencil = stencil;

    // Main subpass.
    VkSubpassDescription subpass_description = {};
    subpass_description.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

    // Attachment TODO: make this configurable.
    const size_t attachment_description_count = 2;
    Vector(VkAttachmentDescription) attachment_descriptions = {};
    attachment_descriptions.reserve(attachment_description_count);

    // Color attachment.
    VkAttachmentDescription color_attachment_description = {};
    color_attachment_description.format = context->swapchain.image_format.format; // TODO: make this configurable.
    color_attachment_description.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment_description.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment_description.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment_description.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color_attachment_description.flags = 0;

    attachment_descriptions.push_back(color_attachment_description);

    VkAttachmentReference color_attachment_reference = {};
    color_attachment_reference.attachment = 0;
    color_attachment_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    subpass_description.colorAttachmentCount = 1;
    subpass_description.pColorAttachments = &color_attachment_reference;

    // Depth attachment.
    VkAttachmentDescription depth_attachment_description = {};
    depth_attachment_description.format = context->device.depth_format;
    depth_attachment_description.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment_description.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment_description.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment_description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment_description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment_description.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment_description.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    attachment_descriptions.push_back(depth_attachment_description);

    VkAttachmentReference depth_attachment_reference = {};
    depth_attachment_reference.attachment = 1;
    depth_attachment_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // TODO: there are other attachment types.

    subpass_description.pDepthStencilAttachment = &depth_attachment_reference;

    // Input from a shader.
    subpass_description.colorAttachmentCount = 0;
    subpass_description.pColorAttachments = 0;

    // Attachments used for multisampling color attachments.
    subpass_description.pResolveAttachments = 0;

    // Attachments that are not used in this subpass but should be preserved.
    subpass_description.pPreserveAttachments = 0;

    // Render pass dependencies. TODO: make this configurable.
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = 0;

    VkRenderPassCreateInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.pNext = 0;
    render_pass_info.flags = 0;
    render_pass_info.attachmentCount = static_cast<u32>(attachment_descriptions.size());
    render_pass_info.pAttachments = attachment_descriptions.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass_description;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    VK_EVALUATE(vkCreateRenderPass(
        context->device.logical_device,
        &render_pass_info,
        context->allocator,
        &out_render_pass->handle))

    out_render_pass->state = __pass::EState::READY_FOR_RECORDING;
}

void vulkan_renderpass_destroy(
    __VulkanContext* context,
    __pass::__VulkanRenderPass* render_pass)
{
    if (render_pass->handle != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(context->device.logical_device, render_pass->handle, context->allocator);
        render_pass->handle = VK_NULL_HANDLE;
    }
    render_pass->state = __pass::EState::NOT_ALLOCATED;
}

void vulkan_renderpass_begin(
    __pass::__VulkanRenderPass* render_pass,
    __cmd_buffer::__VulkanCommandBuffer* command_buffer,
    VkFramebuffer frame_buffer)
{
    VkRenderPassBeginInfo render_pass_begin_info = {};
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = 0;
    render_pass_begin_info.renderPass = render_pass->handle;
    render_pass_begin_info.framebuffer = frame_buffer;
    render_pass_begin_info.renderArea.offset.x = render_pass->render_area.x;
    render_pass_begin_info.renderArea.offset.y = render_pass->render_area.y;
    render_pass_begin_info.renderArea.extent.width = render_pass->render_area.z;
    render_pass_begin_info.renderArea.extent.height = render_pass->render_area.w;

    Array(VkClearValue, 2) clear_values = {};
    clear_values[0].color.float32[0] = render_pass->clear_color.x;
    clear_values[0].color.float32[1] = render_pass->clear_color.y;
    clear_values[0].color.float32[2] = render_pass->clear_color.z;
    clear_values[0].color.float32[3] = render_pass->clear_color.w;

    clear_values[1].depthStencil.depth = render_pass->depth;
    clear_values[1].depthStencil.stencil = render_pass->stencil;

    render_pass_begin_info.clearValueCount = 2;
    render_pass_begin_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(
        command_buffer->handle,
        &render_pass_begin_info,
        VK_SUBPASS_CONTENTS_INLINE);

    command_buffer->state = __cmd_buffer::EState::IN_RENDER_PASS;
    render_pass->state = __pass::EState::IN_RENDER_PASS;
}

void vulkan_renderpass_end(
    __pass::__VulkanRenderPass* render_pass,
    __cmd_buffer::__VulkanCommandBuffer* command_buffer)
{   
    vkCmdEndRenderPass(command_buffer->handle);

    command_buffer->state = __cmd_buffer::EState::RECORDING;
    render_pass->state = __pass::EState::RECORDING;
}
