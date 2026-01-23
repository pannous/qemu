/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "qemu/timer.h"
#include "system/hvf.h"
#include <dlfcn.h>

#ifdef CONFIG_OPENGL
#include "ui/egl-helpers.h"
#endif

#ifdef __APPLE__
#include "virtio-gpu-iosurface.h"
#include "virtio-gpu-vk-swapchain.h"
#include <IOSurface/IOSurface.h>

/* Cocoa display exports for Metal layer access */
extern void *cocoa_get_metal_layer(void);
extern void cocoa_set_metal_layer_enabled(bool enabled);
#endif

#include <virglrenderer.h>

struct virtio_gpu_virgl_resource {
    struct virtio_gpu_simple_resource base;
    MemoryRegion *mr;
#ifdef __APPLE__
    IOSurfaceRef iosurface;
    uint32_t iosurface_id;
    uint32_t ctx_id;
    void *mapped_blob;      /* Blob pointer from virgl_renderer_resource_map */
    uint64_t mapped_size;   /* Size of mapped blob */
    /* Software scanout support for 2D resources without OpenGL */
    pixman_image_t *scanout_image;  /* Pixman image for software scanout */
    uint32_t scanout_stride;        /* Stride of scanout buffer */
#endif
};

static struct virtio_gpu_virgl_resource *
virtio_gpu_virgl_find_resource(VirtIOGPU *g, uint32_t resource_id);

typedef int (*virgl_renderer_resource_register_venus_fn)(uint32_t ctx_id,
                                                         uint32_t res_id);
typedef int (*virgl_renderer_resource_get_iosurface_id_fn)(uint32_t ctx_id,
                                                           uint32_t res_id,
                                                           uint32_t *out_id);
typedef int (*virgl_renderer_get_last_hostptr_fd_fn)(uint32_t ctx_id,
                                                     int *out_fd,
                                                     uint64_t *out_size);
typedef int (*virgl_renderer_get_hostptr_fd_for_size_fn)(uint32_t ctx_id,
                                                         uint64_t min_size,
                                                         int *out_fd,
                                                         uint64_t *out_size);

static bool
virgl_try_register_venus_resource(uint32_t ctx_id, uint32_t res_id)
{
    static virgl_renderer_resource_register_venus_fn register_fn;
    static bool looked_up;

    if (!looked_up) {
        register_fn = (virgl_renderer_resource_register_venus_fn)dlsym(
            RTLD_DEFAULT, "virgl_renderer_resource_register_venus");
        looked_up = true;
    }

    if (!register_fn) {
        warn_report_once("virgl_renderer_resource_register_venus not available; "
                         "zero-copy Venus import will stay disabled");
        return false;
    }

    return register_fn(ctx_id, res_id) == 0;
}

static bool
virgl_try_get_resource_iosurface_id(uint32_t ctx_id, uint32_t res_id, uint32_t *out_id)
{
    static virgl_renderer_resource_get_iosurface_id_fn get_fn;
    static bool looked_up;

    if (!out_id) {
        return false;
    }

    if (!looked_up) {
        get_fn = (virgl_renderer_resource_get_iosurface_id_fn)dlsym(
            RTLD_DEFAULT, "virgl_renderer_resource_get_iosurface_id");
        looked_up = true;
    }

    if (!get_fn) {
        warn_report_once("virgl_renderer_resource_get_iosurface_id not available; "
                         "IOSurface zero-copy path will stay disabled");
        return false;
    }

    return get_fn(ctx_id, res_id, out_id) == 0;
}

static bool
virgl_try_get_hostptr_for_size(VirtIOGPUGL *gl,
                               uint32_t ctx_id,
                               uint64_t min_size,
                               void **out_ptr,
                               uint64_t *out_size)
{
    static virgl_renderer_get_hostptr_fd_for_size_fn get_fn;
    static bool looked_up;
    int fd = -1;
    uint64_t size = 0;

    if (!out_ptr || !out_size) {
        return false;
    }

    if (!looked_up) {
        get_fn = (virgl_renderer_get_hostptr_fd_for_size_fn)dlsym(
            RTLD_DEFAULT, "virgl_renderer_get_hostptr_fd_for_size");
        looked_up = true;
    }

    if (!get_fn) {
        warn_report_once("virgl_renderer_get_hostptr_fd_for_size not available; "
                         "hostptr present path disabled");
        return false;
    }

    if (get_fn(ctx_id, min_size, &fd, &size) != 0 || fd < 0 || !size) {
        return false;
    }

    if (gl->hostptr_map && (gl->hostptr_size != size || gl->hostptr_fd != fd)) {
        munmap(gl->hostptr_map, gl->hostptr_size);
        gl->hostptr_map = NULL;
    }
    if (gl->hostptr_fd >= 0 && gl->hostptr_fd != fd) {
        close(gl->hostptr_fd);
        gl->hostptr_fd = -1;
    }

    if (!gl->hostptr_map) {
        void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            return false;
        }
        gl->hostptr_map = map;
    }

    gl->hostptr_fd = fd;
    gl->hostptr_size = size;
    *out_ptr = gl->hostptr_map;
    *out_size = size;
    return true;
}

/* Debug logging disabled - zero-copy is now default behavior */
#define vkr_hostptr_log(...) do { } while (0)

#ifdef __APPLE__
/* Timer-based present is now always enabled for Venus */
static inline bool virtio_gpu_venus_present_timer_enabled(void)
{
    return true;
}

static bool
virtio_gpu_venus_present_scanout(VirtIOGPU *g, uint32_t scanout_id, const char *tag)
{
    if (!virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        return false;
    }

    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];
    struct virtio_gpu_virgl_resource *res =
        virtio_gpu_virgl_find_resource(g, scanout->resource_id);
    struct virtio_gpu_framebuffer *fb = &scanout->fb;
    if (!fb->width || !fb->height || !fb->stride) {
        vkr_hostptr_log("%s present skip: scanout=%u res_id=%u fb=%ux%u stride=%u",
                        tag ? tag : "present", scanout_id, scanout->resource_id,
                        fb->width, fb->height, fb->stride);
        return false;
    }

    void *present_data = NULL;
    uint64_t present_size = 0;
    uint64_t need = (uint64_t)fb->stride * (uint64_t)fb->height;
    uint32_t ctx_id = res ? res->ctx_id : 0;
    if (!ctx_id) {
        ctx_id = gl->last_venus_ctx_id;
    }
    bool used_hostptr = false;

    if (ctx_id) {
        if (virgl_try_get_hostptr_for_size(gl, ctx_id, need,
                                           &present_data, &present_size) &&
            present_size >= need) {
            used_hostptr = true;
        }
    }

    if (!used_hostptr) {
        if (!res) {
            vkr_hostptr_log("%s present skip: scanout=%u res_id=%u no hostptr and no resource",
                            tag ? tag : "present", scanout_id, scanout->resource_id);
            return false;
        }
        if (!res->mapped_blob) {
            void *data = NULL;
            uint64_t size = 0;
            int ret = virgl_renderer_resource_map(scanout->resource_id, &data, &size);
            if (ret == 0 && data) {
                res->mapped_blob = data;
                res->mapped_size = size;
            }
        }
        present_data = res->mapped_blob;
        present_size = res->mapped_size;
    }

    if (!present_data) {
        vkr_hostptr_log("%s present skip: scanout=%u res_id=%u no data",
                        tag ? tag : "present", scanout_id, scanout->resource_id);
        return false;
    }

    if (!gl->vk_swapchain) {
        void *metal_layer = cocoa_get_metal_layer();
        if (!metal_layer) {
            cocoa_set_metal_layer_enabled(true);
            metal_layer = cocoa_get_metal_layer();
        }
        if (metal_layer) {
            gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                              fb->width,
                                                              fb->height);
            if (gl->vk_swapchain) {
                info_report("Venus: Host Vulkan swapchain initialized (%s %ux%u)",
                            tag ? tag : "present", fb->width, fb->height);
            }
        }
    }

    if (gl->vk_swapchain && virtio_gpu_vk_swapchain_is_valid(gl->vk_swapchain)) {
        uint32_t sw_width, sw_height;
        virtio_gpu_vk_swapchain_get_size(gl->vk_swapchain, &sw_width, &sw_height);
        if (sw_width != fb->width || sw_height != fb->height) {
            virtio_gpu_vk_swapchain_resize(gl->vk_swapchain, fb->width, fb->height);
        }
        if (virtio_gpu_vk_swapchain_present(gl->vk_swapchain, present_data, fb)) {
            vkr_hostptr_log("%s present: scanout=%u res_id=%u ctx_id=%u hostptr=%d",
                            tag ? tag : "present", scanout_id, scanout->resource_id,
                            ctx_id, used_hostptr ? 1 : 0);
            return true;
        }
    }

    return false;
}

static void virtio_gpu_venus_present_timer_cb(void *opaque)
{
    VirtIOGPUGL *gl = opaque;
    VirtIOGPU *g = VIRTIO_GPU(gl);

    if (!gl->venus_present_active) {
        return;
    }

    uint32_t scanout_id = gl->venus_present_scanout_id;
    if (!g->parent_obj.scanout[scanout_id].resource_id) {
        gl->venus_present_active = false;
        return;
    }

    vkr_hostptr_log("timer tick: scanout=%u res_id=%u",
                    scanout_id, g->parent_obj.scanout[scanout_id].resource_id);

    virtio_gpu_venus_present_scanout(g, scanout_id, "timer");

    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    const char *fps_env = getenv("VKR_PRESENT_FPS");
    const char *ns_env = getenv("VKR_PRESENT_TIMER_NS");
    uint64_t interval = 0;
    if (fps_env && fps_env[0]) {
        uint64_t fps = strtoull(fps_env, NULL, 10);
        if (fps) {
            interval = 1000000000ull / fps;
        }
    } else if (ns_env && ns_env[0]) {
        interval = strtoull(ns_env, NULL, 10);
    }

    timer_mod_ns(gl->venus_present_timer, now + interval);
}

static void virtio_gpu_venus_present_start(VirtIOGPU *g, uint32_t scanout_id)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    if (!gl->venus_present_timer) {
        gl->venus_present_timer =
            timer_new_ns(QEMU_CLOCK_REALTIME, virtio_gpu_venus_present_timer_cb, gl);
    }
    gl->venus_present_scanout_id = scanout_id;
    gl->venus_present_active = true;
    vkr_hostptr_log("timer start: scanout=%u", scanout_id);
    timer_mod_ns(gl->venus_present_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
}

static void virtio_gpu_venus_present_stop(VirtIOGPU *g)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    gl->venus_present_active = false;
    if (gl->venus_present_timer) {
        timer_del(gl->venus_present_timer);
    }
    vkr_hostptr_log("timer stop");
}
#endif

static struct virtio_gpu_virgl_resource *
virtio_gpu_virgl_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return NULL;
    }

    return container_of(res, struct virtio_gpu_virgl_resource, base);
}

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4 && defined(CONFIG_OPENGL)
static void *
virgl_get_egl_display(G_GNUC_UNUSED void *cookie)
{
    return qemu_egl_display;
}
#endif

#if VIRGL_VERSION_MAJOR >= 1
struct virtio_gpu_virgl_hostmem_region {
    MemoryRegion mr;
    struct VirtIOGPU *g;
    bool finish_unmapping;
};

static struct virtio_gpu_virgl_hostmem_region *
to_hostmem_region(MemoryRegion *mr)
{
    return container_of(mr, struct virtio_gpu_virgl_hostmem_region, mr);
}

static void virtio_gpu_virgl_resume_cmdq_bh(void *opaque)
{
    VirtIOGPU *g = opaque;

    virtio_gpu_process_cmdq(g);
}

static void virtio_gpu_virgl_hostmem_region_free(void *obj)
{
    MemoryRegion *mr = MEMORY_REGION(obj);
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b;
    VirtIOGPUGL *gl;

    vmr = to_hostmem_region(mr);
    vmr->finish_unmapping = true;

    b = VIRTIO_GPU_BASE(vmr->g);
    b->renderer_blocked--;

    /*
     * memory_region_unref() is executed from RCU thread context, while
     * virglrenderer works only on the main-loop thread that's holding GL
     * context.
     */
    gl = VIRTIO_GPU_GL(vmr->g);
    qemu_bh_schedule(gl->cmdq_resume_bh);
}

static int
virtio_gpu_virgl_map_resource_blob(VirtIOGPU *g,
                                   struct virtio_gpu_virgl_resource *res,
                                   uint64_t offset)
{
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr;
    uint64_t size;
    uint64_t aligned_size;
    void *data;
    int ret;
    uint64_t page_size = qemu_real_host_page_size();

    if (!virtio_gpu_hostmem_enabled(b->conf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: hostmem disabled\n", __func__);
        return -EOPNOTSUPP;
    }

    ret = virgl_renderer_resource_map(res->base.resource_id, &data, &size);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to map virgl resource: %s\n",
                      __func__, strerror(-ret));
        return ret;
    }

    /*
     * HVF on Apple Silicon requires 16KB page alignment for memory regions.
     * Check both the offset (guest-provided) and data pointer (from virglrenderer)
     * are aligned to the host page size. Also round up size to page alignment.
     */
    if (hvf_enabled()) {
        if (!QEMU_IS_ALIGNED(offset, page_size)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: HVF requires %"PRIu64"KB-aligned offset, got 0x%"PRIx64"\n",
                          __func__, page_size / 1024, offset);
            virgl_renderer_resource_unmap(res->base.resource_id);
            return -EINVAL;
        }
        if (!QEMU_IS_ALIGNED((uintptr_t)data, page_size)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: HVF requires %"PRIu64"KB-aligned data pointer, got %p\n",
                          __func__, page_size / 1024, data);
            virgl_renderer_resource_unmap(res->base.resource_id);
            return -EINVAL;
        }
    }

    /* Round up size to page alignment for HVF compatibility */
    aligned_size = ROUND_UP(size, page_size);

    vmr = g_new0(struct virtio_gpu_virgl_hostmem_region, 1);
    vmr->g = g;

    mr = &vmr->mr;
    memory_region_init_ram_ptr(mr, OBJECT(mr), "blob", aligned_size, data);
    memory_region_add_subregion(&b->hostmem, offset, mr);
    memory_region_set_enabled(mr, true);

    /*
     * MR could outlive the resource if MR's reference is held outside of
     * virtio-gpu. In order to prevent unmapping resource while MR is alive,
     * and thus, making the data pointer invalid, we will block virtio-gpu
     * command processing until MR is fully unreferenced and freed.
     */
    OBJECT(mr)->free = virtio_gpu_virgl_hostmem_region_free;

    res->mr = mr;

    trace_virtio_gpu_cmd_res_map_blob(res->base.resource_id, vmr, mr);

    return 0;
}

static int
virtio_gpu_virgl_unmap_resource_blob(VirtIOGPU *g,
                                     struct virtio_gpu_virgl_resource *res,
                                     bool *cmd_suspended)
{
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr = res->mr;
    int ret;

    if (!mr) {
        return 0;
    }

    vmr = to_hostmem_region(res->mr);

    trace_virtio_gpu_cmd_res_unmap_blob(res->base.resource_id, mr, vmr->finish_unmapping);

    /*
     * Perform async unmapping in 3 steps:
     *
     * 1. Begin async unmapping with memory_region_del_subregion()
     *    and suspend/block cmd processing.
     * 2. Wait for res->mr to be freed and cmd processing resumed
     *    asynchronously by virtio_gpu_virgl_hostmem_region_free().
     * 3. Finish the unmapping with final virgl_renderer_resource_unmap().
     */
    if (vmr->finish_unmapping) {
        res->mr = NULL;
        g_free(vmr);

        ret = virgl_renderer_resource_unmap(res->base.resource_id);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: failed to unmap virgl resource: %s\n",
                          __func__, strerror(-ret));
            return ret;
        }
    } else {
        *cmd_suspended = true;

        /* render will be unblocked once MR is freed */
        b->renderer_blocked++;

        /* memory region owns self res->mr object and frees it by itself */
        memory_region_set_enabled(mr, false);
        memory_region_del_subregion(&b->hostmem, mr);
        object_unparent(OBJECT(mr));
    }

    return 0;
}
#endif

static void virgl_cmd_create_resource_2d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_2d c2d;
    struct virtio_gpu_virgl_resource *res;
#ifdef CONFIG_OPENGL
    struct virgl_renderer_resource_create_args args;
#endif

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    if (c2d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c2d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c2d.width;
    res->base.height = c2d.height;
    res->base.format = c2d.format;
    res->base.resource_id = c2d.resource_id;
    res->base.dmabuf_fd = -1;
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);

#ifdef CONFIG_OPENGL
    args.handle = c2d.resource_id;
    args.target = 2;
    args.format = c2d.format;
    args.bind = (1 << 1);
    args.width = c2d.width;
    args.height = c2d.height;
    args.depth = 1;
    args.array_size = 1;
    args.last_level = 0;
    args.nr_samples = 0;
    args.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;
    virgl_renderer_resource_create(&args, NULL, 0);
#else
    /*
     * Venus-only mode: create pixman image for 2D resources.
     * This allows software scanout for console/framebuffer without OpenGL.
     */
    pixman_format_code_t pformat = virtio_gpu_get_pixman_format(c2d.format);
    if (pformat) {
        res->base.image = pixman_image_create_bits(pformat, c2d.width, c2d.height,
                                                   NULL, 0);
        if (!res->base.image) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: pixman alloc failed %d\n",
                          __func__, c2d.resource_id);
        }
    }
#endif
}

static void virgl_cmd_create_resource_3d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_3d c3d;
    struct virgl_renderer_resource_create_args args;
    struct virtio_gpu_virgl_resource *res;

    VIRTIO_GPU_FILL_CMD(c3d);
    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    if (c3d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c3d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c3d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c3d.width;
    res->base.height = c3d.height;
    res->base.format = c3d.format;
    res->base.resource_id = c3d.resource_id;
    res->base.dmabuf_fd = -1;
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);

    args.handle = c3d.resource_id;
    args.target = c3d.target;
    args.format = c3d.format;
    args.bind = c3d.bind;
    args.width = c3d.width;
    args.height = c3d.height;
    args.depth = c3d.depth;
    args.array_size = c3d.array_size;
    args.last_level = c3d.last_level;
    args.nr_samples = c3d.nr_samples;
    args.flags = c3d.flags;
    virgl_renderer_resource_create(&args, NULL, 0);
}

static void virgl_cmd_resource_unref(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd,
                                     bool *cmd_suspended)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_virgl_resource *res;
#ifdef CONFIG_OPENGL
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;
#endif

    VIRTIO_GPU_FILL_CMD(unref);
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_virgl_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

#if VIRGL_VERSION_MAJOR >= 1
    if (virtio_gpu_virgl_unmap_resource_blob(g, res, cmd_suspended)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
    if (*cmd_suspended) {
        return;
    }
#endif

#ifdef __APPLE__
    /* Clean up blob mapped for scanout on macOS */
    if (res->mapped_blob) {
        virgl_renderer_resource_unmap(unref.resource_id);
        res->mapped_blob = NULL;
        res->mapped_size = 0;
    }
    if (res->iosurface) {
        virtio_gpu_release_iosurface(res->iosurface);
        res->iosurface = NULL;
    }
    res->iosurface_id = 0;
    res->ctx_id = 0;
    /* Clean up software scanout pixman image */
    if (res->scanout_image) {
        pixman_image_unref(res->scanout_image);
        res->scanout_image = NULL;
    }
#endif

#ifdef CONFIG_OPENGL
    virgl_renderer_resource_detach_iov(unref.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }
    virgl_renderer_resource_unref(unref.resource_id);
#else
    /* Venus-only mode: clean up QEMU-managed resources */
    if (res->base.iov) {
        virtio_gpu_cleanup_mapping_iov(g, res->base.iov, res->base.iov_cnt);
        res->base.iov = NULL;
        res->base.iov_cnt = 0;
    }
    if (res->base.image) {
        pixman_image_unref(res->base.image);
        res->base.image = NULL;
    }
#endif

    QTAILQ_REMOVE(&g->reslist, &res->base, next);

    g_free(res);
}

static void virgl_cmd_context_create(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;
    size_t cmd_size;

    /*
     * Handle both old (without context_init) and new format commands.
     * Old format is 92 bytes, new format is 96 bytes.
     */
    memset(&cc, 0, sizeof(cc));
    cmd_size = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num, 0,
                          &cc, sizeof(cc));

    /* Accept old format (without context_init) - minimum size is just hdr */
    if (cmd_size < sizeof(struct virtio_gpu_ctrl_hdr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command size too small %zu\n", __func__, cmd_size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    trace_virtio_gpu_cmd_ctx_create(cc.hdr.ctx_id,
                                    cc.debug_name);

    if (cc.context_init) {
        if (!virtio_gpu_context_init_enabled(g->parent_obj.conf)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: context_init disabled",
                          __func__);
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }

#if VIRGL_VERSION_MAJOR >= 1
#ifndef CONFIG_OPENGL
        /*
         * Venus-only mode: only forward Venus context requests (capset=4).
         * VIRGL/VIRGL2 contexts (capset 1,2) would fail without vrend.
         * Accept them as no-op so UEFI can proceed.
         */
        if (virtio_gpu_venus_enabled(g->parent_obj.conf) &&
            cc.context_init != VIRTIO_GPU_CAPSET_VENUS) {
            return;  /* No-op for non-Venus contexts */
        }
#endif
        virgl_renderer_context_create_with_flags(cc.hdr.ctx_id,
                                                 cc.context_init,
                                                 cc.nlen,
                                                 cc.debug_name);
#ifdef __APPLE__
        if (cc.context_init == VIRTIO_GPU_CAPSET_VENUS) {
            VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
            gl->last_venus_ctx_id = cc.hdr.ctx_id;
        }
#endif
        return;
#endif
    }

#ifndef CONFIG_OPENGL
    /*
     * Venus-only mode without OpenGL: non-Venus context creation
     * (context_init=0 defaults to VIRGL2) would fail because vrend
     * is not initialized. Accept the request as a no-op so UEFI
     * can proceed. Only Venus contexts will actually work.
     */
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        return;  /* Success - no-op for non-Venus contexts */
    }
#endif

    virgl_renderer_context_create(cc.hdr.ctx_id, cc.nlen, cc.debug_name);
}

static void virgl_cmd_context_destroy(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    virgl_renderer_context_destroy(cd.hdr.ctx_id);
}

static void virtio_gpu_rect_update(VirtIOGPU *g, int idx, int x, int y,
                                int width, int height)
{
    if (!g->parent_obj.scanout[idx].con) {
        return;
    }

#ifdef CONFIG_OPENGL
    dpy_gl_update(g->parent_obj.scanout[idx].con, x, y, width, height);
#else
    /*
     * Venus-only mode without OpenGL: trigger a display update.
     * The actual rendering happens via Vulkan/MoltenVK on the host.
     */
    dpy_gfx_update_full(g->parent_obj.scanout[idx].con);
#endif
}

static void virgl_cmd_resource_flush(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_flush rf;
    int i;

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);
    vkr_hostptr_log("resource_flush: res_id=%u rect=%ux%u+%u+%u",
                    rf.resource_id, rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    /*
     * Venus-only mode: pixel data is already in the resource's pixman image,
     * populated by TRANSFER_TO_HOST_2D. Just trigger the display update.
     */

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        bool presented = false;
        if (g->parent_obj.scanout[i].resource_id != rf.resource_id) {
            continue;
        }
#ifdef __APPLE__
        vkr_hostptr_log("resource_flush: match scanout=%d res_id=%u",
                        i, rf.resource_id);
#endif
#ifdef __APPLE__
        if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
            VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
            struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[i];
            struct virtio_gpu_virgl_resource *res = virtio_gpu_virgl_find_resource(g, rf.resource_id);
            struct virtio_gpu_framebuffer *fb = &scanout->fb;

            if (res && fb->width && fb->height && fb->stride) {
                void *present_data = NULL;
                uint64_t present_size = 0;
                uint64_t need = (uint64_t)fb->stride * (uint64_t)fb->height;
                uint32_t ctx_id = res->ctx_id ? res->ctx_id : gl->last_venus_ctx_id;
                bool used_hostptr = false;

                if (ctx_id) {
                    if (virgl_try_get_hostptr_for_size(gl, ctx_id, need,
                                                       &present_data, &present_size) &&
                        present_size >= need) {
                        used_hostptr = true;
                    }
                }

                if (!used_hostptr) {
                    if (!res->mapped_blob) {
                        void *data = NULL;
                        uint64_t size = 0;
                        int ret = virgl_renderer_resource_map(rf.resource_id, &data, &size);
                        if (ret == 0 && data) {
                            res->mapped_blob = data;
                            res->mapped_size = size;
                        }
                    }
                    present_data = res->mapped_blob;
                    present_size = res->mapped_size;
                }

                if (present_data) {
                    if (!gl->vk_swapchain) {
                        void *metal_layer = cocoa_get_metal_layer();
                        if (!metal_layer) {
                            cocoa_set_metal_layer_enabled(true);
                            metal_layer = cocoa_get_metal_layer();
                        }
                        if (metal_layer) {
                            gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                                              fb->width,
                                                                              fb->height);
                            if (gl->vk_swapchain) {
                                info_report("Venus: Host Vulkan swapchain initialized (flush %ux%u)",
                                            fb->width, fb->height);
                            }
                        }
                    }

                    if (gl->vk_swapchain && virtio_gpu_vk_swapchain_is_valid(gl->vk_swapchain)) {
                        uint32_t sw_width, sw_height;
                        virtio_gpu_vk_swapchain_get_size(gl->vk_swapchain, &sw_width, &sw_height);
                        if (sw_width != fb->width || sw_height != fb->height) {
                            virtio_gpu_vk_swapchain_resize(gl->vk_swapchain, fb->width, fb->height);
                        }
                        if (virtio_gpu_vk_swapchain_present(gl->vk_swapchain, present_data, fb)) {
                            vkr_hostptr_log("flush present: res_id=%u ctx_id=%u hostptr=%d",
                                            rf.resource_id, ctx_id, used_hostptr ? 1 : 0);
                            presented = true;
                        }
                    }
                }
            }
        }
#endif
        if (presented) {
            continue;
        }
        virtio_gpu_rect_update(g, i, rf.r.x, rf.r.y, rf.r.width, rf.r.height);
    }
}

static void virgl_cmd_set_scanout(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_set_scanout ss;
#ifdef CONFIG_OPENGL
    int ret;
#endif
#ifdef __APPLE__
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
#endif

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);
    vkr_hostptr_log("set_scanout legacy: scanout_id=%u res_id=%u w=%u h=%u",
                    ss.scanout_id, ss.resource_id, ss.r.width, ss.r.height);
    vkr_hostptr_log("timer env: %s", getenv("VKR_PRESENT_TIMER") ? getenv("VKR_PRESENT_TIMER") : "null");

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }
    if (ss.resource_id == 0) {
#ifdef __APPLE__
        virtio_gpu_venus_present_stop(g);
#endif
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }
    g->parent_obj.enable = 1;

#ifdef __APPLE__
    {
        struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
        scanout->fb.width = ss.r.width;
        scanout->fb.height = ss.r.height;
        scanout->fb.stride = ss.r.width * 4;
        scanout->fb.bytes_pp = 4;
        scanout->fb.format = PIXMAN_x8r8g8b8;
        scanout->x = ss.r.x;
        scanout->y = ss.r.y;
        scanout->width = ss.r.width;
        scanout->height = ss.r.height;
    }
#endif

#ifdef __APPLE__
    /* Prefer host swapchain presentation for Venus on macOS (no OpenGL). */
    if (virtio_gpu_venus_enabled(g->parent_obj.conf) &&
        ss.resource_id && ss.r.width && ss.r.height) {
        bool timer_enabled = virtio_gpu_venus_present_timer_enabled();
        vkr_hostptr_log("timer enabled: %d", timer_enabled ? 1 : 0);
        if (timer_enabled) {
            virtio_gpu_venus_present_start(g, ss.scanout_id);
        }
        if (gl->last_venus_ctx_id) {
            void *present_data = NULL;
            uint64_t present_size = 0;
            uint64_t need = (uint64_t)ss.r.width * (uint64_t)ss.r.height * 4;
            if (virgl_try_get_hostptr_for_size(gl, gl->last_venus_ctx_id,
                                               need, &present_data, &present_size)) {
                struct virtio_gpu_framebuffer fb = { 0 };
                fb.width = ss.r.width;
                fb.height = ss.r.height;
                fb.stride = ss.r.width * 4;
                fb.bytes_pp = 4;
                fb.format = PIXMAN_x8r8g8b8;

                vkr_hostptr_log("legacy hostptr: ctx_id=%u size=%llu",
                                gl->last_venus_ctx_id,
                                (unsigned long long)present_size);
                uint64_t need = (uint64_t)fb.stride * (uint64_t)fb.height;
                if (present_size < need) {
                    vkr_hostptr_log("legacy hostptr: too small (have=%llu need=%llu)",
                                    (unsigned long long)present_size,
                                    (unsigned long long)need);
                    goto legacy_hostptr_fallback;
                }

                if (!gl->vk_swapchain) {
                    void *metal_layer = cocoa_get_metal_layer();
                    if (!metal_layer) {
                        cocoa_set_metal_layer_enabled(true);
                        metal_layer = cocoa_get_metal_layer();
                    }
                    if (metal_layer) {
                        gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                                          fb.width,
                                                                          fb.height);
                        if (gl->vk_swapchain) {
                            info_report("Venus: Host Vulkan swapchain initialized (hostptr %ux%u)",
                                        fb.width, fb.height);
                        }
                    }
                }

                if (gl->vk_swapchain && virtio_gpu_vk_swapchain_is_valid(gl->vk_swapchain)) {
                    if (virtio_gpu_vk_swapchain_present(gl->vk_swapchain,
                                                        present_data, &fb)) {
                        struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
                        scanout->resource_id = ss.resource_id;
                        scanout->fb = fb;
                        scanout->x = ss.r.x;
                        scanout->y = ss.r.y;
                        scanout->width = ss.r.width;
                        scanout->height = ss.r.height;
                        return;
                    }
                } else {
                    vkr_hostptr_log("legacy hostptr: swapchain invalid");
                }
            } else {
                vkr_hostptr_log("legacy hostptr: no hostptr ctx_id=%u",
                                gl->last_venus_ctx_id);
            }
        }
legacy_hostptr_fallback:
        ;
        struct virtio_gpu_virgl_resource *res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
        if (res) {
            struct virgl_renderer_resource_info info;
            memset(&info, 0, sizeof(info));
            if (virgl_renderer_resource_get_info(ss.resource_id, &info) == 0) {
                vkr_hostptr_log("legacy swapchain: res_id=%u info=%ux%u stride=%u",
                                ss.resource_id, info.width, info.height, info.stride);
                if (!res->mapped_blob) {
                    void *data = NULL;
                    uint64_t size = 0;
                    int map_ret = virgl_renderer_resource_map(ss.resource_id, &data, &size);
                    if (map_ret == 0 && data) {
                        res->mapped_blob = data;
                        res->mapped_size = size;
                    } else {
                        vkr_hostptr_log("legacy swapchain: map failed res_id=%u ret=%d",
                                        ss.resource_id, map_ret);
                    }
                }

                if (res->mapped_blob) {
                    struct virtio_gpu_framebuffer fb = { 0 };
                    fb.width = info.width;
                    fb.height = info.height;
                    fb.stride = info.stride ? info.stride : info.width * 4;
                    fb.bytes_pp = 4;
                    fb.format = PIXMAN_x8r8g8b8;

                    if (!gl->vk_swapchain) {
                        void *metal_layer = cocoa_get_metal_layer();
                        if (!metal_layer) {
                            cocoa_set_metal_layer_enabled(true);
                            metal_layer = cocoa_get_metal_layer();
                        }
                        if (metal_layer) {
                            gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                                              fb.width,
                                                                              fb.height);
                            if (gl->vk_swapchain) {
                                info_report("Venus: Host Vulkan swapchain initialized (legacy %ux%u)",
                                            fb.width, fb.height);
                                vkr_hostptr_log("legacy swapchain: created %ux%u",
                                                fb.width, fb.height);
                            } else {
                                vkr_hostptr_log("legacy swapchain: create failed");
                            }
                        }
                    }

                    if (gl->vk_swapchain && virtio_gpu_vk_swapchain_is_valid(gl->vk_swapchain)) {
                        if (virtio_gpu_vk_swapchain_present(gl->vk_swapchain,
                                                            res->mapped_blob, &fb)) {
                            struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
                            scanout->resource_id = ss.resource_id;
                            scanout->fb = fb;
                            scanout->x = ss.r.x;
                            scanout->y = ss.r.y;
                            scanout->width = ss.r.width;
                            scanout->height = ss.r.height;
                            return;
                        } else {
                            vkr_hostptr_log("legacy swapchain: present failed res_id=%u",
                                            ss.resource_id);
                        }
                    } else {
                        vkr_hostptr_log("legacy swapchain: swapchain invalid");
                    }
                }
            } else {
                vkr_hostptr_log("legacy swapchain: get_info failed res_id=%u", ss.resource_id);
            }
        }
    }
#endif

    if (ss.resource_id && ss.r.width && ss.r.height) {
#ifdef __APPLE__
        /* Prefer host swapchain on macOS - no OpenGL needed for Venus */
        if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
            return;
        }
#endif
#ifdef CONFIG_OPENGL
        if (do_gl) {
            struct virgl_renderer_resource_info info;
            void *d3d_tex2d = NULL;

#if VIRGL_VERSION_MAJOR >= 1
        struct virgl_renderer_resource_info_ext ext;
        memset(&ext, 0, sizeof(ext));
        ret = virgl_renderer_resource_get_info_ext(ss.resource_id, &ext);
        info = ext.base;
        d3d_tex2d = ext.d3d_tex2d;
#else
        memset(&info, 0, sizeof(info));
        ret = virgl_renderer_resource_get_info(ss.resource_id, &info);
#endif
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: illegal resource specified %d\n",
                          __func__, ss.resource_id);
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            return;
        }
        qemu_console_resize(g->parent_obj.scanout[ss.scanout_id].con,
                            ss.r.width, ss.r.height);
        virgl_renderer_force_ctx_0();
            dpy_gl_scanout_texture(
                g->parent_obj.scanout[ss.scanout_id].con, info.tex_id,
                info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
                info.width, info.height,
                ss.r.x, ss.r.y, ss.r.width, ss.r.height,
                d3d_tex2d);
            return;
        }
#endif
        /*
         * Software scanout using pixman.
         * Use the pixman image created in RESOURCE_CREATE_2D for display.
         */
        struct virtio_gpu_virgl_resource *res;

        res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
        if (!res) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: resource not found %d\n",
                          __func__, ss.resource_id);
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            return;
        }

        qemu_console_resize(g->parent_obj.scanout[ss.scanout_id].con,
                            ss.r.width, ss.r.height);

        /*
         * Use the resource's pixman image for display.
         * The image is populated via TRANSFER_TO_HOST_2D commands.
         */
        if (res->base.image) {
            struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
            pixman_image_ref(res->base.image);
            scanout->ds = qemu_create_displaysurface_pixman(res->base.image);
            dpy_gfx_replace_surface(scanout->con, scanout->ds);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: resource %d has no pixman image\n",
                          __func__, ss.resource_id);
        }
    } else {
        dpy_gfx_replace_surface(
            g->parent_obj.scanout[ss.scanout_id].con, NULL);
#ifdef CONFIG_OPENGL
        dpy_gl_scanout_disable(g->parent_obj.scanout[ss.scanout_id].con);
#endif
    }
    g->parent_obj.scanout[ss.scanout_id].resource_id = ss.resource_id;
}

static void virgl_cmd_submit_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_submit cs;
    void *buf;
    size_t s;
    static int submit_log_budget = 5;

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);
    vkr_hostptr_log("submit_3d: ctx_id=%u size=%u", cs.hdr.ctx_id, cs.size);

    buf = g_malloc(cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    if (s != cs.size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: size mismatch (%zd/%d)",
                      __func__, s, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        goto out;
    }

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        g->stats.req_3d++;
        g->stats.bytes_3d += cs.size;
    }

    virgl_renderer_submit_cmd(buf, cs.hdr.ctx_id, cs.size / 4);

out:
    g_free(buf);

#ifdef __APPLE__
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        for (uint32_t i = 0; i < g->parent_obj.conf.max_outputs; i++) {
            struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[i];
            if (scanout->resource_id) {
                bool ok = virtio_gpu_venus_present_scanout(g, i, "submit");
                if (!ok) {
                    vkr_hostptr_log("submit present skipped: scanout=%u res_id=%u fb=%ux%u stride=%u",
                                    i, scanout->resource_id,
                                    scanout->fb.width, scanout->fb.height,
                                    scanout->fb.stride);
                } else if (submit_log_budget > 0) {
                    vkr_hostptr_log("submit present ok: scanout=%u res_id=%u",
                                    i, scanout->resource_id);
                    submit_log_budget--;
                }
            }
        }
    }
#endif
}

static void virgl_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_to_host_2d t2d;
#ifdef CONFIG_OPENGL
    struct virtio_gpu_box box;
#endif

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

#ifdef CONFIG_OPENGL
    box.x = t2d.r.x;
    box.y = t2d.r.y;
    box.z = 0;
    box.w = t2d.r.width;
    box.h = t2d.r.height;
    box.d = 1;

    virgl_renderer_transfer_write_iov(t2d.resource_id,
                                      0,
                                      0,
                                      0,
                                      0,
                                      (struct virgl_box *)&box,
                                      t2d.offset, NULL, 0);
#else
    /*
     * Venus-only mode: copy data from guest iov to pixman image.
     * This handles the fbdev console transfer for software scanout.
     */
    struct virtio_gpu_virgl_resource *res;
    res = virtio_gpu_virgl_find_resource(g, t2d.resource_id);
    if (res && res->base.image && res->base.iov) {
        uint32_t src_stride = pixman_image_get_stride(res->base.image);
        uint32_t dst_width = pixman_image_get_width(res->base.image);
        uint32_t dst_height = pixman_image_get_height(res->base.image);
        uint32_t bytes_pp = PIXMAN_FORMAT_BPP(pixman_image_get_format(res->base.image)) / 8;
        uint8_t *dst = (uint8_t *)pixman_image_get_data(res->base.image);

        /* Bounds check */
        if (t2d.r.x + t2d.r.width > dst_width ||
            t2d.r.y + t2d.r.height > dst_height) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }

        /* Copy row by row from iov to pixman image */
        for (uint32_t y = 0; y < t2d.r.height; y++) {
            size_t src_offset = t2d.offset + y * src_stride;
            size_t dst_offset = (t2d.r.y + y) * src_stride + t2d.r.x * bytes_pp;
            size_t row_bytes = t2d.r.width * bytes_pp;

            iov_to_buf(res->base.iov, res->base.iov_cnt, src_offset,
                       dst + dst_offset, row_bytes);
        }
    }
#endif
}

static void virgl_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d t3d;

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    virgl_renderer_transfer_write_iov(t3d.resource_id,
                                      t3d.hdr.ctx_id,
                                      t3d.level,
                                      t3d.stride,
                                      t3d.layer_stride,
                                      (struct virgl_box *)&t3d.box,
                                      t3d.offset, NULL, 0);
}

static void
virgl_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d tf3d;

    VIRTIO_GPU_FILL_CMD(tf3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(tf3d.resource_id);

    virgl_renderer_transfer_read_iov(tf3d.resource_id,
                                     tf3d.hdr.ctx_id,
                                     tf3d.level,
                                     tf3d.stride,
                                     tf3d.layer_stride,
                                     (struct virgl_box *)&tf3d.box,
                                     tf3d.offset, NULL, 0);
}


static void virgl_resource_attach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_attach_backing att_rb;
    struct iovec *res_iovs;
    uint32_t res_niov;
    int ret;

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    ret = virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb),
                                        cmd, NULL, &res_iovs, &res_niov);
    if (ret != 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

#ifdef CONFIG_OPENGL
    ret = virgl_renderer_resource_attach_iov(att_rb.resource_id,
                                             res_iovs, res_niov);
    if (ret != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, res_niov);
    }
#else
    /*
     * Venus-only mode: store iov in resource for 2D software scanout.
     * The iov is needed for transfer_to_host_2d to copy data to pixman image.
     */
    struct virtio_gpu_virgl_resource *res;
    res = virtio_gpu_virgl_find_resource(g, att_rb.resource_id);
    if (res) {
        res->base.iov = res_iovs;
        res->base.iov_cnt = res_niov;
    } else {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, res_niov);
    }
#endif
}

static void virgl_resource_detach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_detach_backing detach_rb;

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

#ifdef CONFIG_OPENGL
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;
    virgl_renderer_resource_detach_iov(detach_rb.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs == NULL || num_iovs == 0) {
        return;
    }
    virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
#else
    /* Venus-only mode: clean up iov stored in resource */
    struct virtio_gpu_virgl_resource *res;
    res = virtio_gpu_virgl_find_resource(g, detach_rb.resource_id);
    if (res && res->base.iov) {
        virtio_gpu_cleanup_mapping_iov(g, res->base.iov, res->base.iov_cnt);
        res->base.iov = NULL;
        res->base.iov_cnt = 0;
    }
#endif
}


static void virgl_cmd_ctx_attach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource att_res;

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    virgl_renderer_ctx_attach_resource(att_res.hdr.ctx_id, att_res.resource_id);
}

static void virgl_cmd_ctx_detach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource det_res;

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    virgl_renderer_ctx_detach_resource(det_res.hdr.ctx_id, det_res.resource_id);
}

static void virgl_cmd_get_capset_info(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VIRTIO_GPU_FILL_CMD(info);

    memset(&resp, 0, sizeof(resp));

    if (info.capset_index < g->capset_ids->len) {
        resp.capset_id = g_array_index(g->capset_ids, uint32_t,
                                       info.capset_index);
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    }
    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_get_capset(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t max_ver, max_size;
    VIRTIO_GPU_FILL_CMD(gc);

    virgl_renderer_get_cap_set(gc.capset_id, &max_ver,
                               &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(gc.capset_id,
                             gc.capset_version,
                             (void *)resp->capset_data);
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_cmd_resource_create_blob(VirtIOGPU *g,
                                           struct virtio_gpu_ctrl_command *cmd)
{
    struct virgl_renderer_resource_create_blob_args virgl_args = { 0 };
    g_autofree struct virtio_gpu_virgl_resource *res = NULL;
    struct virtio_gpu_resource_create_blob cblob;
    struct virgl_renderer_resource_info info;
    int ret;

    if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    if (cblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, cblob.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.resource_id = cblob.resource_id;
    res->base.blob_size = cblob.size;
    res->base.dmabuf_fd = -1;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                            cmd, &res->base.addrs,
                                            &res->base.iov, &res->base.iov_cnt);
        if (!ret) {
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
    }

    virgl_args.res_handle = cblob.resource_id;
    virgl_args.ctx_id = cblob.hdr.ctx_id;
    virgl_args.blob_mem = cblob.blob_mem;
    virgl_args.blob_id = cblob.blob_id;
    virgl_args.blob_flags = cblob.blob_flags;
    virgl_args.size = cblob.size;
    virgl_args.iovecs = res->base.iov;
    virgl_args.num_iovs = res->base.iov_cnt;

    ret = virgl_renderer_resource_create_blob(&virgl_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        virtio_gpu_cleanup_mapping(g, &res->base);
        return;
    }

    ret = virgl_renderer_resource_get_info(cblob.resource_id, &info);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: resource does not have info %d: %s\n",
                      __func__, cblob.resource_id, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        virtio_gpu_cleanup_mapping(g, &res->base);
        virgl_renderer_resource_unref(cblob.resource_id);
        return;
    }

    res->base.dmabuf_fd = info.fd;

#ifdef __APPLE__
    res->ctx_id = cblob.hdr.ctx_id;
    res->iosurface_id = 0;
    if (res->base.dmabuf_fd < 0) {
        warn_report_once("Blob resource %d created without dmabuf backing. "
                         "Blob scanout will not work on macOS without dmabuf support.",
                         cblob.resource_id);
    }
#endif

    if (!virgl_try_register_venus_resource(cblob.hdr.ctx_id,
                                           cblob.resource_id)) {
        warn_report_once("Failed to register blob resource %d with Venus context %u",
                         cblob.resource_id, cblob.hdr.ctx_id);
    }

    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);
    res = NULL;
}

static void virgl_cmd_resource_map_blob(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_map_blob mblob;
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_resp_map_info resp;
    int ret;

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    res = virtio_gpu_virgl_find_resource(g, mblob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = virtio_gpu_virgl_map_resource_blob(g, res, mblob.offset);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    virgl_renderer_resource_get_map_info(mblob.resource_id, &resp.map_info);
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_resource_unmap_blob(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd,
                                          bool *cmd_suspended)
{
    struct virtio_gpu_resource_unmap_blob ublob;
    struct virtio_gpu_virgl_resource *res;
    int ret;

    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    res = virtio_gpu_virgl_find_resource(g, ublob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = virtio_gpu_virgl_unmap_resource_blob(g, res, cmd_suspended);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
}

static void virgl_cmd_set_scanout_blob(VirtIOGPU *g,
                                       struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_set_scanout_blob ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_scanout_blob_bswap(&ss);
    trace_virtio_gpu_cmd_set_scanout_blob(ss.scanout_id, ss.resource_id,
                                          ss.r.width, ss.r.height, ss.r.x,
                                          ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
#ifdef __APPLE__
        virtio_gpu_venus_present_stop(g);
#endif
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    if (ss.width < 16 ||
        ss.height < 16 ||
        ss.r.x + ss.r.width > ss.width ||
        ss.r.y + ss.r.height > ss.height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout %d bounds for"
                      " resource %d, rect (%d,%d)+%d,%d, fb %d %d\n",
                      __func__, ss.scanout_id, ss.resource_id,
                      ss.r.x, ss.r.y, ss.r.width, ss.r.height,
                      ss.width, ss.height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    if (!virtio_gpu_scanout_blob_to_fb(&fb, &ss, res->base.blob_size)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    g->parent_obj.enable = 1;

#ifdef __APPLE__
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    vkr_hostptr_log("set_scanout_blob: res_id=%u ctx_id=%u", ss.resource_id, res->ctx_id);
    if (virtio_gpu_venus_present_timer_enabled()) {
        virtio_gpu_venus_present_start(g, ss.scanout_id);
    }
    if (getenv("VKR_USE_IOSURFACE")) {
        uint32_t ios_id = 0;
        if (res->ctx_id &&
            virgl_try_get_resource_iosurface_id(res->ctx_id, ss.resource_id, &ios_id) &&
            ios_id) {
            if (!res->iosurface || res->iosurface_id != ios_id) {
                if (res->iosurface) {
                    virtio_gpu_release_iosurface(res->iosurface);
                }
                res->iosurface = IOSurfaceLookup(ios_id);
                res->iosurface_id = ios_id;
            }
            if (res->iosurface) {
                fprintf(stderr, "QEMU IOSurface zero-copy: res_id=%u iosurface_id=%u\n",
                        ss.resource_id, ios_id);
                cocoa_set_metal_layer_enabled(true);
                if (virtio_gpu_present_iosurface(res->iosurface,
                                                 cocoa_get_metal_layer())) {
                    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
                    scanout->resource_id = ss.resource_id;
                    scanout->fb = fb;
                    scanout->x = ss.r.x;
                    scanout->y = ss.r.y;
                    scanout->width = ss.r.width;
                    scanout->height = ss.r.height;
                    return;
                }
            }
        }
    }

    /*
     * On macOS, dmabuf is not available. We use a host-side Vulkan swapchain
     * for presentation when Venus is enabled, with fallback to software scanout.
     *
     * We need to map the blob to get its host pointer. This is done lazily
     * here since the guest may not have issued MAP_BLOB before SET_SCANOUT_BLOB.
     */
    if (!res->mapped_blob) {
        void *data = NULL;
        uint64_t size = 0;
        int ret = virgl_renderer_resource_map(ss.resource_id, &data, &size);
        if (ret || !data) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: failed to map blob resource %d: %s\n",
                          __func__, ss.resource_id, strerror(-ret));
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
        res->mapped_blob = data;
        res->mapped_size = size;
    }

    if (getenv("VKR_USE_IOSURFACE")) {
        uint32_t ios_w = 0, ios_h = 0;
        virtio_gpu_get_iosurface_size(res->iosurface, &ios_w, &ios_h);
        if (!res->iosurface || ios_w != fb.width || ios_h != fb.height) {
            if (res->iosurface) {
                virtio_gpu_release_iosurface(res->iosurface);
            }
            res->iosurface = virtio_gpu_create_iosurface(fb.width, fb.height,
                                                         fb.stride, fb.format);
            res->iosurface_id = 0;
        }

        if (res->iosurface) {
            virtio_gpu_update_iosurface(res->iosurface,
                                        res->mapped_blob,
                                        fb.width, fb.height,
                                        fb.stride, fb.offset);
            cocoa_set_metal_layer_enabled(true);
            if (virtio_gpu_present_iosurface(res->iosurface,
                                             cocoa_get_metal_layer())) {
                struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
                scanout->resource_id = ss.resource_id;
                scanout->fb = fb;
                scanout->x = ss.r.x;
                scanout->y = ss.r.y;
                scanout->width = ss.r.width;
                scanout->height = ss.r.height;
                return;
            }
        }
    }
    if (!gl->vk_swapchain) {
        void *metal_layer = cocoa_get_metal_layer();
        if (!metal_layer) {
            cocoa_set_metal_layer_enabled(true);
            metal_layer = cocoa_get_metal_layer();
        }
        if (metal_layer) {
            gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                              fb.width,
                                                              fb.height);
            if (gl->vk_swapchain) {
                info_report("Venus: Host Vulkan swapchain initialized (lazy %ux%u)",
                            fb.width, fb.height);
            } else {
                warn_report("Venus: Failed to create host Vulkan swapchain (lazy)");
            }
        }
    }

    if (gl->vk_swapchain && virtio_gpu_vk_swapchain_is_valid(gl->vk_swapchain)) {
        void *present_data = res->mapped_blob;
        uint64_t present_size = res->mapped_size;
        bool used_hostptr = false;

        if (res->ctx_id) {
            /* Present from Venus' last HOST_VISIBLE allocation to avoid guest CPU copies.
             * Alternatives:
             *  - True dmabuf import of the GBM scanout buffer (blocked by res_id mismatch)
             *  - IOSurface path (host-side copy from blob)
             *  - Guest CPU memcpy into GBM (fallback path)
             */
            uint64_t need = (uint64_t)fb.stride * (uint64_t)fb.height;
            if (virgl_try_get_hostptr_for_size(gl, res->ctx_id,
                                               need, &present_data, &present_size)) {
                if (present_size < need) {
                    fprintf(stderr,
                            "QEMU hostptr present: too small (have=%llu need=%llu), fallback to blob\n",
                            (unsigned long long)present_size,
                            (unsigned long long)need);
                    vkr_hostptr_log("hostptr too small: have=%llu need=%llu res_id=%u ctx_id=%u",
                                    (unsigned long long)present_size,
                                    (unsigned long long)need,
                                    ss.resource_id,
                                    res->ctx_id);
                    present_data = res->mapped_blob;
                    present_size = res->mapped_size;
                } else {
                    fprintf(stderr,
                            "QEMU hostptr present: using hostptr %p size=%llu for res_id=%u ctx_id=%u\n",
                            present_data,
                            (unsigned long long)present_size,
                            ss.resource_id,
                            res->ctx_id);
                    vkr_hostptr_log("hostptr ok: ptr=%p size=%llu res_id=%u ctx_id=%u",
                                    present_data,
                                    (unsigned long long)present_size,
                                    ss.resource_id,
                                    res->ctx_id);
                    cocoa_set_metal_layer_enabled(true);
                    used_hostptr = true;
                }
            } else {
                fprintf(stderr,
                        "QEMU hostptr present: no hostptr for res_id=%u ctx_id=%u, fallback to blob\n",
                        ss.resource_id,
                        res->ctx_id);
                vkr_hostptr_log("hostptr missing: res_id=%u ctx_id=%u", ss.resource_id, res->ctx_id);
            }
        }

        fprintf(stderr, "QEMU swapchain present: res_id=%u ctx_id=%u used_hostptr=%d stride=%u height=%u\n",
                ss.resource_id, res->ctx_id, used_hostptr ? 1 : 0, fb.stride, fb.height);
        vkr_hostptr_log("swapchain present: res_id=%u ctx_id=%u used_hostptr=%d stride=%u height=%u",
                        ss.resource_id, res->ctx_id, used_hostptr ? 1 : 0, fb.stride, fb.height);

        /* Resize swapchain if dimensions changed */
        uint32_t sw_width, sw_height;
        virtio_gpu_vk_swapchain_get_size(gl->vk_swapchain, &sw_width, &sw_height);
        if (sw_width != fb.width || sw_height != fb.height) {
            virtio_gpu_vk_swapchain_resize(gl->vk_swapchain, fb.width, fb.height);
        }

        /* Present the blob via Vulkan swapchain */
        if (virtio_gpu_vk_swapchain_present(gl->vk_swapchain, present_data, &fb)) {
            /* Update scanout state for tracking */
            struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss.scanout_id];
            scanout->resource_id = ss.resource_id;
            scanout->fb = fb;
            scanout->x = ss.r.x;
            scanout->y = ss.r.y;
            scanout->width = ss.r.width;
            scanout->height = ss.r.height;
            return;
        }
        /* Fall through to software path on swapchain failure */
    }

    /* Fallback: software scanout via pixman */
    res->base.blob = res->mapped_blob;

    if (!virtio_gpu_do_set_scanout(g, ss.scanout_id, &fb, &res->base,
                                   &ss.r, &cmd->error)) {
        return;
    }
#else
    if (res->base.dmabuf_fd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource not backed by dmabuf %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
    /* dmabuf path for GL-accelerated display */
    if (virtio_gpu_update_dmabuf(g, ss.scanout_id, &res->base, &fb, &ss.r)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to update dmabuf\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
#endif
}
#endif

void virtio_gpu_virgl_process_cmd(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    bool cmd_suspended = false;

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);
    vkr_hostptr_log("cmd: type=%u", cmd->cmd_hdr.type);

    virgl_renderer_force_ctx_0();
    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        virgl_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        virgl_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        virgl_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        vkr_hostptr_log("cmd: submit_3d");
        virgl_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        virgl_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        virgl_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        virgl_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        virgl_resource_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        virgl_resource_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        virgl_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        vkr_hostptr_log("cmd: resource_flush");
        virgl_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        virgl_cmd_resource_unref(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        virgl_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        virgl_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
#if VIRGL_VERSION_MAJOR >= 1
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        virgl_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        virgl_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        virgl_cmd_resource_unmap_blob(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        virgl_cmd_set_scanout_blob(g, cmd);
        break;
#endif
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd_suspended || cmd->finished) {
        return;
    }
    if (cmd->error) {
        fprintf(stderr, "%s: ctrl 0x%x, error 0x%x\n", __func__,
                cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);
#if VIRGL_VERSION_MAJOR >= 1
    if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
        virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id,
                                            VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                            cmd->cmd_hdr.ring_idx,
                                            cmd->cmd_hdr.fence_id);
        return;
    }
#endif
    virgl_renderer_create_fence(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        /*
         * the guest can end up emitting fences out of order
         * so we should check all fenced cmds not just the first one.
         */
#if VIRGL_VERSION_MAJOR >= 1
        if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
            continue;
        }
#endif
        if (cmd->cmd_hdr.fence_id > fence) {
            continue;
        }
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            trace_virtio_gpu_dec_inflight_fences(g->inflight);
        }
    }
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t ring_idx, uint64_t fence_id) {
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX &&
            cmd->cmd_hdr.ctx_id == ctx_id && cmd->cmd_hdr.ring_idx == ring_idx &&
            cmd->cmd_hdr.fence_id <= fence_id) {
            trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                trace_virtio_gpu_dec_inflight_fences(g->inflight);
            }
        }
    }
}
#endif

#ifdef CONFIG_OPENGL
static virgl_renderer_gl_context
virgl_create_context(void *opaque, int scanout_idx,
                     struct virgl_renderer_gl_ctx_param *params)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext ctx;
    QEMUGLParams qparams;

    qparams.major_ver = params->major_ver;
    qparams.minor_ver = params->minor_ver;

    ctx = dpy_gl_ctx_create(g->parent_obj.scanout[scanout_idx].con, &qparams);
    return (virgl_renderer_gl_context)ctx;
}

static void virgl_destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    dpy_gl_ctx_destroy(g->parent_obj.scanout[0].con, qctx);
}

static int virgl_make_context_current(void *opaque, int scanout_idx,
                                      virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    return dpy_gl_ctx_make_current(g->parent_obj.scanout[scanout_idx].con,
                                   qctx);
}

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs = {
#if VIRGL_VERSION_MAJOR >= 1
    .version             = 3,
#else
    .version             = 1,
#endif
    .write_fence         = virgl_write_fence,
    .create_gl_context   = virgl_create_context,
    .destroy_gl_context  = virgl_destroy_context,
    .make_current        = virgl_make_context_current,
#if VIRGL_VERSION_MAJOR >= 1
    .write_context_fence = virgl_write_context_fence,
#endif
};

#else /* !CONFIG_OPENGL */

/*
 * Venus-only mode without OpenGL: provide no-op GL context callbacks.
 * Venus uses Vulkan rendering via virglrenderer and doesn't need GL contexts.
 * These stubs satisfy virglrenderer's callback requirements while indicating
 * no GL context is available.
 */
static virgl_renderer_gl_context
virgl_create_context_stub(void *opaque, int scanout_idx,
                          struct virgl_renderer_gl_ctx_param *params)
{
    /* No GL context available - Venus mode uses Vulkan */
    return NULL;
}

static void virgl_destroy_context_stub(void *opaque,
                                       virgl_renderer_gl_context ctx)
{
    /* Nothing to destroy */
}

static int virgl_make_context_current_stub(void *opaque, int scanout_idx,
                                           virgl_renderer_gl_context ctx)
{
    /* No GL context to make current - return success for NULL context */
    return ctx == NULL ? 0 : -1;
}

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs = {
#if VIRGL_VERSION_MAJOR >= 1
    .version             = 3,
#else
    .version             = 1,
#endif
    .write_fence         = virgl_write_fence,
    .create_gl_context   = virgl_create_context_stub,
    .destroy_gl_context  = virgl_destroy_context_stub,
    .make_current        = virgl_make_context_current_stub,
#if VIRGL_VERSION_MAJOR >= 1
    .write_context_fence = virgl_write_context_fence,
#endif
};
#endif /* CONFIG_OPENGL */

static void virtio_gpu_print_stats(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (g->stats.requests) {
        fprintf(stderr, "stats: vq req %4d, %3d -- 3D %4d (%5d)\n",
                g->stats.requests,
                g->stats.max_inflight,
                g->stats.req_3d,
                g->stats.bytes_3d);
        g->stats.requests     = 0;
        g->stats.max_inflight = 0;
        g->stats.req_3d       = 0;
        g->stats.bytes_3d     = 0;
    } else {
        fprintf(stderr, "stats: idle\r");
    }
    timer_mod(gl->print_stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void virtio_gpu_fence_poll(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    virgl_renderer_poll();
    virtio_gpu_process_cmdq(g);
    if (!QTAILQ_EMPTY(&g->cmdq) || !QTAILQ_EMPTY(&g->fenceq)) {
        timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    }
}

void virtio_gpu_virgl_fence_poll(VirtIOGPU *g)
{
    virtio_gpu_fence_poll(g);
}

void virtio_gpu_virgl_reset_scanout(VirtIOGPU *g)
{
    int i;

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        dpy_gfx_replace_surface(g->parent_obj.scanout[i].con, NULL);
#ifdef CONFIG_OPENGL
        dpy_gl_scanout_disable(g->parent_obj.scanout[i].con);
#endif
    }

#ifdef __APPLE__
    /* Destroy Vulkan swapchain on reset */
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    if (gl->vk_swapchain) {
        virtio_gpu_vk_swapchain_destroy(gl->vk_swapchain);
        gl->vk_swapchain = NULL;
        cocoa_set_metal_layer_enabled(false);
    }
#endif
}

void virtio_gpu_virgl_reset(VirtIOGPU *g)
{
    virgl_renderer_reset();
}

#ifdef __APPLE__
/*
 * On macOS, ensure the Vulkan loader can find MoltenVK ICD for Venus.
 * The Vulkan loader searches VK_ICD_FILENAMES (or VK_DRIVER_FILES) for
 * ICD manifest JSON files. If neither is set, try common MoltenVK paths.
 */
static void setup_moltenvk_icd(void)
{
    static const char *moltenvk_paths[] = {
        "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json",
        "/opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        NULL
    };
    const char *icd_env;

    icd_env = g_getenv("VK_ICD_FILENAMES");
    if (icd_env && *icd_env) {
        return;
    }
    icd_env = g_getenv("VK_DRIVER_FILES");
    if (icd_env && *icd_env) {
        return;
    }

    for (int i = 0; moltenvk_paths[i]; i++) {
        if (g_file_test(moltenvk_paths[i], G_FILE_TEST_EXISTS)) {
            g_setenv("VK_ICD_FILENAMES", moltenvk_paths[i], 0);
            return;
        }
    }

    error_report("MoltenVK ICD not found. Venus requires MoltenVK on macOS. "
                 "Install via: brew install molten-vk, or set VK_ICD_FILENAMES.");
}
#endif

int virtio_gpu_virgl_init(VirtIOGPU *g)
{
    int ret;
    uint32_t flags = 0;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4 && defined(CONFIG_OPENGL)
    if (qemu_egl_display) {
        virtio_gpu_3d_cbs.version = 4;
        virtio_gpu_3d_cbs.get_egl_display = virgl_get_egl_display;
    }
#endif
#if defined(VIRGL_RENDERER_D3D11_SHARE_TEXTURE) && defined(CONFIG_OPENGL)
    if (qemu_egl_angle_d3d) {
        flags |= VIRGL_RENDERER_D3D11_SHARE_TEXTURE;
    }
#endif
#if VIRGL_VERSION_MAJOR >= 1
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        flags |= VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER;
#ifndef CONFIG_OPENGL
        /*
         * Skip vrend (OpenGL) initialization when OpenGL is not available.
         * Non-Venus context creates are handled as no-ops in QEMU.
         */
        flags |= VIRGL_RENDERER_NO_VIRGL;
#endif
#ifdef __APPLE__
        setup_moltenvk_icd();
#endif
    }
#endif

    ret = virgl_renderer_init(g, flags, &virtio_gpu_3d_cbs);
    if (ret != 0) {
        error_report("virgl could not be initialized: %d", ret);
        return ret;
    }

    gl->fence_poll = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                  virtio_gpu_fence_poll, g);

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        gl->print_stats = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       virtio_gpu_print_stats, g);
        timer_mod(gl->print_stats,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }

#if VIRGL_VERSION_MAJOR >= 1
    gl->cmdq_resume_bh = aio_bh_new(qemu_get_aio_context(),
                                    virtio_gpu_virgl_resume_cmdq_bh,
                                    g);
#endif

#ifdef __APPLE__
    /*
     * Initialize host-side Vulkan swapchain for Venus blob presentation.
     * This allows Venus to render to blobs and have QEMU present them
     * via a host Vulkan swapchain without guest swapchain support.
     */
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        void *metal_layer = cocoa_get_metal_layer();
        if (metal_layer) {
            uint32_t width = g->parent_obj.conf.xres;
            uint32_t height = g->parent_obj.conf.yres;

            gl->vk_swapchain = virtio_gpu_vk_swapchain_create(metal_layer,
                                                               width, height);
            if (gl->vk_swapchain) {
                cocoa_set_metal_layer_enabled(true);
                info_report("Venus: Host Vulkan swapchain initialized (%dx%d)",
                            width, height);
            } else {
                warn_report("Venus: Failed to create host Vulkan swapchain, "
                            "falling back to software scanout");
            }
        } else {
            info_report("Venus: No Metal layer available, using software scanout");
        }
    }
#endif

    return 0;
}

static void virtio_gpu_virgl_add_capset(GArray *capset_ids, uint32_t capset_id)
{
    g_array_append_val(capset_ids, capset_id);
}

GArray *virtio_gpu_virgl_get_capsets(VirtIOGPU *g)
{
    uint32_t capset_max_ver, capset_max_size;
    GArray *capset_ids;

    capset_ids = g_array_new(false, false, sizeof(uint32_t));

    /* VIRGL is always supported. */
    virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL);

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2,
                               &capset_max_ver,
                               &capset_max_size);
    if (capset_max_ver) {
        virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL2);
    }

    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS,
                                   &capset_max_ver,
                                   &capset_max_size);
        if (capset_max_size) {
            virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VENUS);
        }
    }

    return capset_ids;
}
