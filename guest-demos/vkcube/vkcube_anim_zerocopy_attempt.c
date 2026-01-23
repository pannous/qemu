/* Zero-copy animated Vulkan cube demo
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
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

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
#define VK_CHECK(x) do{VkResult r=(x);if(r){printf("VK err %d @ %d\n",r,__LINE__);exit(1);}}while(0)

int main(void) {
    // === DRM/GBM Setup ===
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    drmModeRes *res = drmModeGetResources(drm_fd);
    drmModeConnector *conn = NULL;
    for(int i=0; i<res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if(conn && conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(conn); conn = NULL;
    }
    drmModeModeInfo *mode = &conn->modes[0];
    uint32_t W = mode->hdisplay, H = mode->vdisplay;

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];

    // Create GBM scanout buffer
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint32_t stride = gbm_bo_get_stride(bo);
    int prime_fd = gbm_bo_get_fd(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    uint32_t fb_id;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { stride };
    uint32_t offsets[4] = { 0 };
    drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_XRGB8888, handles, strides, offsets, &fb_id, 0);

    // === Vulkan with External Memory ===
    const char *inst_exts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME };
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = inst_exts
    }, NULL, &instance));

    uint32_t gpuCount=1; VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
    printf("Rainbow Cube on %s (%ux%u)\n", props.deviceName, W, H);
    VkPhysicalDeviceMemoryProperties memProps; vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    const char *dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
    };
    float qp=1.0f; VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &(VkDeviceCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,
        .pQueueCreateInfos=&(VkDeviceQueueCreateInfo){
            .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&qp},
        .enabledExtensionCount=4,
        .ppEnabledExtensionNames=dev_exts
    }, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);

    // === Import GBM as render target (ZERO-COPY) ===
    VkSubresourceLayout plane_layout = { .offset=0, .rowPitch=stride };
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout
    };
    VkExternalMemoryImageCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };

    VkImage rtImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext=&ext_info,
        .imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,
        .extent={W,H,1},
        .mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    }, NULL, &rtImg));

    VkMemoryRequirements rtReq; vkGetImageMemoryRequirements(device, rtImg, &rtReq);
    VkDeviceMemory rtMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext=&(VkImportMemoryFdInfoKHR){
            .sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd=prime_fd
        },
        .allocationSize=rtReq.size,
        .memoryTypeIndex=find_mem(&memProps,rtReq.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    }, NULL, &rtMem));
    VK_CHECK(vkBindImageMemory(device, rtImg, rtMem, 0));

    VkImageView rtView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=rtImg,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    }, NULL, &rtView));

    // Depth buffer (device local, no import needed)
    VkImage depthImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT,.extent={W,H,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
    }, NULL, &depthImg));
    VkMemoryRequirements depthReq; vkGetImageMemoryRequirements(device, depthImg, &depthReq);
    VkDeviceMemory depthMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=depthReq.size,
        .memoryTypeIndex=find_mem(&memProps,depthReq.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    }, NULL, &depthMem));
    VK_CHECK(vkBindImageMemory(device, depthImg, depthMem, 0));
    VkImageView depthView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=depthImg,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_D32_SFLOAT,
        .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}
    }, NULL, &depthView));

    // Render pass
    VkRenderPass renderPass;
    VK_CHECK(vkCreateRenderPass(device, &(VkRenderPassCreateInfo){
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2,
        .pAttachments=(VkAttachmentDescription[]){
            {.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
             .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL},
            {.format=VK_FORMAT_D32_SFLOAT,.samples=VK_SAMPLE_COUNT_1_BIT,
             .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
             .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}},
        .subpassCount=1,
        .pSubpasses=&(VkSubpassDescription){
            .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount=1,
            .pColorAttachments=&(VkAttachmentReference){0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            .pDepthStencilAttachment=&(VkAttachmentReference){1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}}
    }, NULL, &renderPass));

    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(device, &(VkFramebufferCreateInfo){
        .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=renderPass,.attachmentCount=2,
        .pAttachments=(VkImageView[]){rtView,depthView},
        .width=W,.height=H,.layers=1
    }, NULL, &framebuffer));

    // Shaders
    size_t vsz, fsz;
    uint32_t *vc = load_spv("/root/cube.vert.spv", &vsz);
    uint32_t *fc = load_spv("/root/cube.frag.spv", &fsz);
    VkShaderModule vm, fm;
    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vsz,.pCode=vc}, NULL, &vm));
    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fsz,.pCode=fc}, NULL, &fm));

    // Descriptor set for UBO
    VkDescriptorSetLayout descLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &(VkDescriptorSetLayoutCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,
        .pBindings=&(VkDescriptorSetLayoutBinding){0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT}
    }, NULL, &descLayout));
    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &(VkPipelineLayoutCreateInfo){
        .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&descLayout
    }, NULL, &pipelineLayout));

    // Pipeline
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,
        .pStages=(VkPipelineShaderStageCreateInfo[]){
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}},
        .pVertexInputState=&(VkPipelineVertexInputStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount=1,
            .pVertexBindingDescriptions=&(VkVertexInputBindingDescription){0,24,VK_VERTEX_INPUT_RATE_VERTEX},
            .vertexAttributeDescriptionCount=2,
            .pVertexAttributeDescriptions=(VkVertexInputAttributeDescription[]){
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32_SFLOAT,12}}},
        .pInputAssemblyState=&(VkPipelineInputAssemblyStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
        .pViewportState=&(VkPipelineViewportStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&(VkViewport){0,0,W,H,0,1},
            .scissorCount=1,.pScissors=&(VkRect2D){{0,0},{W,H}}},
        .pRasterizationState=&(VkPipelineRasterizationStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_BACK_BIT,
            .frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1},
        .pMultisampleState=&(VkPipelineMultisampleStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT},
        .pDepthStencilState=&(VkPipelineDepthStencilStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS},
        .pColorBlendState=&(VkPipelineColorBlendStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,.pAttachments=&(VkPipelineColorBlendAttachmentState){.colorWriteMask=0xF}},
        .layout=pipelineLayout,.renderPass=renderPass
    }, NULL, &pipeline));

    // Vertex buffer
    VkBuffer vertBuf;
    VK_CHECK(vkCreateBuffer(device, &(VkBufferCreateInfo){
        .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=sizeof(cube_verts),.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
    }, NULL, &vertBuf));
    VkMemoryRequirements vbReq; vkGetBufferMemoryRequirements(device, vertBuf, &vbReq);
    VkDeviceMemory vbMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=vbReq.size,
        .memoryTypeIndex=find_mem(&memProps,vbReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &vbMem));
    VK_CHECK(vkBindBufferMemory(device, vertBuf, vbMem, 0));
    void *vbPtr; vkMapMemory(device, vbMem, 0, sizeof(cube_verts), 0, &vbPtr);
    memcpy(vbPtr, cube_verts, sizeof(cube_verts)); vkUnmapMemory(device, vbMem);

    // Uniform buffer
    VkBuffer uboBuf;
    VK_CHECK(vkCreateBuffer(device, &(VkBufferCreateInfo){
        .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=64,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
    }, NULL, &uboBuf));
    VkMemoryRequirements uboReq; vkGetBufferMemoryRequirements(device, uboBuf, &uboReq);
    VkDeviceMemory uboMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=uboReq.size,
        .memoryTypeIndex=find_mem(&memProps,uboReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &uboMem));
    VK_CHECK(vkBindBufferMemory(device, uboBuf, uboMem, 0));
    void *uboPtr; vkMapMemory(device, uboMem, 0, 64, 0, &uboPtr);

    // Descriptor pool and set
    VkDescriptorPool descPool;
    VK_CHECK(vkCreateDescriptorPool(device, &(VkDescriptorPoolCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=1,
        .pPoolSizes=&(VkDescriptorPoolSize){VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}
    }, NULL, &descPool));
    VkDescriptorSet descSet;
    VK_CHECK(vkAllocateDescriptorSets(device, &(VkDescriptorSetAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descPool,.descriptorSetCount=1,.pSetLayouts=&descLayout
    }, &descSet));
    vkUpdateDescriptorSets(device, 1, &(VkWriteDescriptorSet){
        .sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=descSet,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo=&(VkDescriptorBufferInfo){uboBuf,0,64}
    }, 0, NULL);

    // Command pool/buffer
    VkCommandPool cmdPool;
    VK_CHECK(vkCreateCommandPool(device, &(VkCommandPoolCreateInfo){
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    }, NULL, &cmdPool));
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=cmdPool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1
    }, &cmd));
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &(VkFenceCreateInfo){.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));

    // Matrices
    mat4 proj, view;
    mat4_perspective(proj, 3.14159f/4.0f, (float)W/(float)H, 0.1f, 100.0f);
    mat4_lookat(view, 0, 2, 5, 0, 0, 0, 0, 1, 0);

    printf("Spinning for 10s (zero-copy)...\n");
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    int frames = 0;

    while(1) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        float t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9f;
        if (t > 10.0f) break;

        // Update MVP
        mat4 rotY, rotX, model, mv, mvp;
        mat4_rotate_y(rotY, t * 1.0f);
        mat4_rotate_x(rotX, t * 0.5f);
        mat4_mul(model, rotY, rotX);
        mat4_mul(mv, view, model);
        mat4_mul(mvp, proj, mv);
        memcpy(uboPtr, mvp, 64);

        // Record
        vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
            .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT});
        vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
            .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=renderPass,.framebuffer=framebuffer,
            .renderArea={{0,0},{W,H}},.clearValueCount=2,
            .pClearValues=(VkClearValue[]){{.color={{0.02f,0.02f,0.05f,1}}},{.depthStencil={1,0}}}
        }, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descSet, 0, NULL);
        VkDeviceSize off = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vertBuf, &off);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        // Submit
        VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
            .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&cmd
        }, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        // Scanout - NO COPY! GBM buffer IS the render target
        drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode);
        frames++;
    }

    printf("Done! %d frames (%.1f fps) - zero-copy!\n", frames, frames/10.0f);

    vkUnmapMemory(device, uboMem);
    vkDeviceWaitIdle(device);

    // Cleanup
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmdPool, NULL);
    vkDestroyDescriptorPool(device, descPool, NULL);
    vkDestroyBuffer(device, uboBuf, NULL);
    vkFreeMemory(device, uboMem, NULL);
    vkDestroyBuffer(device, vertBuf, NULL);
    vkFreeMemory(device, vbMem, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyDescriptorSetLayout(device, descLayout, NULL);
    vkDestroyShaderModule(device, vm, NULL);
    vkDestroyShaderModule(device, fm, NULL);
    vkDestroyFramebuffer(device, framebuffer, NULL);
    vkDestroyRenderPass(device, renderPass, NULL);
    vkDestroyImageView(device, depthView, NULL);
    vkDestroyImage(device, depthImg, NULL);
    vkFreeMemory(device, depthMem, NULL);
    vkDestroyImageView(device, rtView, NULL);
    vkDestroyImage(device, rtImg, NULL);
    vkFreeMemory(device, rtMem, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);

    drmModeRmFB(drm_fd, fb_id);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(drm_fd);

    free(vc); free(fc);
    return 0;
}
