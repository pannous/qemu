/*
 * Test virtio-gpu blob memory mapping
 * Compile: gcc -o test_blob_mmap test_blob_mmap.c -ldrm
 * Run: ./test_blob_mmap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

/* virtio-gpu DRM ioctls - from drm/virtgpu_drm.h */
#define DRM_VIRTGPU_GETPARAM       0x03
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define DRM_VIRTGPU_MAP            0x01
#define DRM_VIRTGPU_CONTEXT_INIT   0x0b

#define DRM_IOCTL_VIRTGPU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM, struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_MAP \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_MAP, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB, struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT, struct drm_virtgpu_context_init)

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;
    uint64_t blob_id;
};

struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

/* Parameters */
#define VIRTGPU_PARAM_3D_FEATURES      1
#define VIRTGPU_PARAM_RESOURCE_BLOB    6
#define VIRTGPU_PARAM_HOST_VISIBLE     7
#define VIRTGPU_PARAM_CONTEXT_INIT     10

/* Capset IDs */
#define VIRTGPU_CAPSET_VENUS 4

/* Context params */
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID   0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS   0x0002

/* Blob types */
#define VIRTGPU_BLOB_MEM_GUEST       0x0001
#define VIRTGPU_BLOB_MEM_HOST3D      0x0002
#define VIRTGPU_BLOB_MEM_HOST3D_GUEST 0x0003

#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE  0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002

static jmp_buf jump_buffer;
static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig) {
    got_signal = sig;
    longjmp(jump_buffer, 1);
}

static int get_param(int fd, uint64_t param, uint64_t *value) {
    struct drm_virtgpu_getparam args = { .param = param };
    int ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &args);
    if (ret == 0) *value = args.value;
    return ret;
}

int main(int argc, char **argv) {
    const char *device = "/dev/dri/renderD128";
    int fd, ret;
    uint64_t value;

    printf("=== Virtio-GPU Blob Memory Test ===\n\n");

    /* Open DRM device */
    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Failed to open DRM device");
        return 1;
    }
    printf("[OK] Opened %s (fd=%d)\n", device, fd);

    /* Check parameters */
    printf("\n--- Kernel Parameters ---\n");

    if (get_param(fd, VIRTGPU_PARAM_3D_FEATURES, &value) == 0)
        printf("[OK] 3D_FEATURES: %lu\n", value);
    else
        printf("[FAIL] 3D_FEATURES not supported\n");

    if (get_param(fd, VIRTGPU_PARAM_RESOURCE_BLOB, &value) == 0)
        printf("[OK] RESOURCE_BLOB: %lu\n", value);
    else
        printf("[FAIL] RESOURCE_BLOB not supported\n");

    if (get_param(fd, VIRTGPU_PARAM_HOST_VISIBLE, &value) == 0)
        printf("[OK] HOST_VISIBLE: %lu\n", value);
    else
        printf("[INFO] HOST_VISIBLE: not supported (using guest memory)\n");

    if (get_param(fd, VIRTGPU_PARAM_CONTEXT_INIT, &value) == 0)
        printf("[OK] CONTEXT_INIT: %lu\n", value);
    else
        printf("[FAIL] CONTEXT_INIT not supported\n");

    /* Initialize Venus context */
    printf("\n--- Context Init (Venus) ---\n");
    struct drm_virtgpu_context_set_param ctx_params[2] = {
        { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID, .value = VIRTGPU_CAPSET_VENUS },
        { .param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS, .value = 64 },
    };
    struct drm_virtgpu_context_init ctx_init = {
        .num_params = 2,
        .ctx_set_params = (uint64_t)(uintptr_t)ctx_params,
    };

    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ctx_init);
    if (ret) {
        printf("[FAIL] CONTEXT_INIT failed: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("[OK] Venus context initialized\n");

    /* Create blob resource */
    printf("\n--- Blob Creation ---\n");
    size_t blob_size = 4096;  /* 1 page */
    struct drm_virtgpu_resource_create_blob blob_create = {
        .blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
        .blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
        .size = blob_size,
        .blob_id = 0,
    };

    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob_create);
    if (ret) {
        printf("[FAIL] CREATE_BLOB failed: %s (errno=%d)\n", strerror(errno), errno);
        /* Try with guest memory */
        printf("[INFO] Retrying with BLOB_MEM_GUEST...\n");
        blob_create.blob_mem = VIRTGPU_BLOB_MEM_GUEST;
        ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &blob_create);
        if (ret) {
            printf("[FAIL] CREATE_BLOB (guest) failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    }
    printf("[OK] Blob created: bo_handle=%u, res_handle=%u, size=%lu\n",
           blob_create.bo_handle, blob_create.res_handle, blob_size);

    /* Map blob */
    printf("\n--- Blob Mapping ---\n");
    struct drm_virtgpu_map map_args = {
        .handle = blob_create.bo_handle,
    };

    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_MAP, &map_args);
    if (ret) {
        printf("[FAIL] MAP failed: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("[OK] MAP returned offset: 0x%lx\n", map_args.offset);

    /* mmap the blob */
    printf("\n--- Memory Mapping (mmap) ---\n");
    void *ptr = mmap(NULL, blob_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, map_args.offset);
    if (ptr == MAP_FAILED) {
        printf("[FAIL] mmap failed: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("[OK] mmap succeeded: ptr=%p\n", ptr);

    /* Test memory access with signal handling */
    printf("\n--- Memory Access Test ---\n");

    struct sigaction sa, old_sa_bus, old_sa_segv;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGBUS, &sa, &old_sa_bus);
    sigaction(SIGSEGV, &sa, &old_sa_segv);

    if (setjmp(jump_buffer) == 0) {
        /* Try to read */
        volatile uint32_t val = *(volatile uint32_t *)ptr;
        printf("[OK] Read succeeded: value=0x%08x\n", val);

        /* Try to write */
        *(volatile uint32_t *)ptr = 0xDEADBEEF;
        printf("[OK] Write succeeded\n");

        /* Read back */
        val = *(volatile uint32_t *)ptr;
        if (val == 0xDEADBEEF) {
            printf("[OK] Read-back verified: 0x%08x\n", val);
        } else {
            printf("[WARN] Read-back mismatch: expected 0xDEADBEEF, got 0x%08x\n", val);
        }

        /* Write pattern to whole page */
        printf("[INFO] Writing pattern to entire blob...\n");
        memset(ptr, 0xAA, blob_size);
        printf("[OK] memset completed\n");

        /* Verify pattern */
        unsigned char *bytes = (unsigned char *)ptr;
        int mismatches = 0;
        for (size_t i = 0; i < blob_size; i++) {
            if (bytes[i] != 0xAA) mismatches++;
        }
        if (mismatches == 0) {
            printf("[OK] Pattern verified for entire blob\n");
        } else {
            printf("[FAIL] %d mismatches in pattern\n", mismatches);
        }
    } else {
        printf("[FAIL] Got signal %d (%s) during memory access!\n",
               got_signal, got_signal == SIGBUS ? "SIGBUS" : "SIGSEGV");
        printf("       This means the blob memory mapping is broken.\n");
    }

    sigaction(SIGBUS, &old_sa_bus, NULL);
    sigaction(SIGSEGV, &old_sa_segv, NULL);

    /* Cleanup */
    munmap(ptr, blob_size);
    close(fd);

    printf("\n=== Test Complete ===\n");
    return 0;
}
