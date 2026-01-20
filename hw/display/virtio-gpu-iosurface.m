/*
 * Virtio GPU IOSurface support for macOS
 *
 * Copyright (c) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides IOSurface-based scanout for virtio-gpu on macOS.
 * This is the macOS equivalent of dmabuf on Linux.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-gpu.h"
#include "ui/console.h"
#include "virtio-gpu-iosurface.h"

#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

/*
 * Map pixman format to IOSurface pixel format (CoreVideo/CoreGraphics format)
 */
static OSType pixman_to_iosurface_format(pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_x8r8g8b8:
    case PIXMAN_a8r8g8b8:
        return 'BGRA';  /* kCVPixelFormatType_32BGRA */
    case PIXMAN_x8b8g8r8:
    case PIXMAN_a8b8g8r8:
        return 'RGBA';  /* kCVPixelFormatType_32RGBA */
    case PIXMAN_r8g8b8:
        return '24RG';  /* kCVPixelFormatType_24RGB */
    case PIXMAN_b8g8r8:
        return '24BG';  /* kCVPixelFormatType_24BGR */
    default:
        return 0;
    }
}

/*
 * Create an IOSurface for scanout with given dimensions and format.
 */
IOSurfaceRef virtio_gpu_create_iosurface(uint32_t width, uint32_t height,
                                          uint32_t stride,
                                          pixman_format_code_t format)
{
    OSType pixel_format = pixman_to_iosurface_format(format);
    if (!pixel_format) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unsupported pixman format 0x%x for IOSurface\n",
                      __func__, format);
        return NULL;
    }

    uint32_t bytes_pp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(format), 8);

    /* Use CFDictionary with CFNumber values (pure C API) */
    const void *keys[] = {
        kIOSurfaceWidth,
        kIOSurfaceHeight,
        kIOSurfaceBytesPerElement,
        kIOSurfaceBytesPerRow,
        kIOSurfacePixelFormat,
        kIOSurfaceAllocSize,
    };

    CFNumberRef width_num = CFNumberCreate(NULL, kCFNumberIntType, &width);
    CFNumberRef height_num = CFNumberCreate(NULL, kCFNumberIntType, &height);
    CFNumberRef bpe_num = CFNumberCreate(NULL, kCFNumberIntType, &bytes_pp);
    CFNumberRef bpr_num = CFNumberCreate(NULL, kCFNumberIntType, &stride);
    CFNumberRef fmt_num = CFNumberCreate(NULL, kCFNumberIntType, &pixel_format);
    uint32_t alloc_size = stride * height;
    CFNumberRef size_num = CFNumberCreate(NULL, kCFNumberIntType, &alloc_size);

    const void *values[] = {
        width_num, height_num, bpe_num, bpr_num, fmt_num, size_num,
    };

    CFDictionaryRef properties = CFDictionaryCreate(
        NULL, keys, values, 6,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    IOSurfaceRef surface = IOSurfaceCreate(properties);

    CFRelease(properties);
    CFRelease(width_num);
    CFRelease(height_num);
    CFRelease(bpe_num);
    CFRelease(bpr_num);
    CFRelease(fmt_num);
    CFRelease(size_num);

    if (!surface) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to create IOSurface %dx%d\n",
                      __func__, width, height);
    }

    return surface;
}

/*
 * Update IOSurface contents from blob memory.
 * This copies pixels from the blob to the IOSurface.
 */
bool virtio_gpu_update_iosurface(IOSurfaceRef surface,
                                  void *blob_data,
                                  uint32_t width, uint32_t height,
                                  uint32_t src_stride, uint32_t src_offset)
{
    if (!surface || !blob_data) {
        return false;
    }

    IOReturn ret = IOSurfaceLock(surface, 0, NULL);
    if (ret != kIOReturnSuccess) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to lock IOSurface: %d\n",
                      __func__, ret);
        return false;
    }

    void *dst = IOSurfaceGetBaseAddress(surface);
    size_t dst_stride = IOSurfaceGetBytesPerRow(surface);
    size_t dst_height = IOSurfaceGetHeight(surface);
    uint8_t *src = (uint8_t *)blob_data + src_offset;

    /* Copy row by row to handle different strides */
    size_t copy_height = MIN(height, dst_height);
    size_t row_bytes = MIN(src_stride, dst_stride);

    for (size_t y = 0; y < copy_height; y++) {
        memcpy((uint8_t *)dst + y * dst_stride,
               src + y * src_stride,
               row_bytes);
    }

    IOSurfaceUnlock(surface, 0, NULL);
    return true;
}

/*
 * Release an IOSurface.
 */
void virtio_gpu_release_iosurface(IOSurfaceRef surface)
{
    if (surface) {
        CFRelease(surface);
    }
}

/*
 * Get IOSurface dimensions.
 */
void virtio_gpu_get_iosurface_size(IOSurfaceRef surface,
                                    uint32_t *width, uint32_t *height)
{
    if (surface) {
        *width = (uint32_t)IOSurfaceGetWidth(surface);
        *height = (uint32_t)IOSurfaceGetHeight(surface);
    } else {
        *width = 0;
        *height = 0;
    }
}
