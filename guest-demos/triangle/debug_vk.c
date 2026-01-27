#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

int main(void) {
    VkInstance instance;
    VkInstanceCreateInfo instInfo = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkResult r = vkCreateInstance(&instInfo, NULL, &instance);
    if (r != VK_SUCCESS) {
        printf("Failed to create instance: %d\n", r);
        return 1;
    }

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, NULL);
    printf("GPU count: %u\n", gpuCount);

    VkPhysicalDevice gpu;
    gpuCount = 1;
    vkEnumeratePhysicalDevices(instance, &gpuCount, &gpu);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s\n", props.deviceName);

    // List device extensions
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &extCount, NULL);
    printf("Available device extensions: %u\n", extCount);

    VkExtensionProperties *exts = malloc(extCount * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &extCount, exts);
    for (uint32_t i = 0; i < extCount; i++) {
        printf("  %s (v%u)\n", exts[i].extensionName, exts[i].specVersion);
    }

    // List queue families
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, NULL);
    printf("\nQueue families: %u\n", qfCount);

    VkQueueFamilyProperties *qfProps = malloc(qfCount * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qfCount, qfProps);
    for (uint32_t i = 0; i < qfCount; i++) {
        printf("  [%u] flags=0x%x count=%u\n", i, qfProps[i].queueFlags, qfProps[i].queueCount);
    }

    // Try to create device without any extensions
    printf("\nAttempting device creation...\n");
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &qp
    };
    VkDeviceCreateInfo devInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qInfo,
        .enabledExtensionCount = 0
    };

    VkDevice device;
    r = vkCreateDevice(gpu, &devInfo, NULL, &device);
    if (r == VK_SUCCESS) {
        printf("Device created successfully!\n");
        vkDestroyDevice(device, NULL);
    } else {
        printf("Device creation failed: %d\n", r);
    }

    vkDestroyInstance(instance, NULL);
    return 0;
}
