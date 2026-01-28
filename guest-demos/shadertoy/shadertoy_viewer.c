/* Shader test viewer based on vkcube_anim - proven working
 * Usage: ./shadertoy_viewer <vert.spv> <frag.spv> [duration_sec]
 * 
 * Based on vkcube_anim.c architecture which achieves 1100 FPS
 * Modified for fullscreen quad rendering with shadertoy-style uniforms
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

typedef struct {
    float iResolution[3];
    float iTime;
    float iMouse[4];
} ShaderToyUBO;

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

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <vert.spv> <frag.spv> [duration_sec]\n", argv[0]);
        return 1;
    }
    const char *vert_path = argv[1];
    const char *frag_path = argv[2];
    float duration = (argc > 3) ? atof(argv[3]) : 10.0f;
    
    // DRM/GBM Setup (same as vkcube_anim)
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    drmSetMaster(drm_fd);
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
    
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handles[4] = {gbm_bo_get_handle(bo).u32, 0, 0, 0};
    uint32_t strides[4] = {stride, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint32_t fb_id;
    drmModeAddFB2(drm_fd, W, H, GBM_FORMAT_XRGB8888, handles, strides, offsets, &fb_id, 0);
    
    // Vulkan Setup (same pattern as vkcube_anim)
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
        .sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}, NULL, &instance));
    
    uint32_t gpuCount = 1; VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
    printf("Shader Viewer on %s (%ux%u)\n", props.deviceName, W, H);
    
    VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &(VkDeviceCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,
        .pQueueCreateInfos=&(VkDeviceQueueCreateInfo){
            .sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&(float){1.0f}
        }
    }, NULL, &device));
    
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    
    // Render target image (LINEAR + HOST_VISIBLE like vkcube_anim)
    VkImage rtImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .extent={W,H,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    }, NULL, &rtImg));
    VkMemoryRequirements rtReq;
    vkGetImageMemoryRequirements(device, rtImg, &rtReq);
    VkDeviceMemory rtMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=rtReq.size,
        .memoryTypeIndex=find_mem(&memProps, rtReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &rtMem));
    VK_CHECK(vkBindImageMemory(device, rtImg, rtMem, 0));
    void *rtPtr; VK_CHECK(vkMapMemory(device, rtMem, 0, VK_WHOLE_SIZE, 0, &rtPtr));
    
    VkImageView rtView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=rtImg,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}
    }, NULL, &rtView));
    
    // Render pass
    VkRenderPass renderPass;
    VK_CHECK(vkCreateRenderPass(device, &(VkRenderPassCreateInfo){
        .sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,
        .pAttachments=&(VkAttachmentDescription){
            .format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout=VK_IMAGE_LAYOUT_GENERAL
        },
        .subpassCount=1,
        .pSubpasses=&(VkSubpassDescription){
            .pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount=1,
            .pColorAttachments=&(VkAttachmentReference){0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
        }
    }, NULL, &renderPass));
    
    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(device, &(VkFramebufferCreateInfo){
        .sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=renderPass,.attachmentCount=1,.pAttachments=&rtView,
        .width=W,.height=H,.layers=1
    }, NULL, &framebuffer));
    
    // Load shaders
    size_t vsz, fsz;
    uint32_t *vc = load_spv(vert_path, &vsz);
    uint32_t *fc = load_spv(frag_path, &fsz);
    if (!vc || !fc) { printf("Failed to load shaders\n"); return 1; }
    
    VkShaderModule vm, fm;
    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vsz,.pCode=vc}, NULL, &vm));
    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fsz,.pCode=fc}, NULL, &fm));
    
    // Uniform buffer for shadertoy
    VkBuffer uboBuf;
    VK_CHECK(vkCreateBuffer(device, &(VkBufferCreateInfo){
        .sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=64,.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
    }, NULL, &uboBuf));
    VkMemoryRequirements uboReq;
    vkGetBufferMemoryRequirements(device, uboBuf, &uboReq);
    VkDeviceMemory uboMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=uboReq.size,
        .memoryTypeIndex=find_mem(&memProps, uboReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &uboMem));
    VK_CHECK(vkBindBufferMemory(device, uboBuf, uboMem, 0));
    void *uboPtr; vkMapMemory(device, uboMem, 0, 64, 0, &uboPtr);
    
    // Descriptor setup - KEY: Include in pipeline layout!
    VkDescriptorSetLayout descLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &(VkDescriptorSetLayoutCreateInfo){
        .sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,
        .pBindings=&(VkDescriptorSetLayoutBinding){
            0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT
        }
    }, NULL, &descLayout));
    
    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &(VkPipelineLayoutCreateInfo){
        .sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&descLayout  // CRITICAL!
    }, NULL, &pipelineLayout));
    
    // Graphics pipeline
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
        .sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,
        .pStages=(VkPipelineShaderStageCreateInfo[]){
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
            {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}
        },
        .pVertexInputState=&(VkPipelineVertexInputStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        },
        .pInputAssemblyState=&(VkPipelineInputAssemblyStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        },
        .pViewportState=&(VkPipelineViewportStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount=1,.pViewports=&(VkViewport){0,0,W,H,0,1},
            .scissorCount=1,.pScissors=&(VkRect2D){{0,0},{W,H}}
        },
        .pRasterizationState=&(VkPipelineRasterizationStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1.0f
        },
        .pMultisampleState=&(VkPipelineMultisampleStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples=VK_SAMPLE_COUNT_1_BIT
        },
        .pColorBlendState=&(VkPipelineColorBlendStateCreateInfo){
            .sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount=1,
            .pAttachments=&(VkPipelineColorBlendAttachmentState){.colorWriteMask=0xF}
        },
        .layout=pipelineLayout,.renderPass=renderPass
    }, NULL, &pipeline));
    
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
        .sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}, NULL, &cmdPool));
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
        .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=cmdPool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount=1
    }, &cmd));
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &(VkFenceCreateInfo){
        .sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));
    
    // Get image layout
    VkSubresourceLayout rtLayout;
    vkGetImageSubresourceLayout(device, rtImg, &(VkImageSubresource){
        VK_IMAGE_ASPECT_COLOR_BIT,0,0}, &rtLayout);
    
    drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode);
    
    // Render Loop (like vkcube_anim)
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int frames = 0;
    
    printf("Rendering for %.1fs...\n", duration);
    
    while(1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        float t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9f;
        if (t >= duration) break;
        
        // Update UBO
        ShaderToyUBO ubo = {
            .iResolution = {W, H, 1.0f},
            .iTime = t,
            .iMouse = {0, 0, 0, 0}
        };
        memcpy(uboPtr, &ubo, sizeof(ubo));
        
        // Record commands (same pattern as vkcube_anim)
        vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
            .sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT});
        vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
            .sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=renderPass,.framebuffer=framebuffer,
            .renderArea={{0,0},{W,H}},.clearValueCount=1,
            .pClearValues=&(VkClearValue){.color={.float32={0,0,0,1}}}
        }, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &descSet, 0, NULL);  // CRITICAL!
        vkCmdDraw(cmd, 6, 1, 0, 0);  // 6 vertices = fullscreen quad
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        
        // Submit and wait
        VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
            .sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount=1,.pCommandBuffers=&cmd
        }, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);
        
        // Copy to GBM (like vkcube_anim)
        void *gbmPtr = NULL; uint32_t gbmStride; void *mapData = NULL;
        gbmPtr = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &gbmStride, &mapData);
        if (gbmPtr) {
            for (uint32_t y = 0; y < H; y++)
                memcpy((char*)gbmPtr + y * gbmStride,
                       (char*)rtPtr + y * rtLayout.rowPitch, W * 4);
            gbm_bo_unmap(bo, mapData);
        }
        drmModeDirtyFB(drm_fd, fb_id, NULL, 0);
        
        frames++;
        if (frames % 60 == 0) printf("%.1fs: %d frames\n", t, frames);
    }
    
    printf("Done! %d frames in %.1fs (%.1f FPS)\n",
           frames, duration, frames / duration);
    return 0;
}
