#include "vulkan_device.h"

#include "core/logger.h"
#include "containers/vector.h"

struct VulkanPhysicalDeviceRequirements
{
    b8 graphics;
    b8 present;
    b8 compute;
    b8 transfer;
    Vector(const char*) device_extension_names;
    b8 sampler_anisotropy;
    b8 discrete_gpu;
};

struct VulkanPhysicalDeviceQueueFamilyInfo
{
    u32 graphics_family_index;
    u32 present_family_index;
    u32 compute_family_index;
    u32 transfer_family_index;
};

b8 vulkan_check_physical_device_suitability(
    VkPhysicalDevice device,
    VkSurfaceKHR surface,
    const VkPhysicalDeviceProperties* properties,
    const VkPhysicalDeviceFeatures* features,
    const VulkanPhysicalDeviceRequirements* requirements,
    VulkanPhysicalDeviceQueueFamilyInfo* out_queue_family_info,
    __VulkanSwapchainSupportInfo* out_swapchain_support);

b8 vulkan_select_physical_device(__VulkanContext* context){

    u32 physical_devices_count = 0;
    VK_EVALUATE(vkEnumeratePhysicalDevices(context->instance, &physical_devices_count, 0));

    Vector(VkPhysicalDevice) physical_devices = {};
    physical_devices.reserve(physical_devices_count);

    VK_EVALUATE(vkEnumeratePhysicalDevices(context->instance, &physical_devices_count, physical_devices.data()));

    IBX_LOG_INFO("Searching for a suitable Physical Device:");
    b8 found = FALSE;
    for (u32 i = 0; i < physical_devices_count; ++i)
    {
        VkPhysicalDeviceProperties properties;
        VkPhysicalDeviceFeatures features;
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceProperties(physical_devices[i], &properties);
        vkGetPhysicalDeviceFeatures(physical_devices[i], &features);
        vkGetPhysicalDeviceMemoryProperties(physical_devices[i], &memory);

        VulkanPhysicalDeviceRequirements requirements;
        requirements.compute = TRUE;
        requirements.device_extension_names = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        requirements.discrete_gpu = TRUE;
        requirements.graphics = TRUE;
        requirements.present = TRUE;
        requirements.sampler_anisotropy = TRUE;
        requirements.transfer = TRUE;

        VulkanPhysicalDeviceQueueFamilyInfo queue_family_info = {};

        b8 is_suitable = vulkan_check_physical_device_suitability(
            physical_devices[i],
            context->surface,
            &properties,
            &features,
            &requirements,
            &queue_family_info,
            &context->device.swapchain_support_info);

        if (is_suitable)
        {
            IBX_LOG_INFO("Selected physical device: %s", properties.deviceName);
            switch (properties.deviceType)
            {
            case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                IBX_LOG_INFO("gpu type is unknown.")
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                IBX_LOG_INFO("gpu type is integrated.")
                break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                IBX_LOG_INFO("gpu type is discrete.")
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                IBX_LOG_INFO("gpu type is virtual.")
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                IBX_LOG_INFO("gpu type is cpu.")
                break;
            default:
                break; 
            }
            IBX_LOG_INFO("gpu driver version: %d.%d.%d",
            VK_VERSION_MAJOR(properties.driverVersion),
            VK_VERSION_MINOR(properties.driverVersion),
            VK_VERSION_PATCH(properties.driverVersion))
            // Vulkan API version.
            IBX_LOG_INFO("vulkan api version: %d.%d.%d",
            VK_VERSION_MAJOR(properties.apiVersion),
            VK_VERSION_MINOR(properties.apiVersion),
            VK_VERSION_PATCH(properties.apiVersion))

            // TODO: Extract memory information from memory object. See Episode 015.

            found = TRUE;

            context->device.chosen_gpu_device = physical_devices[i];
            context->device.graphics_queue_index = queue_family_info.graphics_family_index;
            context->device.present_queue_index = queue_family_info.present_family_index;
            context->device.transfer_queue_index = queue_family_info.transfer_family_index;
            context->device.compute_queue_index = queue_family_info.compute_family_index;
            context->device.properties = properties;
            context->device.features = features;
            context->device.memory = memory;

            break;
        }
    }

    if (!found)
    {
        IBX_LOG_FATAL("No suitable Physical devices found.");
        return FALSE;
    }

    return TRUE;
}

b8 vulkan_device_create(__VulkanContext* context){
    if (!vulkan_select_physical_device(context)){
        return FALSE;
    }

    IBX_LOG_INFO("Creating logical device.")
    
    // Do not create additional queues for shared indices.
    b8 present_shares_graphics_queue = context->device.graphics_queue_index == context->device.present_queue_index;
    b8 transfer_shares_graphics_queue = context->device.graphics_queue_index == context->device.transfer_queue_index;
    u32 index_count = 1; // There is at least one index.
    if (!present_shares_graphics_queue)
        index_count++;
    if (!transfer_shares_graphics_queue)
        index_count++;

    Vector(u32) indices = {};
    indices.reserve(index_count);

    u8 index = 0;
    indices[index++] = context->device.graphics_queue_index;
    if (!present_shares_graphics_queue)
        indices[index++] = context->device.present_queue_index;
    if (!transfer_shares_graphics_queue)
        indices[index++] = context->device.transfer_queue_index;

    Vector(VkDeviceQueueCreateInfo) queue_create_infos = {};
    queue_create_infos.reserve(index_count);

    for (size_t i = 0; i < index_count; ++i)
    {
        queue_create_infos[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_infos[i].queueFamilyIndex = indices[i];
        queue_create_infos[i].queueCount = 1;
        if (indices[i] == context->device.graphics_queue_index)
            queue_create_infos[i].queueCount = 2;
        queue_create_infos[i].flags = 0;
        queue_create_infos[i].pNext = 0;
        f32 queue_priority = 1.0f;
        queue_create_infos[i].pQueuePriorities = &queue_priority;
    }
    
    // Request device features.
    // TODO: should be config driven.
    VkPhysicalDeviceFeatures device_features = {};
    device_features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo device_create_info;

    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.pNext = 0;
    device_create_info.flags = 0;
    device_create_info.queueCreateInfoCount = index_count;
    device_create_info.pQueueCreateInfos = queue_create_infos.data();
    device_create_info.enabledExtensionCount = 1;
    const char * extension_names = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    device_create_info.ppEnabledExtensionNames = &extension_names;
    device_create_info.pEnabledFeatures = &device_features;

    // Deprecated attributes, set to 0.
    device_create_info.enabledLayerCount = 0;
    device_create_info.ppEnabledLayerNames = 0;

    VK_EVALUATE(vkCreateDevice(
        context->device.chosen_gpu_device,
        &device_create_info,
        context->allocator,
        &context->device.logical_device
    ))

    IBX_LOG_INFO("Logical device created.")
    IBX_LOG_INFO("Obtaining the Queue handles from device.")

    // Get queues.
    vkGetDeviceQueue(
        context->device.logical_device,
        context->device.graphics_queue_index,
        0,
        &context->device.graphics_queue
    );

    vkGetDeviceQueue(
        context->device.logical_device,
        context->device.present_queue_index,
        0,
        &context->device.present_queue
    );

    vkGetDeviceQueue(
        context->device.logical_device,
        context->device.transfer_queue_index,
        0,
        &context->device.transfer_queue
    );

    vkGetDeviceQueue(
        context->device.logical_device,
        context->device.compute_queue_index,
        0,
        &context->device.compute_queue
    );

    IBX_LOG_INFO("Queue handles obtained: graphics, present, transfer and compute.")

    return TRUE;
}

void vulkan_device_release(__VulkanContext* context){
    context->device.graphics_queue = 0;
    context->device.present_queue = 0;
    context->device.transfer_queue = 0;
    context->device.compute_queue = 0;
    context->device.graphics_queue_index = -1;
    context->device.present_queue_index = -1;
    context->device.transfer_queue_index = -1;
    context->device.compute_queue_index = -1;
    if (context->device.logical_device)
    {         
        IBX_LOG_INFO("Releasing logical device.")
        vkDestroyDevice(context->device.logical_device, context->allocator);
        context->device.logical_device = 0;
    }
    context->device.chosen_gpu_device = 0;
}

b8 vulkan_check_physical_device_suitability(
    VkPhysicalDevice device,
    VkSurfaceKHR surface,
    const VkPhysicalDeviceProperties* properties,
    const VkPhysicalDeviceFeatures* features,
    const VulkanPhysicalDeviceRequirements* requirements,
    VulkanPhysicalDeviceQueueFamilyInfo* out_queue_family_info,
    __VulkanSwapchainSupportInfo* out_swapchain_support)
{
    out_queue_family_info->compute_family_index = -1;
    out_queue_family_info->graphics_family_index = -1;
    out_queue_family_info->present_family_index = -1;
    out_queue_family_info->transfer_family_index = -1;

    if (requirements->discrete_gpu)
    {
        if (properties->deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            IBX_LOG_INFO("Physical device is not discrete. Skipping.")
            return FALSE;
        }
    }
    
    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, 0);

    Vector(VkQueueFamilyProperties) queue_families = {};
    queue_families.reserve(queue_family_count);

    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());

    // Look at each queue and see what queue it supports
    IBX_LOG_INFO("graphics | present | compute | name")
    u8 min_transfer_score = 255;
    for (u32 i = 0; i < queue_family_count; ++i)
    {
        u8 current_transfer_score = 0;
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            out_queue_family_info->graphics_family_index = i;
            ++current_transfer_score;
        }

        if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            out_queue_family_info->compute_family_index = i;
            ++current_transfer_score;
        }

        if (queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
        {
            if (current_transfer_score <= min_transfer_score)
            {
                min_transfer_score = current_transfer_score;
                out_queue_family_info->transfer_family_index = i;
            }
        } 

        VkBool32 supports_present = VK_FALSE;
        VK_EVALUATE(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supports_present));
        if (supports_present){
            out_queue_family_info->present_family_index = i;
        }
        
    }    
    IBX_LOG_INFO("      %d |      %d |      %d |      %d | %s",
    out_queue_family_info->graphics_family_index != -1,
    out_queue_family_info->present_family_index != -1,
    out_queue_family_info->compute_family_index != -1,
    out_queue_family_info->transfer_family_index != -1,
    properties->deviceName)

    b8 g = (!requirements->graphics || (requirements->graphics && out_queue_family_info->graphics_family_index != -1));
    b8 p = (!requirements->present || (requirements->present && out_queue_family_info->present_family_index != -1));
    b8 c = (!requirements->compute || (requirements->compute && out_queue_family_info->compute_family_index != -1));
    b8 t = (!requirements->transfer || (requirements->transfer && out_queue_family_info->transfer_family_index != -1));

    if (g && p && c && t)
    {
        IBX_LOG_INFO("Physical device meets the requirements.")
        IBX_LOG_TRACE("Graphics Family Index: %i", out_queue_family_info->graphics_family_index)
        IBX_LOG_TRACE("Present Family Index: %i", out_queue_family_info->present_family_index)
        IBX_LOG_TRACE("Transfer Family Index: %i", out_queue_family_info->transfer_family_index)
        IBX_LOG_TRACE("Compute Family Index: %i", out_queue_family_info->compute_family_index)
    
        vulkan_physical_device_query_swapchain_support(device, surface, out_swapchain_support);

        if (out_swapchain_support->formats.size() < 1 || out_swapchain_support->present_modes.size() < 1)
        {
            IBX_LOG_INFO("Physical device does not support swapchain. Skipping.")
            return FALSE; 
        }

        if (requirements->device_extension_names.data())
        {
            u32 available_extensions_count = 0;
            VK_EVALUATE(vkEnumerateDeviceExtensionProperties(device, 0, &available_extensions_count, 0))

            Vector(VkExtensionProperties) available_extensions = {};
            available_extensions.reserve(available_extensions_count);

            VK_EVALUATE(vkEnumerateDeviceExtensionProperties(device, 0, &available_extensions_count, available_extensions.data()))
            
            // Verify availability of all required layers.
            size_t required_extensions_count = requirements->device_extension_names.size();
            for (size_t i = 0; i < required_extensions_count; ++i)
            {
                b8 found = FALSE;
                for (size_t j = 0; j < available_extensions_count; ++j)
                {
                    if (strcmp(requirements->device_extension_names[i], available_extensions[j].extensionName))
                    {
                        found = TRUE;
                        break;
                    }
                }
                if (!found)
                {
                    IBX_LOG_INFO("Required extension not found: %s, skipping device.", requirements->device_extension_names[i]);
                    return FALSE;
                }   
            }
        }

        if (requirements->sampler_anisotropy && !features->samplerAnisotropy)
        {
            IBX_LOG_INFO("Device does not support samplerAnisotropy, skipping.")
            return FALSE;
        }

        return TRUE;
    }
    
    return FALSE;
}

void vulkan_physical_device_query_swapchain_support(
    VkPhysicalDevice device,
    VkSurfaceKHR surface, 
    __VulkanSwapchainSupportInfo* out_swapchain_support)
{
    IBX_LOG_DEBUG("Querying for Swapchain Support.")

    VK_EVALUATE(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &out_swapchain_support->capabilities))
    
    // Formats.
    u32 format_count = 0;
    VK_EVALUATE(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, 0));
    out_swapchain_support->formats.resize(format_count);
    VK_EVALUATE(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, out_swapchain_support->formats.data()));

    // Present modes.
    u32 present_modes_count = 0;
    VK_EVALUATE(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_modes_count, 0));
    out_swapchain_support->present_modes.resize(present_modes_count);
    VK_EVALUATE(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_modes_count, out_swapchain_support->present_modes.data()));
}

b8 vulkan_device_detect_depth_format(__VulkanDevice *device)
{
    VkFormat candidates[3] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    u32 flags = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    for (u32 i = 0; i < 3; ++i)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(device->chosen_gpu_device, candidates[i], &props);
        if ((props.linearTilingFeatures & flags) == flags)
        {
            device->depth_format = candidates[i];
            IBX_LOG_INFO("Depth format found: %d", candidates[i])
            return TRUE;
        }else if((props.optimalTilingFeatures & flags) == flags)
        {
            device->depth_format = candidates[i];
            IBX_LOG_INFO("Depth format found: %d", candidates[i])
            return TRUE;

        }
    }

    IBX_LOG_INFO("No suitable depth format found.")
    return FALSE;
}
