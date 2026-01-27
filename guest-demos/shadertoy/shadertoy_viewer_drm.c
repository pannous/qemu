/* ShaderToy Viewer - DRM Direct Rendering (No GLFW/Wayland)
 *
 * Architecture (same as working triangle/vkcube demos):
 *   VkImage (LINEAR, HOST_VISIBLE) ← render shader
 *        ↓
 *   memcpy to double-buffered GBM (XRGB8888)
 *        ↓
 *   DRM scanout (immediate mode)
 *
 * Features:
 *   - Shader compilation and loading
 *   - iTime, iResolution, iMouse uniforms
 *   - Animation loop with FPS limiting
 *   - No display server required
 */

#define _POSIX_C_SOURCE 199309L

// C++ includes first
#include <fstream>
#include <string>
#include <vector>

// C includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

// Graphics libraries
extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
}
#include <vulkan/vulkan.h>

#define VK_CHECK(x) do { \
    VkResult r = (x); \
    if (r) { printf("VK err %d @ line %d\n", r, __LINE__); exit(1); } \
} while(0)

const uint32_t DEFAULT_WIDTH = 1280;
const uint32_t DEFAULT_HEIGHT = 720;
const float DEFAULT_DURATION = 30.0f; // Run for 30 seconds by default

struct UniformBufferObject {
    alignas(16) float iResolution[3];
    alignas(4) float iTime;
    alignas(16) float iMouse[4];
};

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *p, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < p->memoryTypeCount; i++)
        if ((bits & (1 << i)) && (p->memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

std::string getShaderBaseName(const std::string& path) {
    size_t lastSlash = path.find_last_of("/\\");
    size_t lastDot = path.find_last_of(".");
    std::string filename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    return (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot - (lastSlash == std::string::npos ? 0 : lastSlash + 1));
}

std::string getShaderDirectory(const std::string& path) {
    size_t lastSlash = path.find_last_of("/\\");
    return (lastSlash == std::string::npos) ? "." : path.substr(0, lastSlash);
}

std::string getAbsolutePath(const std::string& path) {
    if (!path.empty() && path[0] == '/') return path;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        return std::string(cwd) + "/" + path;
    }
    return path;
}

bool compileAndLoadShader(const std::string& fragPath, std::string& outVertSpv, std::string& outFragSpv) {
    std::string absFragPath = getAbsolutePath(fragPath);
    std::string baseName = getShaderBaseName(absFragPath);
    std::string shaderDir = getShaderDirectory(absFragPath);

    bool isGlslFile = (fragPath.size() >= 5 && fragPath.substr(fragPath.size() - 5) == ".glsl");

    std::string tempFrag;
    if (isGlslFile) {
        tempFrag = absFragPath;
        printf("✓ Using GLSL shader: %s\n", tempFrag.c_str());
    } else {
        tempFrag = shaderDir + "/" + baseName + ".glsl";

        // Check if already in Vulkan format
        std::ifstream checkFile(absFragPath);
        std::string firstLine;
        bool needsConversion = true;
        if (checkFile.is_open() && std::getline(checkFile, firstLine)) {
            if (firstLine.find("#version 450") != std::string::npos) {
                needsConversion = false;
                std::string copyCmd = "cp \"" + absFragPath + "\" \"" + tempFrag + "\"";
                if (system(copyCmd.c_str()) != 0) return false;
            }
        }
        checkFile.close();

        if (needsConversion) {
            std::string convertCmd = "python3 /opt/3d/metalshade/convert_book_of_shaders.py \"" +
                                    absFragPath + "\" \"" + tempFrag + "\"";
            if (system(convertCmd.c_str()) != 0) {
                fprintf(stderr, "✗ Shader conversion failed\n");
                return false;
            }
        }
    }

    outFragSpv = shaderDir + "/" + baseName + ".frag.spv";
    outVertSpv = shaderDir + "/" + baseName + ".vert.spv";

    // Compile to SPIR-V
    std::string compileCmd = "/opt/3d/metalshade/glsl_compile.sh \"" + tempFrag + "\" \"" + outFragSpv + "\"";
    if (system(compileCmd.c_str()) != 0) {
        fprintf(stderr, "✗ Shader compilation failed\n");
        return false;
    }

    printf("✓ Compiled: %s\n", outFragSpv.c_str());

    // Copy generic vertex shader
    std::string copyVertCmd = "cp /opt/3d/metalshade/vert.spv \"" + outVertSpv + "\"";
    if (system(copyVertCmd.c_str()) != 0) {
        fprintf(stderr, "✗ Failed to copy vertex shader\n");
        return false;
    }

    printf("✓ Vertex shader: %s\n", outVertSpv.c_str());
    return true;
}

std::vector<char> readFile(const std::string& filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "Failed to open: %s\n", filename.c_str());
        return std::vector<char>();
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    return data;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::string shaderPath = "/opt/3d/metalshade/shadertoy.frag";
    float duration = DEFAULT_DURATION;

    if (argc > 1) {
        shaderPath = argv[1];
    }
    if (argc > 2) {
        duration = atof(argv[2]);
        if (duration <= 0) duration = DEFAULT_DURATION;
    }

    printf("ShaderToy Viewer - DRM Direct Rendering\n");
    printf("Shader: %s\n", shaderPath.c_str());
    printf("Duration: %.1f seconds\n", duration);

    // Compile shader before initializing graphics
    std::string vertSpv, fragSpv;
    if (!compileAndLoadShader(shaderPath, vertSpv, fragSpv)) {
        fprintf(stderr, "✗ Shader compilation failed. Fix errors and try again.\n");
        return 1;
    }

    // === DRM/GBM Setup ===
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    drmSetMaster(drm_fd);
    drmModeRes* res = drmModeGetResources(drm_fd);

    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED) break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) {
        fprintf(stderr, "No connected display\n");
        return 1;
    }

    drmModeModeInfo* mode = &conn->modes[0];
    uint32_t W = mode->hdisplay, H = mode->vdisplay;
    printf("Display: %ux%u\n", W, H);

    drmModeEncoder* enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];

    // Create double-buffered GBM scanout buffers
    struct gbm_device* gbm = gbm_create_device(drm_fd);
    struct gbm_bo* bo[2];
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

    // === Vulkan Setup ===
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    }, NULL, &instance));

    uint32_t gpuCount = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s\n", props.deviceName);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    float qp = 1.0f;
    VkDevice device;
    VK_CHECK(vkCreateDevice(gpu, &(VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1,
            .pQueuePriorities = &qp
        }
    }, NULL, &device));

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // === Render Target: LINEAR + HOST_VISIBLE ===
    VkImage rtImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    }, NULL, &rtImg));

    VkMemoryRequirements rtReq;
    vkGetImageMemoryRequirements(device, rtImg, &rtReq);

    VkDeviceMemory rtMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rtReq.size,
        .memoryTypeIndex = find_mem(&memProps, rtReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &rtMem));

    VK_CHECK(vkBindImageMemory(device, rtImg, rtMem, 0));

    void* rtPtr;
    VK_CHECK(vkMapMemory(device, rtMem, 0, VK_WHOLE_SIZE, 0, &rtPtr));

    VkImageView rtView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = rtImg,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    }, NULL, &rtView));

    // === Render Pass ===
    VkRenderPass renderPass;
    VK_CHECK(vkCreateRenderPass(device, &(VkRenderPassCreateInfo){
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &(VkAttachmentDescription){
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL
        },
        .subpassCount = 1,
        .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &(VkAttachmentReference){
                .attachment = 0,
                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            }
        }
    }, NULL, &renderPass));

    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(device, &(VkFramebufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = renderPass,
        .attachmentCount = 1,
        .pAttachments = &rtView,
        .width = W,
        .height = H,
        .layers = 1
    }, NULL, &framebuffer));

    // === Shaders ===
    auto vertCode = readFile(vertSpv);
    auto fragCode = readFile(fragSpv);
    if (vertCode.empty() || fragCode.empty()) {
        fprintf(stderr, "Failed to load compiled shaders\n");
        return 1;
    }

    VkShaderModule vertMod, fragMod;
    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertCode.size(),
        .pCode = reinterpret_cast<const uint32_t*>(vertCode.data())
    }, NULL, &vertMod));

    VK_CHECK(vkCreateShaderModule(device, &(VkShaderModuleCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragCode.size(),
        .pCode = reinterpret_cast<const uint32_t*>(fragCode.data())
    }, NULL, &fragMod));

    // === Uniform Buffer ===
    VkBuffer uboBuf;
    VK_CHECK(vkCreateBuffer(device, &(VkBufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(UniformBufferObject),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
    }, NULL, &uboBuf));

    VkMemoryRequirements uboReq;
    vkGetBufferMemoryRequirements(device, uboBuf, &uboReq);

    VkDeviceMemory uboMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = uboReq.size,
        .memoryTypeIndex = find_mem(&memProps, uboReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &uboMem));

    VK_CHECK(vkBindBufferMemory(device, uboBuf, uboMem, 0));

    void* uboPtr;
    vkMapMemory(device, uboMem, 0, sizeof(UniformBufferObject), 0, &uboPtr);

    // === Texture Image (simple gradient like original) ===
    const uint32_t texWidth = 256;
    const uint32_t texHeight = 256;
    VkDeviceSize imageSize = texWidth * texHeight * 4;

    VkBuffer stagingBuf;
    VK_CHECK(vkCreateBuffer(device, &(VkBufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = imageSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    }, NULL, &stagingBuf));

    VkMemoryRequirements stagingReq;
    vkGetBufferMemoryRequirements(device, stagingBuf, &stagingReq);

    VkDeviceMemory stagingMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = stagingReq.size,
        .memoryTypeIndex = find_mem(&memProps, stagingReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    }, NULL, &stagingMem));

    VK_CHECK(vkBindBufferMemory(device, stagingBuf, stagingMem, 0));

    void* texData;
    vkMapMemory(device, stagingMem, 0, imageSize, 0, &texData);
    for (uint32_t y = 0; y < texHeight; y++) {
        for (uint32_t x = 0; x < texWidth; x++) {
            uint32_t idx = (y * texWidth + x) * 4;
            float fx = x / (float)texWidth;
            float fy = y / (float)texHeight;
            ((uint8_t*)texData)[idx + 0] = (uint8_t)(fx * 255);
            ((uint8_t*)texData)[idx + 1] = (uint8_t)(fy * 255);
            ((uint8_t*)texData)[idx + 2] = (uint8_t)((fx + fy) * 128);
            ((uint8_t*)texData)[idx + 3] = 255;
        }
    }
    vkUnmapMemory(device, stagingMem);

    VkImage texImg;
    VK_CHECK(vkCreateImage(device, &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = { texWidth, texHeight, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    }, NULL, &texImg));

    VkMemoryRequirements texReq;
    vkGetImageMemoryRequirements(device, texImg, &texReq);

    VkDeviceMemory texMem;
    VK_CHECK(vkAllocateMemory(device, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = texReq.size,
        .memoryTypeIndex = find_mem(&memProps, texReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    }, NULL, &texMem));

    VK_CHECK(vkBindImageMemory(device, texImg, texMem, 0));

    // Command pool for setup commands
    VkCommandPool setupCmdPool;
    VK_CHECK(vkCreateCommandPool(device, &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    }, NULL, &setupCmdPool));

    VkCommandBuffer setupCmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = setupCmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    }, &setupCmd));

    // Transition texture image and copy data
    vkBeginCommandBuffer(setupCmd, &(VkCommandBufferBeginInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    });

    vkCmdPipelineBarrier(setupCmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1,
        &(VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = texImg,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        });

    vkCmdCopyBufferToImage(setupCmd, stagingBuf, texImg,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
        &(VkBufferImageCopy){
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { texWidth, texHeight, 1 }
        });

    vkCmdPipelineBarrier(setupCmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1,
        &(VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = texImg,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        });

    vkEndCommandBuffer(setupCmd);

    VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &setupCmd
    }, VK_NULL_HANDLE));
    vkQueueWaitIdle(queue);

    vkDestroyBuffer(device, stagingBuf, NULL);
    vkFreeMemory(device, stagingMem, NULL);
    vkDestroyCommandPool(device, setupCmdPool, NULL);

    VkImageView texView;
    VK_CHECK(vkCreateImageView(device, &(VkImageViewCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texImg,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    }, NULL, &texView));

    VkSampler texSampler;
    VK_CHECK(vkCreateSampler(device, &(VkSamplerCreateInfo){
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 16.0f
    }, NULL, &texSampler));

    // === Descriptor Set Layout ===
    VkDescriptorSetLayout descLayout;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &(VkDescriptorSetLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = (VkDescriptorSetLayoutBinding[]){
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT }
        }
    }, NULL, &descLayout));

    VkPipelineLayout pipelineLayout;
    VK_CHECK(vkCreatePipelineLayout(device, &(VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descLayout
    }, NULL, &pipelineLayout));

    // === Pipeline ===
    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = (VkPipelineShaderStageCreateInfo[]){
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertMod, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragMod, .pName = "main" }
        },
        .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        },
        .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        },
        .pViewportState = &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &(VkViewport){ 0, 0, (float)W, (float)H, 0, 1 },
            .scissorCount = 1,
            .pScissors = &(VkRect2D){ {0, 0}, {W, H} }
        },
        .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0f
        },
        .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
        },
        .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &(VkPipelineColorBlendAttachmentState){
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
            }
        },
        .layout = pipelineLayout,
        .renderPass = renderPass
    }, NULL, &pipeline));

    // === Descriptor Pool and Set ===
    VkDescriptorPool descPool;
    VK_CHECK(vkCreateDescriptorPool(device, &(VkDescriptorPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = (VkDescriptorPoolSize[]){
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }
        }
    }, NULL, &descPool));

    VkDescriptorSet descSet;
    VK_CHECK(vkAllocateDescriptorSets(device, &(VkDescriptorSetAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descLayout
    }, &descSet));

    vkUpdateDescriptorSets(device, 2, (VkWriteDescriptorSet[]){
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descSet, .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .pBufferInfo = &(VkDescriptorBufferInfo){ uboBuf, 0, sizeof(UniformBufferObject) } },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = descSet, .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .pImageInfo = &(VkDescriptorImageInfo){
              .sampler = texSampler,
              .imageView = texView,
              .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }
    }, 0, NULL);

    // === Command Pool/Buffer ===
    VkCommandPool cmdPool;
    VK_CHECK(vkCreateCommandPool(device, &(VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    }, NULL, &cmdPool));

    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    }, &cmd));

    VkFence fence;
    VK_CHECK(vkCreateFence(device, &(VkFenceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    }, NULL, &fence));

    // Get image layout for copying
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, rtImg,
        &(VkImageSubresource){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, &layout);

    // Set initial mode
    drmModeSetCrtc(drm_fd, crtc_id, fb_id[0], 0, 0, &conn->connector_id, 1, mode);

    printf("✓ Running shader animation\n");
    printf("Controls: Ctrl+C to stop\n\n");

    struct timespec start, last_frame, last_report;
    clock_gettime(CLOCK_MONOTONIC, &start);
    last_frame = start;
    last_report = start;
    int frames = 0;
    int frames_since_report = 0;
    int current_buffer = 0;

    // Target 60 FPS
    const long target_frame_ns = 16666666; // 16.67ms

    // === Main Render Loop ===
    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9f;

        // Check duration
        if (t >= duration) {
            printf("\n✓ Duration reached (%.1fs)\n", duration);
            break;
        }

        // Update uniforms
        UniformBufferObject ubo;
        ubo.iResolution[0] = (float)W;
        ubo.iResolution[1] = (float)H;
        ubo.iResolution[2] = 1.0f;
        ubo.iTime = t;
        ubo.iMouse[0] = 0.0f;
        ubo.iMouse[1] = 0.0f;
        ubo.iMouse[2] = 0.0f;
        ubo.iMouse[3] = 0.0f;
        memcpy(uboPtr, &ubo, sizeof(ubo));

        // Record command buffer
        vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
        });

        vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderPass,
            .framebuffer = framebuffer,
            .renderArea = { {0, 0}, {W, H} },
            .clearValueCount = 1,
            .pClearValues = &(VkClearValue){ .color = { {0.0f, 0.0f, 0.0f, 1.0f} } }
        }, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout, 0, 1, &descSet, 0, NULL);
        vkCmdDraw(cmd, 6, 1, 0, 0); // Full-screen quad (2 triangles)
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        // Submit and wait
        VK_CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd
        }, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        // Copy to GBM buffer
        void* gbmPtr = NULL;
        uint32_t gbmStride;
        void* mapData = NULL;
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

        // Frame rate limiting
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

    // === Cleanup ===
    vkUnmapMemory(device, rtMem);
    vkUnmapMemory(device, uboMem);
    vkDeviceWaitIdle(device);

    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmdPool, NULL);
    vkDestroyDescriptorPool(device, descPool, NULL);
    vkDestroySampler(device, texSampler, NULL);
    vkDestroyImageView(device, texView, NULL);
    vkDestroyImage(device, texImg, NULL);
    vkFreeMemory(device, texMem, NULL);
    vkDestroyBuffer(device, uboBuf, NULL);
    vkFreeMemory(device, uboMem, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
    vkDestroyDescriptorSetLayout(device, descLayout, NULL);
    vkDestroyShaderModule(device, vertMod, NULL);
    vkDestroyShaderModule(device, fragMod, NULL);
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

    return 0;
}
