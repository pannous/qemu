/* ShaderToy Viewer - DRM Direct Rendering
 *
 * Simple DRM/GBM/Vulkan shader viewer without display server.
 * Usage: ./shadertoy_drm <vert.spv> <frag.spv> [duration_sec]
 *
 * Based on test_tri.c and vkcube_anim.c architecture.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
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

typedef struct {
    float iResolution[3];
    float iTime;
    float iMouse[4];
} UniformBufferObject;

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *p, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if ((bits & (1 << i)) && (p->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

static uint32_t *load_spv(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *data = malloc(*size);
    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <vert.spv> <frag.spv> [duration_sec]\n", argv[0]);
        printf("Example: %s vert.spv frag.spv 30\n", argv[0]);
        return 1;
    }

    const char *vert_path = argv[1];
    const char *frag_path = argv[2];
    float duration = (argc > 3) ? atof(argv[3]) : 30.0f;

    printf("ShaderToy Viewer - DRM\n"); fflush(stdout);
    printf("Vertex: %s\n", vert_path); fflush(stdout);
    printf("Fragment: %s\n", frag_path); fflush(stdout);
    printf("Duration: %.1fs\n", duration); fflush(stdout);

    // === DRM/GBM Setup ===
    printf("Opening DRM device...\n"); fflush(stdout);
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) { perror("/dev/dri/card0"); return 1; }
    printf("✓ DRM fd=%d\n", drm_fd); fflush(stdout);

    printf("Getting DRM resources...\n"); fflush(stdout);
    drmSetMaster(drm_fd);
    drmModeRes *res = drmModeGetResources(drm_fd);
    printf("✓ Got resources\n"); fflush(stdout);

    printf("Finding display...\n"); fflush(stdout);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) { printf("No display\n"); return 1; }

    drmModeModeInfo *mode = &conn->modes[0];
    uint32_t W = mode->hdisplay, H = mode->vdisplay;
    printf("Display: %ux%u\n", W, H); fflush(stdout);

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];

    // Double-buffered GBM
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    struct gbm_bo *bo[2];
    uint32_t fb_id[2];

    for (int i = 0; i < 2; i++) {
        bo[i] = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        uint32_t stride = gbm_bo_get_stride(bo[i]);
        uint32_t handles[4] = { gbm_bo_get_handle(bo[i]).u32 };
        uint32_t strides[4] = { stride };
        uint32_t offsets[4] = { 0 };
        drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_XRGB8888, handles, strides, offsets, &fb_id[i], 0);
    }

    // === Vulkan Setup (no extensions like vkcube) ===
    printf("\nCreating Vulkan instance...\n"); fflush(stdout);
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    };
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&inst_info, NULL, &instance));
    printf("✓ Instance created\n"); fflush(stdout);

    printf("Enumerating devices...\n"); fflush(stdout);
    uint32_t gpuCount = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    printf("✓ Found %u device(s)\n", gpuCount); fflush(stdout);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s\n", props.deviceName); fflush(stdout);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    // Device (no extensions like vkcube)
    printf("Creating device...\n"); fflush(stdout);
    float qp = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &qp
    };
    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info
    };
    VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &dev_info, NULL, &device));
    printf("✓ Device created\n"); fflush(stdout);

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    printf("✓ Got queue\n"); fflush(stdout);

    // TEST: Try creating descriptor pool early to see if it works
    printf("TEST: Creating descriptor pool early...\n"); fflush(stdout);
    VkDescriptorPool testPool;
    VkResult testResult = vkCreateDescriptorPool(device, &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
    }, NULL, &testPool);
    if (testResult == VK_SUCCESS) {
        printf("✓ TEST descriptor pool created successfully!\n"); fflush(stdout);
        vkDestroyDescriptorPool(device, testPool, NULL);
    } else {
        printf("✗ TEST descriptor pool failed with error %d\n", testResult); fflush(stdout);
    }

    // Render target - LINEAR + HOST_VISIBLE
    printf("Creating render target image %ux%u...\n", W, H); fflush(stdout);
    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    };
    printf("  Calling vkCreateImage...\n"); fflush(stdout);
    VkImage rtImg;
    VkResult img_res = vkCreateImage(device, &img_info, NULL, &rtImg);
    if (img_res != VK_SUCCESS) {
        printf("ERROR: vkCreateImage failed with %d\n", img_res);
        return 1;
    }
    printf("✓ Image created\n"); fflush(stdout);

    printf("Getting memory requirements...\n"); fflush(stdout);
    VkMemoryRequirements rtReq;
    vkGetImageMemoryRequirements(device, rtImg, &rtReq);
    printf("  Memory size: %llu bytes, alignment: %llu\n",
           (unsigned long long)rtReq.size,
           (unsigned long long)rtReq.alignment); fflush(stdout);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rtReq.size,
        .memoryTypeIndex = find_mem(&memProps, rtReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    VkDeviceMemory rtMem;
    VK_CHECK(vkAllocateMemory(device, &alloc_info, NULL, &rtMem));
    printf("✓ Memory allocated\n"); fflush(stdout);

    VK_CHECK(vkBindImageMemory(device, rtImg, rtMem, 0));
    printf("✓ Memory bound\n"); fflush(stdout);

    void *rtPtr;
    VK_CHECK(vkMapMemory(device, rtMem, 0, VK_WHOLE_SIZE, 0, &rtPtr));
    printf("✓ Memory mapped\n"); fflush(stdout);

    printf("Creating image view...\n"); fflush(stdout);
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = rtImg,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView rtView;
    VK_CHECK(vkCreateImageView(device, &view_info, NULL, &rtView));
    printf("✓ Image view created\n"); fflush(stdout);

    // Render pass
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
    VkRenderPass renderPass;
    VK_CHECK(vkCreateRenderPass(device, &rp_info, NULL, &renderPass));
    printf("✓ Render pass created\n"); fflush(stdout);

    printf("Creating framebuffer...\n"); fflush(stdout);
    VkFramebufferCreateInfo fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = renderPass,
        .attachmentCount = 1,
        .pAttachments = &rtView,
        .width = W,
        .height = H,
        .layers = 1
    };
    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));
    printf("✓ Framebuffer created\n"); fflush(stdout);

    // Load shaders
    printf("Loading shaders...\n"); fflush(stdout);
    size_t vsz, fsz;
    uint32_t *vc = load_spv(vert_path, &vsz);
    uint32_t *fc = load_spv(frag_path, &fsz);
    if (!vc || !fc) { printf("Failed to load shaders\n"); return 1; }

    VkShaderModuleCreateInfo vs_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vsz,
        .pCode = vc
    };
    VkShaderModuleCreateInfo fs_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fsz,
        .pCode = fc
    };
    VkShaderModule vm, fm;
    VK_CHECK(vkCreateShaderModule(device, &vs_info, NULL, &vm));
    VK_CHECK(vkCreateShaderModule(device, &fs_info, NULL, &fm));
    printf("✓ Shader modules created\n"); fflush(stdout);

    // Uniform buffer
    printf("Creating uniform buffer...\n"); fflush(stdout);
    VkBufferCreateInfo uboBuf_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(UniformBufferObject),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
    };
    VkBuffer uboBuf;
    printf("  Calling vkCreateBuffer...\n"); fflush(stdout);
    VK_CHECK(vkCreateBuffer(device, &uboBuf_info, NULL, &uboBuf));
    printf("  ✓ Buffer created\n"); fflush(stdout);

    printf("  Getting buffer memory requirements...\n"); fflush(stdout);
    VkMemoryRequirements uboReq;
    vkGetBufferMemoryRequirements(device, uboBuf, &uboReq);
    printf("  ✓ Got requirements (size: %llu)\n", (unsigned long long)uboReq.size); fflush(stdout);

    printf("  Allocating buffer memory...\n"); fflush(stdout);
    VkMemoryAllocateInfo uboAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = uboReq.size,
        .memoryTypeIndex = find_mem(&memProps, uboReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    VkDeviceMemory uboMem;
    VK_CHECK(vkAllocateMemory(device, &uboAlloc, NULL, &uboMem));
    printf("  ✓ Memory allocated\n"); fflush(stdout);

    printf("  Binding buffer memory...\n"); fflush(stdout);
    VK_CHECK(vkBindBufferMemory(device, uboBuf, uboMem, 0));
    printf("  ✓ Memory bound\n"); fflush(stdout);

    printf("  Mapping buffer memory...\n"); fflush(stdout);
    void *uboPtr;
    vkMapMemory(device, uboMem, 0, sizeof(UniformBufferObject), 0, &uboPtr);
    printf("  ✓ Memory mapped\n"); fflush(stdout);

    // Descriptor set
    printf("Creating descriptor set layout...\n"); fflush(stdout);
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
    };
    VkDescriptorSetLayoutCreateInfo descLayout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout descLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &descLayout_info, NULL, &descLayout));
    printf("✓ Descriptor set layout created\n"); fflush(stdout);

    printf("Creating pipeline layout...\n"); fflush(stdout);
    VkPipelineLayoutCreateInfo pipelineLayout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descLayout
    };
    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayout_info, NULL, &pipelineLayout));
    printf("✓ Pipeline layout created\n"); fflush(stdout);

    // Pipeline
    printf("Creating graphics pipeline...\n"); fflush(stdout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" }
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
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
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
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
        .layout = pipelineLayout,
        .renderPass = renderPass
    };
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline));
    printf("✓ Graphics pipeline created\n"); fflush(stdout);

    // Descriptor pool and set (using vkcube's inline style)
    printf("Creating descriptor pool...\n"); fflush(stdout);
    VkDescriptorPool descPool;
    VK_CHECK(vkCreateDescriptorPool(device, &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
    }, NULL, &descPool));
    printf("✓ Descriptor pool created!\n"); fflush(stdout);

    printf("Allocating descriptor set...\n"); fflush(stdout);
    VkDescriptorSet descSet;
    VK_CHECK(vkAllocateDescriptorSets(device, &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descLayout
    }, &descSet));
    printf("✓ Descriptor set allocated!\n"); fflush(stdout);

    printf("Updating descriptor sets...\n"); fflush(stdout);
    VkDescriptorBufferInfo bufferInfo = { uboBuf, 0, sizeof(UniformBufferObject) };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &bufferInfo
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    printf("✓ Descriptor sets updated!\n"); fflush(stdout);

    // Command pool/buffer
    printf("Creating command pool...\n"); fflush(stdout);
    VkCommandPoolCreateInfo cmdPool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    VkCommandPool cmdPool;
    VK_CHECK(vkCreateCommandPool(device, &cmdPool_info, NULL, &cmdPool));
    printf("✓ Command pool created!\n"); fflush(stdout);

    printf("Allocating command buffer...\n"); fflush(stdout);
    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd));
    printf("✓ Command buffer allocated!\n"); fflush(stdout);

    printf("Creating fence...\n"); fflush(stdout);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    printf("✓ Fence created!\n"); fflush(stdout);

    // Get image layout
    printf("Getting image subresource layout...\n"); fflush(stdout);
    VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, rtImg, &subres, &layout);
    printf("✓ Got image layout (offset: %llu, rowPitch: %llu)\n",
           (unsigned long long)layout.offset, (unsigned long long)layout.rowPitch); fflush(stdout);

    // Set initial mode
    printf("Setting initial DRM mode...\n"); fflush(stdout);
    drmModeSetCrtc(drm_fd, crtc_id, fb_id[0], 0, 0, &conn->connector_id, 1, mode);
    printf("✓ Initial mode set\n"); fflush(stdout);

    printf("\n✓ Running shader\n"); fflush(stdout);

    struct timespec start, last_frame, last_report;
    clock_gettime(CLOCK_MONOTONIC, &start);
    last_frame = start;
    last_report = start;
    int frames = 0;
    int frames_since_report = 0;
    int current_buffer = 0;

    const long target_frame_ns = 16666666; // 60 FPS

    // === Render Loop ===
    printf("Entering render loop...\n"); fflush(stdout);
    while (1) {
        printf("  Frame start...\n"); fflush(stdout);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9f;

        if (t >= duration) break;

        printf("  Updating uniforms (t=%.2f)...\n", t); fflush(stdout);
        // Update uniforms
        UniformBufferObject ubo;
        ubo.iResolution[0] = (float)W;
        ubo.iResolution[1] = (float)H;
        ubo.iResolution[2] = 1.0f;
        ubo.iTime = t;
        ubo.iMouse[0] = ubo.iMouse[1] = ubo.iMouse[2] = ubo.iMouse[3] = 0.0f;
        memcpy(uboPtr, &ubo, sizeof(ubo));
        printf("  Uniforms updated\n"); fflush(stdout);

        // Record
        printf("  Beginning command buffer...\n"); fflush(stdout);
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        };
        vkBeginCommandBuffer(cmd, &begin);
        printf("  Command buffer begun\n"); fflush(stdout);

        VkClearValue clear = { .color = { {0.0f, 0.0f, 0.0f, 1.0f} } };
        VkRenderPassBeginInfo rp_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = framebuffer,
            .renderArea = { {0, 0}, {W, H} },
            .clearValueCount = 1,
            .pClearValues = &clear
        };
        vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout, 0, 1, &descSet, 0, NULL);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        // Submit
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd
        };
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        // Copy to GBM
        void *gbmPtr = NULL;
        uint32_t gbmStride;
        void *mapData = NULL;
        gbmPtr = gbm_bo_map(bo[current_buffer], 0, 0, W, H,
            GBM_BO_TRANSFER_WRITE, &gbmStride, &mapData);
        if (gbmPtr) {
            for (uint32_t y = 0; y < H; y++) {
                memcpy((char*)gbmPtr + y * gbmStride,
                       (char*)rtPtr + layout.offset + y * layout.rowPitch,
                       W * 4);
            }
            gbm_bo_unmap(bo[current_buffer], mapData);
        }

        // Display
        drmModeSetCrtc(drm_fd, crtc_id, fb_id[current_buffer], 0, 0,
            &conn->connector_id, 1, mode);

        current_buffer = 1 - current_buffer;
        frames++;
        frames_since_report++;

        // FPS reporting
        float time_since_report = (now.tv_sec - last_report.tv_sec) +
                                  (now.tv_nsec - last_report.tv_nsec) / 1e9f;
        if (time_since_report >= 1.0f) {
            float fps = frames_since_report / time_since_report;
            printf("\rFrame %d: %.1f FPS | Time: %.1fs / %.1fs",
                frames, fps, t, duration);
            fflush(stdout);
            frames_since_report = 0;
            last_report = now;
        }

        // Frame limiting
        struct timespec frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long frame_time_ns = (frame_end.tv_sec - now.tv_sec) * 1000000000L +
                            (frame_end.tv_nsec - now.tv_nsec);
        long sleep_ns = target_frame_ns - frame_time_ns;
        if (sleep_ns > 0) {
            struct timespec sleep_time = { 0, sleep_ns };
            nanosleep(&sleep_time, NULL);
        }
    }

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    float total_time = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1e9f;
    printf("\n\n✓ Done! %d frames in %.2fs (%.1f fps avg)\n",
           frames, total_time, frames / total_time);

    // Cleanup
    vkUnmapMemory(device, rtMem);
    vkUnmapMemory(device, uboMem);
    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmdPool, NULL);
    vkDestroyDescriptorPool(device, descPool, NULL);
    vkDestroyBuffer(device, uboBuf, NULL);
    vkFreeMemory(device, uboMem, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyDescriptorSetLayout(device, descLayout, NULL);
    vkDestroyShaderModule(device, vm, NULL);
    vkDestroyShaderModule(device, fm, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, renderPass, NULL);
    vkDestroyImageView(device, rtView, NULL);
    vkDestroyImage(device, rtImg, NULL);
    vkFreeMemory(device, rtMem, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    for (int i = 0; i < 2; i++) {
        drmModeRmFB(drm_fd, fb_id[i]);
        gbm_bo_destroy(bo[i]);
    }
    gbm_device_destroy(gbm);
    close(drm_fd);

    free(vc);
    free(fc);

    return 0;
}
