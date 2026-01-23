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

#define VK_CHECK(x) do{VkResult r=(x);if(r){printf("VK err %d @ line %d\n",r,__LINE__);exit(1);}}while(0)

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *p,uint32_t b,VkMemoryPropertyFlags f){
    for(uint32_t i=0;i<p->memoryTypeCount;i++) if((b&(1<<i))&&(p->memoryTypes[i].propertyFlags&f)==f) return i;
    return UINT32_MAX;
}

static uint32_t *load_spv(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *data = malloc(*size); fread(data, 1, *size, f); fclose(f);
    return data;
}

int main(void) {
    int drm_fd=open("/dev/dri/card0",O_RDWR);
    drmModeRes *res=drmModeGetResources(drm_fd);
    drmModeConnector *conn=NULL;
    for(int i=0;i<res->count_connectors;i++){conn=drmModeGetConnector(drm_fd,res->connectors[i]);if(conn&&conn->connection==DRM_MODE_CONNECTED)break;drmModeFreeConnector(conn);conn=NULL;}
    drmModeModeInfo *mode=&conn->modes[0]; uint32_t W=mode->hdisplay,H=mode->vdisplay;
    printf("Display: %ux%u\n",W,H);
    drmModeEncoder *enc=drmModeGetEncoder(drm_fd,conn->encoder_id);
    uint32_t crtc_id=enc?enc->crtc_id:res->crtcs[0];
    struct gbm_device *gbm=gbm_create_device(drm_fd);
    struct gbm_bo *bo=gbm_bo_create(gbm,W,H,GBM_FORMAT_XRGB8888,GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
    uint32_t stride=gbm_bo_get_stride(bo);
    uint32_t fb_id,hs[4]={gbm_bo_get_handle(bo).u32},st[4]={stride},of[4]={0};
    drmModeAddFB2(drm_fd,W,H,GBM_FORMAT_XRGB8888,hs,st,of,&fb_id,0);
    
    VkInstance instance;
    VkInstanceCreateInfo instInfo={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VK_CHECK(vkCreateInstance(&instInfo,NULL,&instance));
    uint32_t gpuCount=1; VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance,&gpuCount,&gpu);
    printf("GPU: "); VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu,&props); printf("%s\n",props.deviceName);
    VkPhysicalDeviceMemoryProperties memProps; vkGetPhysicalDeviceMemoryProperties(gpu,&memProps);
    
    float qp=1.0f;
    VkDeviceQueueCreateInfo qInfo={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&qp};
    VkDeviceCreateInfo devInfo={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qInfo};
    VkDevice device; VK_CHECK(vkCreateDevice(gpu,&devInfo,NULL,&device));
    VkQueue queue; vkGetDeviceQueue(device,0,0,&queue);
    
    // Render target - LINEAR for CPU access
    VkImage rtImg;
    VkImageCreateInfo imgInfo={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_B8G8R8A8_UNORM,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_LINEAR,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VK_CHECK(vkCreateImage(device,&imgInfo,NULL,&rtImg));
    VkMemoryRequirements rtReq; vkGetImageMemoryRequirements(device,rtImg,&rtReq);
    VkDeviceMemory rtMem;
    VkMemoryAllocateInfo rtAlloc={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=rtReq.size,
        .memoryTypeIndex=find_mem(&memProps,rtReq.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(device,&rtAlloc,NULL,&rtMem));
    VK_CHECK(vkBindImageMemory(device,rtImg,rtMem,0));
    VkImageView rtView;
    VkImageViewCreateInfo viewInfo={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=rtImg,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VK_CHECK(vkCreateImageView(device,&viewInfo,NULL,&rtView));
    
    // Render pass
    VkAttachmentDescription att={.format=VK_FORMAT_B8G8R8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference colorRef={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&colorRef};
    VkRenderPassCreateInfo rpInfo={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&att,.subpassCount=1,.pSubpasses=&subpass};
    VkRenderPass renderPass; VK_CHECK(vkCreateRenderPass(device,&rpInfo,NULL,&renderPass));
    
    VkFramebufferCreateInfo fbInfo={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=renderPass,.attachmentCount=1,.pAttachments=&rtView,.width=W,.height=H,.layers=1};
    VkFramebuffer framebuffer; VK_CHECK(vkCreateFramebuffer(device,&fbInfo,NULL,&framebuffer));
    
    // Shaders
    size_t vs,fs; uint32_t *vc=load_spv("/root/tri.vert.spv",&vs), *fc=load_spv("/root/tri.frag.spv",&fs);
    VkShaderModule vm,fm;
    VK_CHECK(vkCreateShaderModule(device,&(VkShaderModuleCreateInfo){.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=vs,.pCode=vc},NULL,&vm));
    VK_CHECK(vkCreateShaderModule(device,&(VkShaderModuleCreateInfo){.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=fs,.pCode=fc},NULL,&fm));
    
    // Pipeline (no vertex input, no descriptors)
    VkPipelineLayoutCreateInfo plInfo={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pipelineLayout; VK_CHECK(vkCreatePipelineLayout(device,&plInfo,NULL,&pipelineLayout));
    
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vm,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fm,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp={0,0,W,H,0,1}; VkRect2D sc={{0,0},{W,H}};
    VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba={.colorWriteMask=0xF};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
    VkGraphicsPipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vps,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pColorBlendState=&cb,.layout=pipelineLayout,.renderPass=renderPass};
    VkPipeline pipeline; VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    
    // Command buffer
    VkCommandPool cmdPool; VK_CHECK(vkCreateCommandPool(device,&(VkCommandPoolCreateInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO},NULL,&cmdPool));
    VkCommandBuffer cmd; VK_CHECK(vkAllocateCommandBuffers(device,&(VkCommandBufferAllocateInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cmdPool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1},&cmd));
    VkFence fence; VK_CHECK(vkCreateFence(device,&(VkFenceCreateInfo){.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO},NULL,&fence));
    
    // Record: clear to blue, draw triangle
    vkBeginCommandBuffer(cmd,&(VkCommandBufferBeginInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
    VkClearValue clear={.color={{0.0f,0.0f,0.3f,1.0f}}};
    vkCmdBeginRenderPass(cmd,&(VkRenderPassBeginInfo){.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=renderPass,
        .framebuffer=framebuffer,.renderArea={{0,0},{W,H}},.clearValueCount=1,.pClearValues=&clear},VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    
    // Submit
    VK_CHECK(vkQueueSubmit(queue,1,&(VkSubmitInfo){.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd},fence));
    VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX));
    printf("Render done\n");
    
    // Read back and check
    VkImageSubresource subres={VK_IMAGE_ASPECT_COLOR_BIT,0,0};
    VkSubresourceLayout layout; vkGetImageSubresourceLayout(device,rtImg,&subres,&layout);
    printf("Layout: offset=%llu rowPitch=%llu\n",(unsigned long long)layout.offset,(unsigned long long)layout.rowPitch);
    
    void *ptr; VK_CHECK(vkMapMemory(device,rtMem,0,VK_WHOLE_SIZE,0,&ptr));
    uint32_t *pix = (uint32_t*)((char*)ptr + layout.offset);
    printf("Pixel[0,0]=0x%08X (expect blue ~0x004D0000 BGRA)\n", pix[0]);
    printf("Pixel[W/2,H/2]=0x%08X (expect triangle color)\n", pix[(H/2)*(layout.rowPitch/4) + W/2]);
    
    // Copy to GBM
    void *mapData=NULL; uint32_t mapStride;
    void *gbmPtr=gbm_bo_map(bo,0,0,W,H,GBM_BO_TRANSFER_WRITE,&mapStride,&mapData);
    if(gbmPtr){
        for(uint32_t y=0;y<H;y++) memcpy((char*)gbmPtr+y*mapStride,(char*)ptr+layout.offset+y*layout.rowPitch,W*4);
        gbm_bo_unmap(bo,mapData);
        printf("Copied to GBM\n");
    }
    vkUnmapMemory(device,rtMem);
    
    drmModeSetCrtc(drm_fd,crtc_id,fb_id,0,0,&conn->connector_id,1,mode);
    printf("Should show RGB triangle on blue for 5s\n");
    sleep(5);
    return 0;
}
