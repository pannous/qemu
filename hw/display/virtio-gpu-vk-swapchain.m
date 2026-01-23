/*
 * Virtio GPU Host Vulkan Swapchain for macOS
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Implements host-side Vulkan swapchain for Venus blob presentation
 * via MoltenVK on macOS.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/virtio/virtio-gpu.h"
#include "virtio-gpu-vk-swapchain.h"

#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

#define VK_SWAPCHAIN_IMAGE_COUNT 3
#define VK_CHECK(expr) do { \
    VkResult _res = (expr); \
    if (_res != VK_SUCCESS) { \
        error_report("%s: Vulkan error %d at %s:%d", __func__, _res, __FILE__, __LINE__); \
        return false; \
    } \
} while(0)

#define VK_CHECK_NULL(expr) do { \
    VkResult _res = (expr); \
    if (_res != VK_SUCCESS) { \
        error_report("%s: Vulkan error %d at %s:%d", __func__, _res, __FILE__, __LINE__); \
        return NULL; \
    } \
} while(0)

struct VirtIOGPUVkSwapchain {
    /* Vulkan instance and device */
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family_index;

    /* Surface and swapchain */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;

    /* Swapchain images */
    VkImage *images;
    uint32_t image_count;

    /* Synchronization */
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence in_flight;

    /* Command pool and buffer for blit operations */
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;

    /* Staging buffer for CPU-to-GPU transfer */
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    VkDeviceSize staging_size;

    /* Metal layer reference */
    CAMetalLayer *metal_layer;

    /* State */
    bool valid;
};

static bool find_queue_family(VkPhysicalDevice device, VkSurfaceKHR surface,
                               uint32_t *queue_family_index)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);

    VkQueueFamilyProperties *props = g_new0(VkQueueFamilyProperties, count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props);

    for (uint32_t i = 0; i < count; i++) {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);

        if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            *queue_family_index = i;
            g_free(props);
            return true;
        }
    }

    g_free(props);
    return false;
}

static uint32_t find_memory_type(VkPhysicalDevice physical_device,
                                  uint32_t type_filter,
                                  VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

static bool create_staging_buffer(VirtIOGPUVkSwapchain *s, VkDeviceSize size)
{
    /* Clean up existing buffer if any */
    if (s->staging_buffer != VK_NULL_HANDLE) {
        vkUnmapMemory(s->device, s->staging_memory);
        vkDestroyBuffer(s->device, s->staging_buffer, NULL);
        vkFreeMemory(s->device, s->staging_memory, NULL);
        s->staging_buffer = VK_NULL_HANDLE;
        s->staging_memory = VK_NULL_HANDLE;
        s->staging_mapped = NULL;
    }

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VK_CHECK(vkCreateBuffer(s->device, &buffer_info, NULL, &s->staging_buffer));

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(s->device, s->staging_buffer, &mem_reqs);

    uint32_t mem_type = find_memory_type(s->physical_device, mem_reqs.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        error_report("%s: failed to find suitable memory type", __func__);
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    VK_CHECK(vkAllocateMemory(s->device, &alloc_info, NULL, &s->staging_memory));
    VK_CHECK(vkBindBufferMemory(s->device, s->staging_buffer, s->staging_memory, 0));
    VK_CHECK(vkMapMemory(s->device, s->staging_memory, 0, size, 0, &s->staging_mapped));

    s->staging_size = size;
    return true;
}

static bool create_swapchain(VirtIOGPUVkSwapchain *s, uint32_t width, uint32_t height)
{
    /* Query surface capabilities */
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->physical_device,
                                                        s->surface, &caps));

    /* Choose extent */
    if (caps.currentExtent.width != UINT32_MAX) {
        s->extent = caps.currentExtent;
    } else {
        s->extent.width = CLAMP(width, caps.minImageExtent.width,
                                caps.maxImageExtent.width);
        s->extent.height = CLAMP(height, caps.minImageExtent.height,
                                 caps.maxImageExtent.height);
    }

    /* Choose image count */
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    /* Query surface format */
    uint32_t format_count;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface,
                                                   &format_count, NULL));
    VkSurfaceFormatKHR *formats = g_new0(VkSurfaceFormatKHR, format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->physical_device, s->surface,
                                          &format_count, formats);

    /* Prefer BGRA8 SRGB, fall back to first available */
    s->format = formats[0].format;
    VkColorSpaceKHR color_space = formats[0].colorSpace;
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            s->format = formats[i].format;
            color_space = formats[i].colorSpace;
            break;
        }
    }
    g_free(formats);

    /* Query present modes - prefer FIFO for VSync */
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface,
                                               &present_mode_count, NULL);
    VkPresentModeKHR *present_modes = g_new0(VkPresentModeKHR, present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s->physical_device, s->surface,
                                               &present_mode_count, present_modes);

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < present_mode_count; i++) {
        if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }
    g_free(present_modes);

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->surface,
        .minImageCount = image_count,
        .imageFormat = s->format,
        .imageColorSpace = color_space,
        .imageExtent = s->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = s->swapchain,
    };

    VkSwapchainKHR new_swapchain;
    VK_CHECK(vkCreateSwapchainKHR(s->device, &create_info, NULL, &new_swapchain));

    /* Destroy old swapchain if exists */
    if (s->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        g_free(s->images);
    }
    s->swapchain = new_swapchain;

    /* Get swapchain images */
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->image_count, NULL);
    s->images = g_new0(VkImage, s->image_count);
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->image_count, s->images);

    return true;
}

VirtIOGPUVkSwapchain *virtio_gpu_vk_swapchain_create(void *metal_layer,
                                                       uint32_t width,
                                                       uint32_t height)
{
    if (!metal_layer) {
        error_report("%s: metal_layer is NULL", __func__);
        return NULL;
    }

    VirtIOGPUVkSwapchain *s = g_new0(VirtIOGPUVkSwapchain, 1);
    s->metal_layer = (__bridge CAMetalLayer *)metal_layer;

    /* Create Vulkan instance with MoltenVK extensions */
    const char *instance_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "QEMU Venus Swapchain",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "QEMU",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ARRAY_SIZE(instance_extensions),
        .ppEnabledExtensionNames = instance_extensions,
    };

    VK_CHECK_NULL(vkCreateInstance(&instance_info, NULL, &s->instance));

    /* Create Metal surface */
    VkMetalSurfaceCreateInfoEXT surface_info = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = (__bridge const CAMetalLayer *)s->metal_layer,
    };

    PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT =
        (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(s->instance,
                                                            "vkCreateMetalSurfaceEXT");
    if (!vkCreateMetalSurfaceEXT) {
        error_report("%s: vkCreateMetalSurfaceEXT not found", __func__);
        goto error;
    }

    VK_CHECK_NULL(vkCreateMetalSurfaceEXT(s->instance, &surface_info, NULL,
                                           &s->surface));

    /* Select physical device */
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(s->instance, &device_count, NULL);
    if (device_count == 0) {
        error_report("%s: no Vulkan devices found", __func__);
        goto error;
    }

    VkPhysicalDevice *devices = g_new0(VkPhysicalDevice, device_count);
    vkEnumeratePhysicalDevices(s->instance, &device_count, devices);

    /* Find a device that supports our surface */
    for (uint32_t i = 0; i < device_count; i++) {
        if (find_queue_family(devices[i], s->surface, &s->queue_family_index)) {
            s->physical_device = devices[i];
            break;
        }
    }
    g_free(devices);

    if (s->physical_device == VK_NULL_HANDLE) {
        error_report("%s: no suitable Vulkan device found", __func__);
        goto error;
    }

    /* Create logical device */
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s->queue_family_index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        "VK_KHR_portability_subset",
    };

    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = ARRAY_SIZE(device_extensions),
        .ppEnabledExtensionNames = device_extensions,
    };

    VK_CHECK_NULL(vkCreateDevice(s->physical_device, &device_info, NULL,
                                  &s->device));

    vkGetDeviceQueue(s->device, s->queue_family_index, 0, &s->queue);

    /* Create command pool and buffer */
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = s->queue_family_index,
    };

    VK_CHECK_NULL(vkCreateCommandPool(s->device, &pool_info, NULL,
                                       &s->command_pool));

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VK_CHECK_NULL(vkAllocateCommandBuffers(s->device, &alloc_info,
                                            &s->command_buffer));

    /* Create synchronization objects */
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    VK_CHECK_NULL(vkCreateSemaphore(s->device, &sem_info, NULL,
                                     &s->image_available));
    VK_CHECK_NULL(vkCreateSemaphore(s->device, &sem_info, NULL,
                                     &s->render_finished));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VK_CHECK_NULL(vkCreateFence(s->device, &fence_info, NULL, &s->in_flight));

    /* Create swapchain */
    if (!create_swapchain(s, width, height)) {
        goto error;
    }

    /* Create initial staging buffer */
    VkDeviceSize initial_size = width * height * 4;
    if (!create_staging_buffer(s, initial_size)) {
        goto error;
    }

    s->valid = true;
    return s;

error:
    virtio_gpu_vk_swapchain_destroy(s);
    return NULL;
}

void virtio_gpu_vk_swapchain_destroy(VirtIOGPUVkSwapchain *s)
{
    if (!s) {
        return;
    }

    if (s->device) {
        vkDeviceWaitIdle(s->device);

        if (s->staging_buffer != VK_NULL_HANDLE) {
            vkUnmapMemory(s->device, s->staging_memory);
            vkDestroyBuffer(s->device, s->staging_buffer, NULL);
            vkFreeMemory(s->device, s->staging_memory, NULL);
        }

        if (s->in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(s->device, s->in_flight, NULL);
        }
        if (s->render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(s->device, s->render_finished, NULL);
        }
        if (s->image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(s->device, s->image_available, NULL);
        }

        if (s->command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(s->device, s->command_pool, NULL);
        }

        if (s->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
        }

        vkDestroyDevice(s->device, NULL);
    }

    if (s->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(s->instance, s->surface, NULL);
    }

    if (s->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(s->instance, NULL);
    }

    g_free(s->images);
    g_free(s);
}

bool virtio_gpu_vk_swapchain_resize(VirtIOGPUVkSwapchain *s,
                                     uint32_t width, uint32_t height)
{
    if (!s || !s->valid) {
        return false;
    }

    vkDeviceWaitIdle(s->device);

    if (!create_swapchain(s, width, height)) {
        s->valid = false;
        return false;
    }

    /* Update staging buffer if needed */
    VkDeviceSize needed_size = width * height * 4;
    if (needed_size > s->staging_size) {
        if (!create_staging_buffer(s, needed_size)) {
            s->valid = false;
            return false;
        }
    }

    return true;
}

bool virtio_gpu_vk_swapchain_present(VirtIOGPUVkSwapchain *s,
                                      void *blob_data,
                                      struct virtio_gpu_framebuffer *fb)
{
    if (!s || !s->valid || !blob_data || !fb) {
        return false;
    }

    if (getenv("VKR_PRESENT_DEBUG")) {
        uint32_t *p = (uint32_t *)((uint8_t *)blob_data + fb->offset);
        fprintf(stderr, "swapchain debug: fmt=%u stride=%u w=%u h=%u first_pixel=0x%08x\n",
                fb->format, fb->stride, fb->width, fb->height, p ? *p : 0);
        FILE *f = fopen("/tmp/vkr_hostptr.log", "a");
        if (f) {
            fprintf(f, "swapchain debug: fmt=%u stride=%u w=%u h=%u first_pixel=0x%08x\n",
                    fb->format, fb->stride, fb->width, fb->height, p ? *p : 0);
            fclose(f);
        }
    }

    /* Wait for previous frame */
    vkWaitForFences(s->device, 1, &s->in_flight, VK_TRUE, UINT64_MAX);

    /* Acquire next image */
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX,
                                             s->image_available, VK_NULL_HANDLE,
                                             &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!virtio_gpu_vk_swapchain_resize(s, s->extent.width, s->extent.height)) {
            return false;
        }
        return virtio_gpu_vk_swapchain_present(s, blob_data, fb);
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        error_report("%s: vkAcquireNextImageKHR failed: %d", __func__, result);
        return false;
    }

    vkResetFences(s->device, 1, &s->in_flight);

    /* Copy blob data to staging buffer */
    VkDeviceSize copy_size = fb->stride * fb->height;
    if (copy_size > s->staging_size) {
        if (!create_staging_buffer(s, copy_size)) {
            return false;
        }
    }
    memcpy(s->staging_mapped, (uint8_t *)blob_data + fb->offset, copy_size);

    /* Record command buffer */
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkResetCommandBuffer(s->command_buffer, 0);
    VK_CHECK(vkBeginCommandBuffer(s->command_buffer, &begin_info));

    /* Transition swapchain image to transfer dst */
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s->images[image_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(s->command_buffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    /* Copy from staging buffer to swapchain image */
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = fb->stride / fb->bytes_pp,
        .bufferImageHeight = fb->height,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { fb->width, fb->height, 1 },
    };

    vkCmdCopyBufferToImage(s->command_buffer, s->staging_buffer,
                            s->images[image_index],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &region);

    /* Transition to present layout */
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    vkCmdPipelineBarrier(s->command_buffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VK_CHECK(vkEndCommandBuffer(s->command_buffer));

    /* Submit */
    VkPipelineStageFlags wait_stages[] = {
        VK_PIPELINE_STAGE_TRANSFER_BIT
    };

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s->image_available,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &s->command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &s->render_finished,
    };

    VK_CHECK(vkQueueSubmit(s->queue, 1, &submit_info, s->in_flight));

    /* Present */
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &s->swapchain,
        .pImageIndices = &image_index,
    };

    result = vkQueuePresentKHR(s->queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        virtio_gpu_vk_swapchain_resize(s, s->extent.width, s->extent.height);
    } else if (result != VK_SUCCESS) {
        error_report("%s: vkQueuePresentKHR failed: %d", __func__, result);
        return false;
    }

    return true;
}

bool virtio_gpu_vk_swapchain_is_valid(VirtIOGPUVkSwapchain *s)
{
    return s && s->valid;
}

void virtio_gpu_vk_swapchain_get_size(VirtIOGPUVkSwapchain *s,
                                       uint32_t *width, uint32_t *height)
{
    if (s && s->valid) {
        *width = s->extent.width;
        *height = s->extent.height;
    } else {
        *width = 0;
        *height = 0;
    }
}
