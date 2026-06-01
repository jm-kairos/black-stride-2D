#include "vulkan_server.h"

#include "core/logger.h"

#include "renderer/renderer_types.h"
#include "vulkan_platform.h"
#include "vulkan_device.h"
#include "vulkan_swapchain.h"
#include "vulkan_renderpass.h"
#include "vulkan_command_buffer.h"

#include "containers/vector.h" 
#include "containers/string.h"

#include "platform/platform.h"

static __VulkanContext context = {};

// Begin Private Functions.
i32 find_memory_index(u32 type_filter, u32 properties);
void create_command_buffers(THIS_RENDERER_SERVER_PTR renderer_server);
// End Private Functions.

b8 vulkan_renderer_server_initialize(THIS_RENDERER_SERVER_PTR renderer_server, const char *app_name, platform_state *plat_stat)
{  
    context.find_memory_index = find_memory_index;

    // TODO: custom allocator
    context.allocator = 0;

    VkApplicationInfo app_info;
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext = 0;
    app_info.pApplicationName = app_name;
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);;
    app_info.pEngineName = "Ibex Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo create_instance_info; 
    create_instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_instance_info.pNext = 0;
    create_instance_info.flags = 0;
    create_instance_info.pApplicationInfo = &app_info;

    // Process validation Layers in here.

    Vector(const char*) required_validation_layer_names = {};
    u32 required_validation_layer_count = 0;

#if defined(IBX_DEBUG) 
    IBX_LOG_INFO("Vulkan validation layers enabled. Enumerating...")

    // The list of validation layers required.
    required_validation_layer_names.push_back( "VK_LAYER_KHRONOS_validation" );
    required_validation_layer_count = required_validation_layer_names.size();
    
    // Obtain a list of available validation layers.
    u32 available_layer_count = 0;
    VK_EVALUATE(vkEnumerateInstanceLayerProperties(&available_layer_count, 0))
    
    Vector(VkLayerProperties) available_layers = {};
    available_layers.reserve(available_layer_count);
    VK_EVALUATE(vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data()))

    // Verify availability of all required layers.
    for (size_t i = 0; i < required_validation_layer_count; ++i)
    {
        IBX_LOG_INFO("Searching for layer: %s...", required_validation_layer_names[i])
        b8 found = FALSE;
        for (size_t j = 0; j < available_layer_count; ++j)
        {
            if (strcmp(required_validation_layer_names[i], available_layers[j].layerName))
            {
                found = TRUE;
                IBX_LOG_INFO("Found.")
                break;
            }
        }
        if (!found)
        {
            IBX_LOG_FATAL("Required validation layer is missing: %s", required_validation_layer_names[i]);
            return FALSE;
        }   
    }
    IBX_LOG_INFO("All required validation layers are present.")
#endif

    create_instance_info.enabledLayerCount = required_validation_layer_count;
    create_instance_info.ppEnabledLayerNames = required_validation_layer_names.data();

    Vector(const char*) required_extensions =  {};
    required_extensions.reserve(32);
    required_extensions.push_back( VK_KHR_SURFACE_EXTENSION_NAME ); // Generic surface extension.
    // Get platform specific extensions.
    vulkan_platform_get_required_extension_names(required_extensions);

#if defined(IBX_DEBUG)
    required_extensions.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME ); // Debug utilities.

    IBX_LOG_DEBUG("Required extensions:");
    const size_t length = required_extensions.size();

    for (size_t i = 0; i < length; i++)
        IBX_LOG_DEBUG(required_extensions[i])

#endif

    create_instance_info.enabledExtensionCount = required_extensions.size();
    create_instance_info.ppEnabledExtensionNames = required_extensions.data();

    VK_EVALUATE(vkCreateInstance(&create_instance_info, context.allocator, &context.instance))

#if defined(IBX_DEBUG)

    // TODO: Implement a Vulkan Debugger.

#endif

    if (!vulkan_platform_create_surface(plat_stat, &context))
    {
        IBX_LOG_ERROR("Failed to create VkSurface")
        return FALSE; 
    }
    
    // Device creation
    if(!vulkan_device_create(&context)){
        IBX_LOG_ERROR("Failed to create VkDevice.")
        return FALSE;
    }

    // Create swapchain.
    vulkan_swapchain_create(&context, 
        context.framebuffer_width, 
        context.framebuffer_height,
        &context.swapchain);

    ImageRenderArea render_area;
    render_area.x = 0;
    render_area.y = 0;
    render_area.z = (f32)context.framebuffer_width;
    render_area.w = (f32)context.framebuffer_height;

    ClearColor clear_color;
    clear_color.x = 0.1f;
    clear_color.y = 0.1f;
    clear_color.z = 0.1f;
    clear_color.w = 1.0f;

    vulkan_renderpass_create(
        &context,
        render_area,
        clear_color,
        1.0f,
        0,
        &context.main_render_pass);

    // Create command buffers.
    create_command_buffers(renderer_server);
    
    IBX_LOG_INFO("Vulkan renderer initialized successfully.")
    return TRUE;
}

void vulkan_renderer_server_terminate(THIS_RENDERER_SERVER_PTR renderer_server)
{
    IBX_LOG_DEBUG("Destroying Vulkan main render pass...")
    vulkan_renderpass_destroy(&context, &context.main_render_pass);
    IBX_LOG_DEBUG("Destroying Vulkan swapchain...")
    vulkan_swapchain_destroy(&context, &context.swapchain);
    IBX_LOG_DEBUG("Destroying Vulkan device...")
    vulkan_device_release(&context);
    IBX_LOG_DEBUG("Destroying Vulkan surface...")
    vkDestroySurfaceKHR(context.instance, context.surface, context.allocator); 
    IBX_LOG_DEBUG("Destroying Vulkan instance...")
    vkDestroyInstance(context.instance, context.allocator);
}

void vulkan_renderer_server_resized(THIS_RENDERER_SERVER_PTR renderer_server, u16 width, u16 height)
{
}

b8 vulkan_renderer_server_begin_frame(THIS_RENDERER_SERVER_PTR renderer_server, real dt)
{
    return b8();
}

b8 vulkan_renderer_server_end_frame(THIS_RENDERER_SERVER_PTR renderer_server, real dt)
{
    return b8();
}

i32 find_memory_index(u32 type_filter, u32 properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(context.device.chosen_gpu_device, &memory_properties);

    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    IBX_LOG_WARN("Failed to find suitable memory type for allocation. Type filter: %i, required properties: %i", type_filter, properties)
    return -1;
}

void create_command_buffers(THIS_RENDERER_SERVER_PTR renderer_server)
{
    // Why do we have more than one command buffer? 
    // We need one command buffer per frame in flight, 
    // so that we can record commands for the next frame while the current frame is being rendered.

    if (context.graphics_command_buffers.empty())
    {
        const size_t swapchain_image_count = (size_t)context.swapchain.image_count;
        context.graphics_command_buffers.reserve(swapchain_image_count);
        for (u32 i = 0; i < swapchain_image_count; ++i)
        {
            __cmd_buffer::__VulkanCommandBuffer cmd_buffer;
            cmd_buffer.handle = VK_NULL_HANDLE;
            cmd_buffer.state = __cmd_buffer::EState::NOT_ALLOCATED;
            context.graphics_command_buffers.push_back(cmd_buffer);
        }
    }
    
    for (auto& cmd_buffer : context.graphics_command_buffers)
    {
        if (cmd_buffer.handle != VK_NULL_HANDLE)
        {
            vulkan_command_buffer_free_to_pool(
                &context, 
                context.device.graphics_command_pool, 
                &cmd_buffer
            );
        }

        cmd_buffer.handle = VK_NULL_HANDLE;
        cmd_buffer.state = __cmd_buffer::EState::NOT_ALLOCATED;

        vulkan_command_buffer_allocate(
            &context, 
            context.device.graphics_command_pool, 
            TRUE,
            &cmd_buffer
        );
    }
}