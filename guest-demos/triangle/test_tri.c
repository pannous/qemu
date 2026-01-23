/* Zero-copy-ish Vulkan triangle demo (host-present path)
 *
 * Architecture:
 *   VkImage (LINEAR, HOST_VISIBLE) ── render on host GPU
 *        │
 *        ├─→ QEMU presents hostptr via Vulkan swapchain (no guest CPU copy)
 *        └─→ GBM blob (SCANOUT) only used to trigger SET_SCANOUT_BLOB
 *
 * Why: We want the host to present directly from Venus' host-visible allocation,
 *      avoiding the guest-side memcpy into GBM.
 * Alternatives:
 *   1) True dmabuf import of the GBM buffer (blocked by resource ID mismatch).
 *   2) IOSurface path (host-side copy from blob).
 *   3) Guest CPU copy to GBM (current fallback path).
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(x) do { \
    VkResult r = (x); \
    if (r) { printf("VK err %d @ line %d\n", r, __LINE__); exit(1); } \
} while(0)

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *p, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if ((bits & (1 << i)) && (p->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static uint32_t *load_spv(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *data = malloc(*size);
    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

int main(void) {
    printf("Starting...\n"); fflush(stdout);

    // === DRM/GBM Setup ===
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }
    printf("Opened DRM fd=%d\n", drm_fd); fflush(stdout);

    // Become DRM master (needed for modesetting)
    if (drmSetMaster(drm_fd) < 0) {
        printf("Warning: drmSetMaster failed: %s (continuing anyway)\n", strerror(errno));
    } else {
        printf("Became DRM master\n");
    }
    fflush(stdout);

    drmModeRes *res = drmModeGetResources(drm_fd);
    printf("Got resources res=%p\n", (void*)res); fflush(stdout);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) { printf("No connected display\n"); return 1; }
    printf("Found connector\n"); fflush(stdout);

    drmModeModeInfo *mode = &conn->modes[0];
    uint32_t W = mode->hdisplay, H = mode->vdisplay;
    printf("Display: %ux%u\n", W, H); fflush(stdout);

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];
    printf("Got encoder, crtc_id=%u\n", crtc_id); fflush(stdout);

    // Create GBM device and scanout buffer
    printf("Creating GBM device...\n"); fflush(stdout);
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    printf("GBM device=%p\n", (void*)gbm); fflush(stdout);
    printf("Creating GBM bo...\n"); fflush(stdout);
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    printf("GBM bo=%p\n", (void*)bo); fflush(stdout);
    if (!bo) { printf("Failed to create GBM buffer\n"); return 1; }

    printf("Getting stride...\n"); fflush(stdout);
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("stride=%u\n", stride); fflush(stdout);
    printf("Getting fd...\n"); fflush(stdout);
    int prime_fd = gbm_bo_get_fd(bo);
    printf("GBM blob: stride=%u, prime_fd=%d\n", stride, prime_fd); fflush(stdout);

    // Create DRM framebuffer from GBM buffer
    printf("Creating DRM framebuffer...\n"); fflush(stdout);
    uint32_t fb_id;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
    uint32_t strides[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    printf("handle=%u\n", handles[0]); fflush(stdout);
    if (drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_XRGB8888, handles, strides, offsets, &fb_id, 0)) {
        printf("Failed to create framebuffer\n");
        return 1;
    }
    printf("Created framebuffer fb_id=%u\n", fb_id); fflush(stdout);

    // === Vulkan Setup with External Memory ===
    printf("Creating Vulkan instance...\n"); fflush(stdout);
    const char *inst_exts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = inst_exts
    };
    VkInstance instance;
    VkResult res_inst = vkCreateInstance(&inst_info, NULL, &instance);
    printf("vkCreateInstance returned %d\n", res_inst); fflush(stdout);
    if (res_inst != VK_SUCCESS) { printf("VK err %d @ instance creation\n", res_inst); return 1; }

    printf("Enumerating physical devices...\n"); fflush(stdout);
    uint32_t gpu_count = 1;
    VkPhysicalDevice gpu;
    VkResult res_enum = vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);
    printf("vkEnumeratePhysicalDevices returned %d, count=%u\n", res_enum, gpu_count); fflush(stdout);

    printf("Getting device properties...\n"); fflush(stdout);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s\n", props.deviceName); fflush(stdout);

    printf("Getting memory properties...\n"); fflush(stdout);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
    printf("Memory types: %u, heaps: %u\n", mem_props.memoryTypeCount, mem_props.memoryHeapCount); fflush(stdout);

    // Create device with external memory extensions
    printf("Creating device...\n"); fflush(stdout);
    const char *dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME
    };
    float qp = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &qp
    };
    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 3,
        .ppEnabledExtensionNames = dev_exts
    };
    VkDevice device;
    VkResult res_dev = vkCreateDevice(gpu, &dev_info, NULL, &device);
    printf("vkCreateDevice returned %d\n", res_dev); fflush(stdout);
    if (res_dev != VK_SUCCESS) { printf("VK err %d @ device creation\n", res_dev); return 1; }

    printf("Getting device queue...\n"); fflush(stdout);
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    printf("Queue=%p\n", (void*)queue); fflush(stdout);

    // === Import GBM buffer as VkImage (ZERO-COPY!) ===
    printf("Creating VkImage...\n"); fflush(stdout);

    // Create image - we'll import the GBM memory separately
    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkImage render_img;
    VkResult res_img = vkCreateImage(device, &img_info, NULL, &render_img);
    printf("vkCreateImage returned %d\n", res_img); fflush(stdout);
    if (res_img != VK_SUCCESS) { printf("VK err %d @ image creation\n", res_img); return 1; }

    // Get memory requirements for the image
    printf("Getting image memory requirements...\n"); fflush(stdout);
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, render_img, &mem_req);
    printf("Image memory: size=%llu, alignment=%llu, typeBits=0x%x\n",
           (unsigned long long)mem_req.size,
           (unsigned long long)mem_req.alignment,
           mem_req.memoryTypeBits); fflush(stdout);

    // Print memory types
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        printf("  MemType %u: flags=0x%x heap=%u %s\n", i,
               mem_props.memoryTypes[i].propertyFlags,
               mem_props.memoryTypes[i].heapIndex,
               (mem_req.memoryTypeBits & (1 << i)) ? "(compatible)" : ""); fflush(stdout);
    }

    // Find HOST_VISIBLE memory type for CPU access (needed for copy to GBM)
    printf("Finding HOST_VISIBLE memory type...\n"); fflush(stdout);
    uint32_t mem_type = find_mem(&mem_props, mem_req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        // Fallback to just HOST_VISIBLE
        mem_type = find_mem(&mem_props, mem_req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (mem_type == UINT32_MAX) {
        printf("ERROR: No HOST_VISIBLE memory type found!\n");
        return 1;
    }
    printf("Using memory type: %u (HOST_VISIBLE)\n", mem_type); fflush(stdout);

    // Close the GBM prime_fd - we only need the GBM BO for scanout metadata.
    close(prime_fd);

    // Allocate HOST_VISIBLE memory for rendering. Host will present this via swapchain.
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type
    };

    VkDeviceMemory render_mem;
    VkResult res_alloc = vkAllocateMemory(device, &alloc_info, NULL, &render_mem);
    printf("vkAllocateMemory returned %d\n", res_alloc); fflush(stdout);
    if (res_alloc != VK_SUCCESS) { printf("VK err %d @ alloc\n", res_alloc); return 1; }

    printf("Binding memory...\n"); fflush(stdout);
    VkResult res_bind = vkBindImageMemory(device, render_img, render_mem, 0);
    printf("vkBindImageMemory returned %d\n", res_bind); fflush(stdout);
    if (res_bind != VK_SUCCESS) { printf("VK err %d @ bind\n", res_bind); return 1; }

    printf("Done with memory setup (HOST_VISIBLE, no guest copy)\n"); fflush(stdout);

    // Create image view
    printf("Creating image view...\n"); fflush(stdout);
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = render_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView render_view;
    VkResult res_view = vkCreateImageView(device, &view_info, NULL, &render_view);
    printf("vkCreateImageView returned %d\n", res_view); fflush(stdout);
    if (res_view != VK_SUCCESS) { printf("VK err %d @ view\n", res_view); return 1; }

    // === Render Pass & Framebuffer ===
    printf("Creating render pass...\n"); fflush(stdout);
    VkAttachmentDescription att = {
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL
    };
    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref
    };
    VkRenderPassCreateInfo rp_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPass render_pass;
    VkResult res_rp = vkCreateRenderPass(device, &rp_info, NULL, &render_pass);
    printf("vkCreateRenderPass returned %d\n", res_rp); fflush(stdout);
    if (res_rp != VK_SUCCESS) { printf("VK err %d @ render pass\n", res_rp); return 1; }

    printf("Creating framebuffer...\n"); fflush(stdout);
    VkFramebufferCreateInfo fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &render_view,
        .width = W,
        .height = H,
        .layers = 1
    };
    VkFramebuffer framebuffer;
    VkResult res_fb = vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer);
    printf("vkCreateFramebuffer returned %d\n", res_fb); fflush(stdout);
    if (res_fb != VK_SUCCESS) { printf("VK err %d @ framebuffer\n", res_fb); return 1; }

    // === Pipeline ===
    printf("Loading shaders...\n"); fflush(stdout);
    size_t vs_size, fs_size;
    uint32_t *vs_code = load_spv("/root/tri.vert.spv", &vs_size);
    uint32_t *fs_code = load_spv("/root/tri.frag.spv", &fs_size);
    printf("vs_code=%p vs_size=%zu, fs_code=%p fs_size=%zu\n", (void*)vs_code, vs_size, (void*)fs_code, fs_size); fflush(stdout);
    if (!vs_code || !fs_code) { printf("Failed to load shaders\n"); return 1; }

    printf("Creating shader modules...\n"); fflush(stdout);
    VkShaderModuleCreateInfo vs_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vs_size,
        .pCode = vs_code
    };
    VkShaderModuleCreateInfo fs_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fs_size,
        .pCode = fs_code
    };
    VkShaderModule vs_mod, fs_mod;
    VkResult res_vs = vkCreateShaderModule(device, &vs_info, NULL, &vs_mod);
    printf("vkCreateShaderModule (vert) returned %d\n", res_vs); fflush(stdout);
    if (res_vs != VK_SUCCESS) { printf("VK err %d @ vs shader\n", res_vs); return 1; }
    VkResult res_fs = vkCreateShaderModule(device, &fs_info, NULL, &fs_mod);
    printf("vkCreateShaderModule (frag) returned %d\n", res_fs); fflush(stdout);
    if (res_fs != VK_SUCCESS) { printf("VK err %d @ fs shader\n", res_fs); return 1; }

    printf("Creating pipeline layout...\n"); fflush(stdout);
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pipeline_layout;
    VkResult res_layout = vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout);
    printf("vkCreatePipelineLayout returned %d\n", res_layout); fflush(stdout);
    if (res_layout != VK_SUCCESS) { printf("VK err %d @ layout\n", res_layout); return 1; }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_mod, .pName = "main" }
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { {0, 0}, {W, H} };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1, .pScissors = &sc
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba
    };

    printf("Creating graphics pipeline...\n"); fflush(stdout);
    VkGraphicsPipelineCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .layout = pipeline_layout,
        .renderPass = render_pass
    };
    VkPipeline pipeline;
    VkResult res_pipe = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline);
    printf("vkCreateGraphicsPipelines returned %d\n", res_pipe); fflush(stdout);
    if (res_pipe != VK_SUCCESS) { printf("VK err %d @ pipeline\n", res_pipe); return 1; }

    // === Command Buffer ===
    printf("Creating command pool...\n"); fflush(stdout);
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cmd_pool;
    VkResult res_pool = vkCreateCommandPool(device, &pool_info, NULL, &cmd_pool);
    printf("vkCreateCommandPool returned %d\n", res_pool); fflush(stdout);
    if (res_pool != VK_SUCCESS) { printf("VK err %d @ cmd pool\n", res_pool); return 1; }

    printf("Allocating command buffer...\n"); fflush(stdout);
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd;
    VkResult res_cmd = vkAllocateCommandBuffers(device, &cmd_alloc, &cmd);
    printf("vkAllocateCommandBuffers returned %d\n", res_cmd); fflush(stdout);
    if (res_cmd != VK_SUCCESS) { printf("VK err %d @ cmd alloc\n", res_cmd); return 1; }

    printf("Creating fence...\n"); fflush(stdout);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VkResult res_fence = vkCreateFence(device, &fence_info, NULL, &fence);
    printf("vkCreateFence returned %d\n", res_fence); fflush(stdout);
    if (res_fence != VK_SUCCESS) { printf("VK err %d @ fence\n", res_fence); return 1; }

    // === Render ===
    printf("Starting render...\n"); fflush(stdout);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clear = { .color = { { 0.0f, 0.0f, 0.3f, 1.0f } } };
    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea = { {0, 0}, {W, H} },
        .clearValueCount = 1,
        .pClearValues = &clear
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd
    };
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    printf("Rendered triangle\n"); fflush(stdout);

    // No guest-side copy: QEMU will present the host-visible allocation directly.
    VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, render_img, &subres, &layout);
    if (layout.rowPitch != stride) {
        printf("WARNING: VkImage rowPitch (%llu) != GBM stride (%u)\n",
               (unsigned long long)layout.rowPitch, stride);
    }

    // Scanout the GBM buffer - use various methods to display
    printf("Setting DRM scanout...\n");
    printf("  crtc_id=%u, fb_id=%u\n", crtc_id, fb_id);
    printf("  connector_id=%u\n", conn->connector_id);
    printf("  mode: %s (%ux%u @ %uHz)\n", mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh);
    fflush(stdout);

    int ret = -1;

    // Method 1: Try drmModeDirtyFB to mark the framebuffer as dirty
    // This tells the display to refresh from the framebuffer
    drmModeClip clip = { 0, 0, W, H };
    ret = drmModeDirtyFB(drm_fd, fb_id, &clip, 1);
    if (ret == 0) {
        printf("drmModeDirtyFB succeeded - buffer marked for display\n");
    } else {
        printf("drmModeDirtyFB returned %d: %s\n", ret, strerror(errno));
    }

    // Method 2: Try drmModeSetCrtc
    ret = drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode);
    if (ret == 0) {
        printf("drmModeSetCrtc succeeded!\n");
    } else {
        printf("drmModeSetCrtc returned %d: %s\n", ret, strerror(errno));
    }
    fflush(stdout);

    printf("RGB triangle on blue (5s)\n"); fflush(stdout);
    sleep(5);

    // Cleanup
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyShaderModule(device, vs_mod, NULL);
    vkDestroyShaderModule(device, fs_mod, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, render_pass, NULL);
    vkDestroyImageView(device, render_view, NULL);
    vkDestroyImage(device, render_img, NULL);
    vkFreeMemory(device, render_mem, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    drmModeRmFB(drm_fd, fb_id);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(drm_fd);

    free(vs_code);
    free(fs_code);

    return 0;
}
