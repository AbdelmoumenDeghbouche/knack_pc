/* Check whether native-size depth/stencil images can be blitted to reduced
 * renderer targets. The renderer needs both directions for live presets. */
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static VkDeviceSize largest_local_heap(VkPhysicalDevice device) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            memory.memoryHeaps[i].size > largest) {
            largest = memory.memoryHeaps[i].size;
        }
    }
    return largest;
}

static int better_device(VkPhysicalDevice candidate, VkPhysicalDevice current) {
    VkPhysicalDeviceProperties next, old;
    vkGetPhysicalDeviceProperties(candidate, &next);
    vkGetPhysicalDeviceProperties(current, &old);
    const int next_api = next.apiVersion >= VK_API_VERSION_1_3;
    const int old_api = old.apiVersion >= VK_API_VERSION_1_3;
    if (next_api != old_api) return next_api;
    const int next_discrete = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const int old_discrete = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    if (next_discrete != old_discrete) return next_discrete;
    const int next_cpu = next.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const int old_cpu = old.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (next_cpu != old_cpu) return !next_cpu;
    return largest_local_heap(candidate) > largest_local_heap(current);
}

int main(void) {
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bbport scene scaling probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, NULL, &instance) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot create Vulkan instance\n", stderr);
        return 1;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || !count) {
        fputs("GPU scene scaling: no Vulkan device\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices || vkEnumeratePhysicalDevices(instance, &count, devices) != VK_SUCCESS) {
        fputs("GPU scene scaling: cannot enumerate Vulkan devices\n", stderr);
        free(devices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    /* Match vk_instance.cpp's default ranking or its explicit BB_GPU_ID index. */
    VkPhysicalDevice selected = devices[0];
    const char *gpu_id = getenv("BB_GPU_ID");
    if (gpu_id && atoi(gpu_id) >= 0) {
        const unsigned long index = strtoul(gpu_id, NULL, 10);
        if (index >= count) {
            fputs("GPU scene scaling: BB_GPU_ID is outside the device list\n", stderr);
            free(devices);
            vkDestroyInstance(instance, NULL);
            return 1;
        }
        selected = devices[index];
    } else {
        for (uint32_t i = 1; i < count; ++i)
            if (better_device(devices[i], selected)) selected = devices[i];
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selected, &props);
    const struct { VkFormat format; const char *name; } formats[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
        {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
    };
    int supported = 1;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        VkFormatProperties features;
        vkGetPhysicalDeviceFormatProperties(selected, formats[i].format, &features);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((features.optimalTilingFeatures & required) != required) {
            fprintf(stderr, "GPU scene scaling: %s lacks blit support for %s\n",
                    props.deviceName, formats[i].name);
            supported = 0;
        }
    }
    if (supported) fprintf(stderr, "GPU scene scaling: %s supports live presets\n", props.deviceName);
    free(devices);
    vkDestroyInstance(instance, NULL);
    return supported ? 0 : 1;
}
