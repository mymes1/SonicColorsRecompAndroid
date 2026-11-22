#pragma once
// Minimal Vulkan 1.0/1.1 bootstrap for Android. Loads libvulkan.so at runtime through
// the NDK's <vulkan/vulkan.h> entry points (no link-time dependency on a specific
// loader) and picks the GPU features the GX backend expects.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

struct VkContext
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

    // Device properties (logged; the tuner uses them for Mali/Valhall heuristics)
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    char driverInfo[VK_MAX_DRIVER_INFO_SIZE] = {};
    uint32_t vendorId = 0, deviceId = 0;
    uint32_t apiVersion = 0;
    uint32_t driverVersion = 0;
    bool isMali = false;       // ARM Mali (Midgard/Valhall/…)
    bool isValhall = false;    // Mali-G57/G68/G77/G78/G7xx/G5xx (G5x and newer G-series)
    bool isAdrenoSixFivePlus = false;
    bool hasTimestamps = false;
    uint32_t timestampPeriodAge = 0;

    // Enabled device features
    bool samplerAnisotropy = false;
    bool depthClamp = false;
    bool independentBlend = false;
    bool fillModeNonSolid = false;
    float maxSamplerAnisotropy = 1.0f;
    uint32_t maxPushConstantsSize = 0;

    // Vulkan 1.1 entry points (loaded when the instance reports 1.1+)
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2 = nullptr;

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugUtilsMessenger = nullptr;
};

extern VkContext g_vk;

// Creates the instance/device. `validation` adds the validation layer when present.
bool Vk_Init(bool validation);
void Vk_Shutdown();

// A memory type index for `typeBits` with the requested properties (first match on a
// DEVICE_LOCAL heap when `preferDeviceLocal`).
uint32_t Vk_MemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags, bool preferDeviceLocal = false);
// Allocates and binds device memory to `image` / `buffer`.
bool Vk_AllocImage(VkImage image, VkMemoryPropertyFlags flags, VkDeviceMemory* out);
bool Vk_AllocBuffer(VkBuffer buffer, VkMemoryPropertyFlags flags, VkDeviceMemory* out);

// One-shot command buffer helpers (synchronous; used at init and for resource uploads).
VkCommandBuffer Vk_BeginOneShot();
void Vk_EndOneShot(VkCommandBuffer cmd);

// Format/shader-compilation result objects shared with the backend.
VkFormat Vk_FindDepthFormat();
VkFormat Vk_FindColorFormat(bool wantRgba8, bool wantRgb10);
std::string Vk_DeviceSummary();
