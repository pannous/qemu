/* Native macOS MoltenVK Rotating Gradient Cube Demo
 * Performance baseline for QEMU+Venus comparison
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define WIDTH 800
#define HEIGHT 600
#define MAX_FRAMES 3

// Rainbow cube vertices: position (x,y,z) + color (r,g,b)
static const float cube_verts[] = {
    // Front face
    -1,-1, 1,  1,0,0,   1,-1, 1,  1,1,0,   1, 1, 1,  0,1,0,
    -1,-1, 1,  1,0,0,   1, 1, 1,  0,1,0,  -1, 1, 1,  1,0,1,
    // Back face
     1,-1,-1,  0,1,1,  -1,-1,-1,  1,0.5,0,  -1, 1,-1,  1,0,0,
     1,-1,-1,  0,1,1,  -1, 1,-1,  1,0,0,   1, 1,-1,  0,0,1,
    // Top face
    -1, 1, 1,  0,0,1,   1, 1, 1,  1,1,0,   1, 1,-1,  1,0,1,
    -1, 1, 1,  0,0,1,   1, 1,-1,  1,0,1,  -1, 1,-1,  0,1,0,
    // Bottom face
    -1,-1,-1,  1,0,1,   1,-1,-1,  0,1,1,   1,-1, 1,  1,1,0,
    -1,-1,-1,  1,0,1,   1,-1, 1,  1,1,0,  -1,-1, 1,  0,1,0,
    // Right face
     1,-1, 1,  0,1,0,   1,-1,-1,  1,0,0,   1, 1,-1,  0,0,1,
     1,-1, 1,  0,1,0,   1, 1,-1,  0,0,1,   1, 1, 1,  1,1,0,
    // Left face
    -1,-1,-1,  0,0,0,  -1,-1, 1,  1,0,0,  -1, 1, 1,  1,1,1,
    -1,-1,-1,  0,0,0,  -1, 1, 1,  1,1,1,  -1, 1,-1,  0,0,1,
};

typedef float mat4[16];
void mat4_identity(mat4 m) { memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.0f; }
void mat4_mul(mat4 out, const mat4 a, const mat4 b) {
    mat4 tmp;
    for(int c=0;c<4;c++) for(int r=0;r<4;r++)
        tmp[c*4+r] = a[0*4+r]*b[c*4+0]+a[1*4+r]*b[c*4+1]+a[2*4+r]*b[c*4+2]+a[3*4+r]*b[c*4+3];
    memcpy(out,tmp,64);
}
void mat4_perspective(mat4 m, float fovy, float aspect, float n, float f) {
    memset(m,0,64); float t=1.0f/tanf(fovy/2);
    m[0]=t/aspect; m[5]=-t; m[10]=f/(n-f); m[11]=-1; m[14]=n*f/(n-f);
}
void mat4_lookat(mat4 m, float ex,float ey,float ez, float cx,float cy,float cz, float ux,float uy,float uz) {
    float fx=cx-ex,fy=cy-ey,fz=cz-ez,fl=sqrtf(fx*fx+fy*fy+fz*fz); fx/=fl;fy/=fl;fz/=fl;
    float sx=fy*uz-fz*uy,sy=fz*ux-fx*uz,sz=fx*uy-fy*ux,sl=sqrtf(sx*sx+sy*sy+sz*sz); sx/=sl;sy/=sl;sz/=sl;
    float uxn=sy*fz-sz*fy,uyn=sz*fx-sx*fz,uzn=sx*fy-sy*fx;
    mat4_identity(m); m[0]=sx;m[4]=sy;m[8]=sz; m[1]=uxn;m[5]=uyn;m[9]=uzn; m[2]=-fx;m[6]=-fy;m[10]=-fz;
    m[12]=-(sx*ex+sy*ey+sz*ez); m[13]=-(uxn*ex+uyn*ey+uzn*ez); m[14]=fx*ex+fy*ey+fz*ez;
}
void mat4_rotate_y(mat4 m, float a) { mat4_identity(m); m[0]=cosf(a);m[8]=sinf(a);m[2]=-sinf(a);m[10]=cosf(a); }
void mat4_rotate_x(mat4 m, float a) { mat4_identity(m); m[5]=cosf(a);m[9]=-sinf(a);m[6]=sinf(a);m[10]=cosf(a); }

static uint32_t *load_spv(const char *p, size_t *sz) {
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); *sz=ftell(f); fseek(f,0,SEEK_SET);
    uint32_t *d=malloc(*sz); fread(d,1,*sz,f); fclose(f); return d;
}
static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *p, uint32_t bits, VkMemoryPropertyFlags flags) {
    for(uint32_t i=0; i<p->memoryTypeCount; i++)
        if((bits&(1<<i)) && (p->memoryTypes[i].propertyFlags&flags)==flags) return i;
    return UINT32_MAX;
}
#define VK_CHECK(x) do{VkResult r=(x);if(r){printf("VK err %d (%s) @ line %d\n",r,#x,__LINE__);exit(1);}}while(0)

// Vulkan globals
static VkInstance instance;
static VkPhysicalDevice pdev;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage swapImages[8];
static VkImageView swapViews[8];
static uint32_t swapCount;
static VkRenderPass renderPass;
static VkFramebuffer framebuffers[8];
static VkPipelineLayout pipeLayout;
static VkPipeline pipeline;
static VkBuffer vertBuf, uboBuf;
static VkDeviceMemory vertMem, uboMem;
static VkDescriptorSetLayout descLayout;
static VkDescriptorPool descPool;
static VkDescriptorSet descSets[MAX_FRAMES];
static VkCommandPool cmdPool;
static VkCommandBuffer cmdBufs[MAX_FRAMES];
static VkSemaphore imageAvail[MAX_FRAMES], renderDone[MAX_FRAMES];
static VkFence inFlight[MAX_FRAMES];
static uint32_t curFrame = 0;
static void *uboMap;

// FPS and frame time tracking
static int frameCount = 0;
static double lastFPSTime = 0;
static double lastFrameTime = 0;
static double frameTimeSum = 0;
static double minFrameTime = 999999;
static double maxFrameTime = 0;

static double getTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void initVulkan(CAMetalLayer *metalLayer) {
    // Instance - MoltenVK requires portability enumeration
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.apiVersion = VK_API_VERSION_1_1;  // Use 1.1 for better compatibility
    const char *exts[] = {
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
        VK_KHR_SURFACE_EXTENSION_NAME,
        "VK_KHR_portability_enumeration"
    };
    VkInstanceCreateInfo instInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = 3;
    instInfo.ppEnabledExtensionNames = exts;
    instInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    VK_CHECK(vkCreateInstance(&instInfo, NULL, &instance));

    // Physical device
    uint32_t pdevCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdevCount, NULL);
    VkPhysicalDevice pdevs[8];
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &pdevCount, pdevs));
    pdev = pdevs[0];

    // Surface from Metal layer
    VkMetalSurfaceCreateInfoEXT surfInfo = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    surfInfo.pLayer = metalLayer;
    VK_CHECK(vkCreateMetalSurfaceEXT(instance, &surfInfo, NULL, &surface));

    // Queue family
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qfCount, NULL);
    VkQueueFamilyProperties qfProps[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qfCount, qfProps);
    uint32_t qfIdx = 0;
    for(uint32_t i=0; i<qfCount; i++) {
        VkBool32 surf = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(pdev, i, surface, &surf);
        if(qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && surf) { qfIdx = i; break; }
    }

    // Device - MoltenVK is a portability subset driver
    float qPrio = 1.0f;
    VkDeviceQueueCreateInfo qInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qInfo.queueFamilyIndex = qfIdx;
    qInfo.queueCount = 1;
    qInfo.pQueuePriorities = &qPrio;
    const char *devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
    VkDeviceCreateInfo devInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &qInfo;
    devInfo.enabledExtensionCount = 2;
    devInfo.ppEnabledExtensionNames = devExts;
    VK_CHECK(vkCreateDevice(pdev, &devInfo, NULL, &device));
    vkGetDeviceQueue(device, qfIdx, 0, &queue);

    // Swapchain
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps);

    // Query available present modes
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface, &pmCount, NULL);
    VkPresentModeKHR pmodes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface, &pmCount, pmodes);
    fprintf(stderr, "Available present modes: ");
    for(uint32_t i=0; i<pmCount; i++) fprintf(stderr, "%d ", pmodes[i]);
    fprintf(stderr, "\n");
    fflush(stderr);

    VkSwapchainCreateInfoKHR swInfo = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swInfo.surface = surface;
    swInfo.minImageCount = caps.minImageCount < 3 ? caps.minImageCount + 1 : 3;
    swInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swInfo.imageExtent = (VkExtent2D){WIDTH, HEIGHT};
    swInfo.imageArrayLayers = 1;
    swInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swInfo.preTransform = caps.currentTransform;
    swInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swInfo.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;  // Uncapped FPS, no VSync
    fprintf(stderr, "Using present mode: %d (IMMEDIATE)\n", swInfo.presentMode);
    fflush(stderr);
    VK_CHECK(vkCreateSwapchainKHR(device, &swInfo, NULL, &swapchain));

    swapCount = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &swapCount, swapImages);
    for(uint32_t i=0; i<swapCount; i++) {
        VkImageViewCreateInfo ivInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivInfo.image = swapImages[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        ivInfo.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1};
        VK_CHECK(vkCreateImageView(device, &ivInfo, NULL, &swapViews[i]));
    }

    // RenderPass
    VkAttachmentDescription att = {0};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &att;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &sub;
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, NULL, &renderPass));

    // Framebuffers
    for(uint32_t i=0; i<swapCount; i++) {
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &swapViews[i];
        fbInfo.width = WIDTH;
        fbInfo.height = HEIGHT;
        fbInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fbInfo, NULL, &framebuffers[i]));
    }

    // Vertex buffer
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(pdev, &memProps);

    VkBufferCreateInfo vbInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    vbInfo.size = sizeof(cube_verts);
    vbInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device, &vbInfo, NULL, &vertBuf));
    VkMemoryRequirements vbReq;
    vkGetBufferMemoryRequirements(device, vertBuf, &vbReq);
    VkMemoryAllocateInfo vbAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    vbAlloc.allocationSize = vbReq.size;
    vbAlloc.memoryTypeIndex = find_mem(&memProps, vbReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VK_CHECK(vkAllocateMemory(device, &vbAlloc, NULL, &vertMem));
    void *vbMap;
    vkMapMemory(device, vertMem, 0, sizeof(cube_verts), 0, &vbMap);
    memcpy(vbMap, cube_verts, sizeof(cube_verts));
    vkUnmapMemory(device, vertMem);
    vkBindBufferMemory(device, vertBuf, vertMem, 0);

    // UBO
    VkBufferCreateInfo uboInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    uboInfo.size = sizeof(mat4) * MAX_FRAMES;
    uboInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device, &uboInfo, NULL, &uboBuf));
    VkMemoryRequirements uboReq;
    vkGetBufferMemoryRequirements(device, uboBuf, &uboReq);
    VkMemoryAllocateInfo uboAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    uboAlloc.allocationSize = uboReq.size;
    uboAlloc.memoryTypeIndex = find_mem(&memProps, uboReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VK_CHECK(vkAllocateMemory(device, &uboAlloc, NULL, &uboMem));
    vkMapMemory(device, uboMem, 0, sizeof(mat4) * MAX_FRAMES, 0, &uboMap);
    vkBindBufferMemory(device, uboBuf, uboMem, 0);

    // Descriptor set layout
    VkDescriptorSetLayoutBinding bind = {0};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &bind;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dslInfo, NULL, &descLayout));

    // Pipeline layout
    VkPipelineLayoutCreateInfo plInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &plInfo, NULL, &pipeLayout));

    // Shaders
    size_t vertSize, fragSize;
    uint32_t *vertCode = load_spv("cube.vert.spv", &vertSize);
    uint32_t *fragCode = load_spv("cube.frag.spv", &fragSize);
    if(!vertCode || !fragCode) { printf("Failed to load shaders\n"); exit(1); }

    VkShaderModuleCreateInfo vertModInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertModInfo.codeSize = vertSize;
    vertModInfo.pCode = vertCode;
    VkShaderModule vertMod;
    VK_CHECK(vkCreateShaderModule(device, &vertModInfo, NULL, &vertMod));

    VkShaderModuleCreateInfo fragModInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragModInfo.codeSize = fragSize;
    fragModInfo.pCode = fragCode;
    VkShaderModule fragMod;
    VK_CHECK(vkCreateShaderModule(device, &fragModInfo, NULL, &fragMod));

    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkVertexInputBindingDescription viBind = {0, sizeof(float)*6, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription viAttr[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float)*3}
    };
    VkPipelineVertexInputStateCreateInfo viState = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    viState.vertexBindingDescriptionCount = 1;
    viState.pVertexBindingDescriptions = &viBind;
    viState.vertexAttributeDescriptionCount = 2;
    viState.pVertexAttributeDescriptions = viAttr;

    VkPipelineInputAssemblyStateCreateInfo iaState = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {0,0,WIDTH,HEIGHT,0,1};
    VkRect2D sc = {{0,0},{WIDTH,HEIGHT}};
    VkPipelineViewportStateCreateInfo vpState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpState.viewportCount = 1;
    vpState.pViewports = &vp;
    vpState.scissorCount = 1;
    vpState.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rsState = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsState.polygonMode = VK_POLYGON_MODE_FILL;
    rsState.cullMode = VK_CULL_MODE_BACK_BIT;
    rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msState = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cbAtt = {0};
    cbAtt.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbState = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbState.attachmentCount = 1;
    cbState.pAttachments = &cbAtt;

    VkGraphicsPipelineCreateInfo pipeInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &viState;
    pipeInfo.pInputAssemblyState = &iaState;
    pipeInfo.pViewportState = &vpState;
    pipeInfo.pRasterizationState = &rsState;
    pipeInfo.pMultisampleState = &msState;
    pipeInfo.pColorBlendState = &cbState;
    pipeInfo.layout = pipeLayout;
    pipeInfo.renderPass = renderPass;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, NULL, &pipeline));

    vkDestroyShaderModule(device, vertMod, NULL);
    vkDestroyShaderModule(device, fragMod, NULL);
    free(vertCode);
    free(fragCode);

    // Descriptor pool & sets
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES};
    VkDescriptorPoolCreateInfo dpInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpInfo.maxSets = MAX_FRAMES;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device, &dpInfo, NULL, &descPool));

    VkDescriptorSetLayout layouts[MAX_FRAMES];
    for(int i=0; i<MAX_FRAMES; i++) layouts[i] = descLayout;
    VkDescriptorSetAllocateInfo dsAlloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsAlloc.descriptorPool = descPool;
    dsAlloc.descriptorSetCount = MAX_FRAMES;
    dsAlloc.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsAlloc, descSets));

    for(int i=0; i<MAX_FRAMES; i++) {
        VkDescriptorBufferInfo bufInfo = {uboBuf, sizeof(mat4) * i, sizeof(mat4)};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descSets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    }

    // Command pool & buffers
    VkCommandPoolCreateInfo cpInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpInfo.queueFamilyIndex = qfIdx;
    VK_CHECK(vkCreateCommandPool(device, &cpInfo, NULL, &cmdPool));

    VkCommandBufferAllocateInfo cbAlloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAlloc.commandPool = cmdPool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = MAX_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbAlloc, cmdBufs));

    // Sync objects
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for(int i=0; i<MAX_FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(device, &semInfo, NULL, &imageAvail[i]));
        VK_CHECK(vkCreateSemaphore(device, &semInfo, NULL, &renderDone[i]));
        VK_CHECK(vkCreateFence(device, &fenceInfo, NULL, &inFlight[i]));
    }

    lastFPSTime = getTime();
    lastFrameTime = lastFPSTime;
    printf("Native MoltenVK initialized successfully!\n");
    fflush(stdout);
}

void renderFrame() {
    // FPS and frame time tracking
    double now = getTime();
    double frameTime = (lastFrameTime > 0) ? (now - lastFrameTime) : 0;
    lastFrameTime = now;

    if (frameTime > 0) {
        frameTimeSum += frameTime;
        if (frameTime < minFrameTime) minFrameTime = frameTime;
        if (frameTime > maxFrameTime) maxFrameTime = frameTime;
    }

    frameCount++;
    if(now - lastFPSTime >= 1.0) {
        double avgFrameTime = frameTimeSum / frameCount;
        double avgFrameTimeMs = avgFrameTime * 1000.0;
        double theoreticalMaxFPS = 1000.0 / avgFrameTimeMs;

        printf("FPS: %d (avg: %.1f) | Frame time: %.2fms (%.0f max FPS)\n",
               frameCount, 1.0 / avgFrameTime, avgFrameTimeMs, theoreticalMaxFPS);
        fflush(stdout);

        frameCount = 0;
        frameTimeSum = 0;
        minFrameTime = 999999;
        maxFrameTime = 0;
        lastFPSTime = now;
    }

    vkWaitForFences(device, 1, &inFlight[curFrame], VK_TRUE, UINT64_MAX);

    uint32_t imgIdx;
    vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvail[curFrame], VK_NULL_HANDLE, &imgIdx);

    vkResetFences(device, 1, &inFlight[curFrame]);

    // Update UBO
    float angle = now * 0.5f;
    mat4 model, view, proj, mvp;
    mat4_rotate_y(model, angle);
    mat4 rotX;
    mat4_rotate_x(rotX, angle * 0.7f);
    mat4_mul(model, model, rotX);
    mat4_lookat(view, 0,0,5, 0,0,0, 0,1,0);
    mat4_perspective(proj, 0.8f, (float)WIDTH/HEIGHT, 0.1f, 100.0f);
    mat4_mul(mvp, proj, view);
    mat4_mul(mvp, mvp, model);
    memcpy((char*)uboMap + sizeof(mat4) * curFrame, mvp, sizeof(mat4));

    // Record command buffer
    vkResetCommandBuffer(cmdBufs[curFrame], 0);
    VkCommandBufferBeginInfo cbBegin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmdBufs[curFrame], &cbBegin);

    VkClearValue clear = {{{0.1f, 0.1f, 0.15f, 1.0f}}};
    VkRenderPassBeginInfo rpBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = renderPass;
    rpBegin.framebuffer = framebuffers[imgIdx];
    rpBegin.renderArea = (VkRect2D){{0,0},{WIDTH,HEIGHT}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmdBufs[curFrame], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmdBufs[curFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmdBufs[curFrame], 0, 1, &vertBuf, &offset);
    vkCmdBindDescriptorSets(cmdBufs[curFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &descSets[curFrame], 0, NULL);
    vkCmdDraw(cmdBufs[curFrame], 36, 1, 0, 0);

    vkCmdEndRenderPass(cmdBufs[curFrame]);
    vkEndCommandBuffer(cmdBufs[curFrame]);

    // Submit
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvail[curFrame];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdBufs[curFrame];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone[curFrame];
    vkQueueSubmit(queue, 1, &submit, inFlight[curFrame]);

    // Present
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone[curFrame];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imgIdx;
    vkQueuePresentKHR(queue, &present);

    curFrame = (curFrame + 1) % MAX_FRAMES;
}

// NSView subclass for Metal layer
@interface VulkanView : NSView
@end

@implementation VulkanView
- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = NO;  // Disable VSync at Metal layer level
    return layer;
}
- (BOOL)wantsUpdateLayer { return YES; }
@end

// App delegate
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@property (strong) VulkanView *view;
@property (assign) BOOL running;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSRect frame = NSMakeRect(100, 100, WIDTH, HEIGHT);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable)
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"Native MoltenVK Cube"];

    self.view = [[VulkanView alloc] initWithFrame:frame];
    [self.view setWantsLayer:YES];
    [self.window setContentView:self.view];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    CAMetalLayer *metalLayer = (CAMetalLayer *)self.view.layer;
    initVulkan(metalLayer);

    // Run render loop on background thread for max performance
    self.running = YES;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
        while(self.running) {
            renderFrame();
        }
    });
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    self.running = NO;
    usleep(100000);  // Give render thread time to exit
    vkDeviceWaitIdle(device);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}
@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
