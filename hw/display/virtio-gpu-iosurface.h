/*
 * Virtio GPU IOSurface support for macOS
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VIRTIO_GPU_IOSURFACE_H
#define VIRTIO_GPU_IOSURFACE_H

#ifdef __APPLE__

#include <stdint.h>
#include <stdbool.h>
#include "ui/qemu-pixman.h"

/* Forward declare IOSurfaceRef to avoid including IOSurface.h in headers */
typedef struct __IOSurface *IOSurfaceRef;

IOSurfaceRef virtio_gpu_create_iosurface(uint32_t width, uint32_t height,
                                          uint32_t stride,
                                          pixman_format_code_t format);

bool virtio_gpu_update_iosurface(IOSurfaceRef surface,
                                  void *blob_data,
                                  uint32_t width, uint32_t height,
                                  uint32_t src_stride, uint32_t src_offset);

void virtio_gpu_release_iosurface(IOSurfaceRef surface);

void virtio_gpu_get_iosurface_size(IOSurfaceRef surface,
                                    uint32_t *width, uint32_t *height);

bool virtio_gpu_present_iosurface(IOSurfaceRef surface, void *metal_layer);

#endif /* __APPLE__ */

#endif /* VIRTIO_GPU_IOSURFACE_H */
