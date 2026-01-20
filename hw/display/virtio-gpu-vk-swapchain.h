/*
 * Virtio GPU Host Vulkan Swapchain for macOS
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides host-side Vulkan swapchain for presenting Venus blob resources
 * to the display via MoltenVK on macOS. This bypasses the need for guest
 * swapchain support by intercepting scanout commands and presenting via
 * a host-managed Vulkan swapchain.
 */

#ifndef VIRTIO_GPU_VK_SWAPCHAIN_H
#define VIRTIO_GPU_VK_SWAPCHAIN_H

#ifdef __APPLE__

#include <stdint.h>
#include <stdbool.h>

/* Opaque handle to the Vulkan swapchain context */
typedef struct VirtIOGPUVkSwapchain VirtIOGPUVkSwapchain;

/* Forward declare framebuffer struct */
struct virtio_gpu_framebuffer;

/*
 * Create a host Vulkan swapchain for presentation.
 * metal_layer: Pointer to CAMetalLayer from Cocoa display
 * width, height: Initial dimensions
 * Returns: Swapchain handle or NULL on failure
 */
VirtIOGPUVkSwapchain *virtio_gpu_vk_swapchain_create(void *metal_layer,
                                                       uint32_t width,
                                                       uint32_t height);

/*
 * Destroy the Vulkan swapchain and free all resources.
 */
void virtio_gpu_vk_swapchain_destroy(VirtIOGPUVkSwapchain *swapchain);

/*
 * Resize the swapchain to new dimensions.
 * This recreates the swapchain with the new size.
 */
bool virtio_gpu_vk_swapchain_resize(VirtIOGPUVkSwapchain *swapchain,
                                     uint32_t width, uint32_t height);

/*
 * Present a blob resource to the swapchain.
 * This acquires a swapchain image, blits the blob content, and presents.
 * blob_data: Pointer to the mapped blob memory
 * fb: Framebuffer descriptor with format, stride, dimensions
 * Returns: true on success, false on failure
 */
bool virtio_gpu_vk_swapchain_present(VirtIOGPUVkSwapchain *swapchain,
                                      void *blob_data,
                                      struct virtio_gpu_framebuffer *fb);

/*
 * Check if the swapchain is valid and ready for presentation.
 */
bool virtio_gpu_vk_swapchain_is_valid(VirtIOGPUVkSwapchain *swapchain);

/*
 * Get the current swapchain dimensions.
 */
void virtio_gpu_vk_swapchain_get_size(VirtIOGPUVkSwapchain *swapchain,
                                       uint32_t *width, uint32_t *height);

#endif /* __APPLE__ */

#endif /* VIRTIO_GPU_VK_SWAPCHAIN_H */
