/*
 * Simple Vulkan enumeration test
 * Compile: gcc -o test_vulkan test_vulkan.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

int main(int argc, char **argv) {
    printf("=== Simple Vulkan Test ===\n\n");

    /* Create instance */
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VulkanTest",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "NoEngine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };

    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };

    VkInstance instance;
    VkResult result = vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        printf("[FAIL] vkCreateInstance failed: %d\n", result);
        return 1;
    }
    printf("[OK] vkCreateInstance succeeded\n");

    /* Enumerate physical devices */
    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    if (result != VK_SUCCESS) {
        printf("[FAIL] vkEnumeratePhysicalDevices (count) failed: %d\n", result);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    printf("[OK] Found %u physical device(s)\n", deviceCount);

    if (deviceCount == 0) {
        printf("[WARN] No physical devices found!\n");
        vkDestroyInstance(instance, NULL);
        return 0;
    }

    VkPhysicalDevice *devices = malloc(sizeof(VkPhysicalDevice) * deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices);
    if (result != VK_SUCCESS) {
        printf("[FAIL] vkEnumeratePhysicalDevices (fetch) failed: %d\n", result);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    for (uint32_t i = 0; i < deviceCount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        printf("\n[Device %u]\n", i);
        printf("  Name: %s\n", props.deviceName);
        printf("  Type: %d\n", props.deviceType);
        printf("  API Version: %u.%u.%u\n",
               VK_VERSION_MAJOR(props.apiVersion),
               VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion));
        printf("  Vendor ID: 0x%x\n", props.vendorID);
        printf("  Device ID: 0x%x\n", props.deviceID);
    }

    free(devices);
    vkDestroyInstance(instance, NULL);
    printf("\n=== Test Complete ===\n");
    return 0;
}
