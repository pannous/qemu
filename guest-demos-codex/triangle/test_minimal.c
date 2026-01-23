/* Minimal Vulkan test to isolate Venus crashes */
#include <stdio.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(x) do { VkResult r = x; if (r != VK_SUCCESS) { printf("VK err %d @ line %d\n", r, __LINE__); return 1; } } while(0)

int main(void) {
    printf("Creating instance...\n"); fflush(stdout);
    VkInstanceCreateInfo inst_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance instance;
    VK_CHECK(vkCreateInstance(&inst_info, NULL, &instance));
    printf("Instance created\n"); fflush(stdout);

    printf("Enumerating devices...\n"); fflush(stdout);
    uint32_t gpu_count = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &gpu_count, &gpu);
    printf("Found %u devices\n", gpu_count); fflush(stdout);

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
    printf("Device created\n"); fflush(stdout);

    printf("Creating fence...\n"); fflush(stdout);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
    printf("Fence created!\n"); fflush(stdout);

    printf("Destroying fence...\n"); fflush(stdout);
    vkDestroyFence(device, fence, NULL);
    printf("Fence destroyed\n"); fflush(stdout);

    printf("Cleaning up...\n"); fflush(stdout);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("SUCCESS!\n");
    return 0;
}
