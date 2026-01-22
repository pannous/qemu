/* Zero-copy Vulkan triangle demo
 *
 * Architecture:
 *   GBM blob (SCANOUT) ←─ import fd ─→ VkImage ←─ render
 *        │
 *        └─→ DRM scanout (same memory, no copy!)
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
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
    // === DRM/GBM Setup ===
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }

    drmModeRes *res = drmModeGetResources(drm_fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) { printf("No connected display\n"); return 1; }

    drmModeModeInfo *mode = &conn->modes[0];
    uint32_t W = mode->hdisplay, H = mode->vdisplay;
    printf("Display: %ux%u\n", W, H);

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];

    // Create GBM device and scanout buffer
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) { printf("Failed to create GBM buffer\n"); return 1; }

    uint32_t stride = gbm_bo_get_stride(bo);
    int prime_fd = gbm_bo_get_fd(bo);
    printf("GBM blob: stride=%u, prime_fd=%d\n", stride, prime_fd);

    // Create DRM framebuffer from GBM buffer
    uint32_t fb_id;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
    uint32_t strides[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_ARGB8888, handles, strides, offsets, &fb_id, 0)) {
        printf("Failed to create framebuffer\n");
        return 1;
    }

    // === Vulkan Setup with External Memory ===
    const char *inst_exts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = inst_exts
    };
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&inst_info, NULL, &instance));

    uint32_t gpu_count = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s\n", props.deviceName);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);

    // Create device with external memory extensions
    const char *dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
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
        .enabledExtensionCount = 4,
        .ppEnabledExtensionNames = dev_exts
    };
    VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &dev_info, NULL, &device));

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // === Import GBM buffer as VkImage (ZERO-COPY!) ===

    // Get DRM format modifier from GBM
    uint64_t modifier = gbm_bo_get_modifier(bo);
    printf("DRM modifier: 0x%llx\n", (unsigned long long)modifier);

    // Create VkImage with external memory and DRM format modifier
    VkSubresourceLayout plane_layout = {
        .offset = 0,
        .size = 0,  // Derived from image
        .rowPitch = stride,
        .arrayPitch = 0,
        .depthPitch = 0
    };

    VkImageDrmFormatModifierExplicitCreateInfoEXT drm_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout
    };

    VkExternalMemoryImageCreateInfo ext_img_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };

    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkImage render_img;
    VK_CHECK(vkCreateImage(device, &img_info, NULL, &render_img));

    // Import DMA-BUF fd as VkDeviceMemory
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, render_img, &mem_req);
    printf("Image memory: size=%llu, alignment=%llu\n",
           (unsigned long long)mem_req.size,
           (unsigned long long)mem_req.alignment);

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = prime_fd  // Ownership transferred to Vulkan
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = find_mem(&mem_props, mem_req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };

    VkDeviceMemory render_mem;
    VK_CHECK(vkAllocateMemory(device, &alloc_info, NULL, &render_mem));
    VK_CHECK(vkBindImageMemory(device, render_img, render_mem, 0));

    printf("Imported GBM buffer into Vulkan (zero-copy)\n");

    // Create image view
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = render_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView render_view;
    VK_CHECK(vkCreateImageView(device, &view_info, NULL, &render_view));

    // === Render Pass & Framebuffer ===
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
    VK_CHECK(vkCreateRenderPass(device, &rp_info, NULL, &render_pass));

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
    VK_CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));

    // === Pipeline ===
    size_t vs_size, fs_size;
    uint32_t *vs_code = load_spv("/root/tri.vert.spv", &vs_size);
    uint32_t *fs_code = load_spv("/root/tri.frag.spv", &fs_size);
    if (!vs_code || !fs_code) { printf("Failed to load shaders\n"); return 1; }

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
    VK_CHECK(vkCreateShaderModule(device, &vs_info, NULL, &vs_mod));
    VK_CHECK(vkCreateShaderModule(device, &fs_info, NULL, &fs_mod));

    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pipeline_layout;
    VK_CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout));

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
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));

    // === Command Buffer ===
    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cmd_pool;
    VK_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &cmd_pool));

    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd));

    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

    // === Render ===
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

    printf("Rendered to GBM buffer (zero-copy)\n");

    // === Scanout - NO COPY NEEDED! ===
    // The GBM buffer IS the render target, just flip it to display
    drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode);

    printf("RGB triangle on blue - zero-copy scanout! (5s)\n");
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
