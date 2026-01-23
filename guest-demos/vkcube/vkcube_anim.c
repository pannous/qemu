/* Animated Vulkan cube demo - host swapchain present path
 *
 * Architecture:
 *   VkImage (LINEAR, HOST_VISIBLE) ← render on host GPU
 *        ↓
 *   QEMU presents hostptr via Vulkan swapchain (no guest CPU copy)
 *        ↓
 *   DRM scanout used only to trigger scanout updates
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
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

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        return 0;
    }
    uint32_t prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop && strcmp(prop->name, name) == 0) {
            prop_id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return prop_id;
}

static uint32_t find_primary_plane(int fd, drmModeRes *res, uint32_t crtc_id) {
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        return 0;
    }

    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }
    if (crtc_index < 0) {
        drmModeFreePlaneResources(plane_res);
        return 0;
    }
    uint32_t crtc_bit = 1u << crtc_index;

    uint32_t best_plane = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint32_t plane_id = plane_res->planes[i];
        drmModePlane *plane = drmModeGetPlane(fd, plane_id);
        if (!plane) {
            continue;
        }
        if (!(plane->possible_crtcs & crtc_bit)) {
            drmModeFreePlane(plane);
            continue;
        }

        uint32_t type_prop = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type");
        if (type_prop) {
            drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane_id,
                                                                        DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t p = 0; p < props->count_props; p++) {
                    if (props->props[p] == type_prop) {
                        if (props->prop_values[p] == 1) {
                            best_plane = plane_id;
                        }
                        break;
                    }
                }
                drmModeFreeObjectProperties(props);
            }
        }

        if (!best_plane) {
            best_plane = plane_id;
        }
        drmModeFreePlane(plane);
        if (best_plane) {
            break;
        }
    }

    drmModeFreePlaneResources(plane_res);
    return best_plane;
}

int main(void) {
    // === DRM/GBM Setup ===
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    drmSetMaster(drm_fd);
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0) {
        printf("Enabled DRM universal planes\n");
    }
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
        printf("Enabled DRM atomic\n");
    }
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

    drmModeCrtc *orig_crtc = drmModeGetCrtc(drm_fd, crtc_id);

    // Create GBM scanout buffer (XRGB8888 - no alpha!)
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint32_t stride = gbm_bo_get_stride(bo);

    uint32_t fb_id = 0;
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { stride };
    uint32_t offsets[4] = { 0 };
    uint64_t modifiers[4] = { gbm_bo_get_modifier(bo), 0, 0, 0 };
    if (drmModeAddFB2WithModifiers(drm_fd, W, H, GBM_FORMAT_XRGB8888,
                                   handles, strides, offsets, modifiers,
                                   &fb_id, DRM_MODE_FB_MODIFIERS) == 0) {
        printf("Created FB with modifiers (mod=0x%llx)\n",
               (unsigned long long)modifiers[0]);
    } else {
        if (drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_XRGB8888,
                          handles, strides, offsets, &fb_id, 0) == 0) {
            printf("Created FB without modifiers\n");
        } else {
            printf("Failed to create DRM framebuffer\n");
            return 1;
        }
    }

    // === Vulkan Setup (No External Memory!) ===
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    }, NULL, &instance));

    uint32_t gpuCount=1; VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
    printf("Rainbow Cube on %s (%ux%u)\n", props.deviceName, W, H);
    VkPhysicalDeviceMemoryProperties memProps; vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    float qp=1.0f; VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &(VkDeviceCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,
        .pQueueCreateInfos=&(VkDeviceQueueCreateInfo){
            .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&qp}
    }, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);

    // === Render target: LINEAR + HOST_VISIBLE (like test_tri) ===
    VkImage rtImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,
        .extent={W,H,1},
        .mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_LINEAR,  // LINEAR for CPU access
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    }, NULL, &rtImg));

    VkMemoryRequirements rtReq; vkGetImageMemoryRequirements(device, rtImg, &rtReq);
    VkDeviceMemory rtMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=rtReq.size,
        .memoryTypeIndex=find_mem(&memProps,rtReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &rtMem));
    VK_CHECK(vkBindImageMemory(device, rtImg, rtMem, 0));
    void *rtPtr = NULL;
    VK_CHECK(vkMapMemory(device, rtMem, 0, VK_WHOLE_SIZE, 0, &rtPtr));

    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, rtImg,
                                &(VkImageSubresource){VK_IMAGE_ASPECT_COLOR_BIT,0,0},
                                &layout);
    printf("Render image rowPitch=%llu size=%llu\n",
           (unsigned long long)layout.rowPitch,
           (unsigned long long)rtReq.size);

    VkImageView rtView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=rtImg,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    }, NULL, &rtView));

    // Depth buffer (OPTIMAL tiling is fine, we don't need to read it)
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

    uint32_t plane_id = find_primary_plane(drm_fd, res, crtc_id);
    uint32_t mode_blob_id = 0;
    bool atomic_ready = false;
    uint32_t conn_crtc = 0, crtc_mode = 0, crtc_active = 0;
    uint32_t plane_fb = 0, plane_crtc = 0, plane_src_x = 0, plane_src_y = 0;
    uint32_t plane_src_w = 0, plane_src_h = 0, plane_crtc_x = 0, plane_crtc_y = 0;
    uint32_t plane_crtc_w = 0, plane_crtc_h = 0;

    if (plane_id) {
        conn_crtc = get_prop_id(drm_fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        crtc_mode = get_prop_id(drm_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        crtc_active = get_prop_id(drm_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        plane_fb = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        plane_crtc = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        plane_src_x = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
        plane_src_y = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        plane_src_w = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
        plane_src_h = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
        plane_crtc_x = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        plane_crtc_y = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        plane_crtc_w = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        plane_crtc_h = get_prop_id(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

        if (conn_crtc && crtc_mode && crtc_active && plane_fb && plane_crtc &&
            plane_src_x && plane_src_y && plane_src_w && plane_src_h &&
            plane_crtc_x && plane_crtc_y && plane_crtc_w && plane_crtc_h) {
            if (drmModeCreatePropertyBlob(drm_fd, mode, sizeof(*mode), &mode_blob_id) == 0) {
                atomic_ready = true;
            }
        }
    }

    /* One-time modeset/scanout; host-present handles per-frame updates. */
    if (atomic_ready) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            drmModeAtomicAddProperty(req, conn->connector_id, conn_crtc, crtc_id);
            drmModeAtomicAddProperty(req, crtc_id, crtc_mode, mode_blob_id);
            drmModeAtomicAddProperty(req, crtc_id, crtc_active, 1);
            drmModeAtomicAddProperty(req, plane_id, plane_fb, fb_id);
            drmModeAtomicAddProperty(req, plane_id, plane_crtc, crtc_id);
            drmModeAtomicAddProperty(req, plane_id, plane_crtc_x, 0);
            drmModeAtomicAddProperty(req, plane_id, plane_crtc_y, 0);
            drmModeAtomicAddProperty(req, plane_id, plane_crtc_w, W);
            drmModeAtomicAddProperty(req, plane_id, plane_crtc_h, H);
            drmModeAtomicAddProperty(req, plane_id, plane_src_x, 0);
            drmModeAtomicAddProperty(req, plane_id, plane_src_y, 0);
            drmModeAtomicAddProperty(req, plane_id, plane_src_w, W << 16);
            drmModeAtomicAddProperty(req, plane_id, plane_src_h, H << 16);

            if (drmModeAtomicCommit(drm_fd, req,
                                    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) != 0) {
                atomic_ready = false;
            }
            drmModeAtomicFree(req);
        }
    }
    if (!atomic_ready) {
        drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode);
    }

    printf("Spinning for 10s (HOST_VISIBLE, no guest copy)...\n");
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

        // Submit and wait
        VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
            .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&cmd
        }, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        /* Ensure GPU writes are visible to host-visible memory before host reads. */
        VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = rtMem,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        vkInvalidateMappedMemoryRanges(device, 1, &range);

        frames++;
    }

    printf("Done! %d frames (%.1f fps) - HOST_VISIBLE, no guest copy\n", frames, frames/10.0f);

    vkUnmapMemory(device, uboMem);
    vkDeviceWaitIdle(device);

    if (orig_crtc && orig_crtc->buffer_id) {
        drmModeSetCrtc(drm_fd, orig_crtc->crtc_id, orig_crtc->buffer_id,
                       orig_crtc->x, orig_crtc->y, &conn->connector_id, 1,
                       &orig_crtc->mode);
    }
    drmDropMaster(drm_fd);
    if (mode_blob_id) {
        drmModeDestroyPropertyBlob(drm_fd, mode_blob_id);
    }

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

    if (orig_crtc) {
        drmModeFreeCrtc(orig_crtc);
    }
    drmModeRmFB(drm_fd, fb_id);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(drm_fd);

    free(vc); free(fc);
    return 0;
}
