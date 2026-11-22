// Vulkan bootstrap: instance, physical device choice and the logical device.
//
// Android devices in the target class (including ARM Mali-G57 MC2 / Valhall) only
// guarantee Vulkan 1.0/1.1, no geometry shaders and no compute fallbacks we could rely
// on, so the renderer is written against a 1.0 feature set plus optional extensions.
#include "vk_context.h"

#include "../../log.h"

#include <android/log.h>

#include <array>
#include <cstring>
#include <vector>

VkContext g_vk;

namespace
{
std::vector<std::string> s_extensions;

const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool HasLayer(const char* name)
{
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) != VK_SUCCESS) return false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOG_ERROR("vulkan: {}", data->pMessage ? data->pMessage : "(no message)");
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOG_WARN("vulkan: {}", data->pMessage ? data->pMessage : "(no message)");
    else
        LOG_DEBUG("vulkan: {}", data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool ExtensionAvailable(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// Mali-G57 is Valhall; the MC number (2) only changes the core count. Valhall's tiler
// prefers render passes that stay open (we render one frame per pass) and dislikes
// mid-render-pass render-target changes (which the GX backend does not do), plus small
// numbers of large draws.
void ClassifyDevice(const VkPhysicalDeviceProperties& p)
{
    std::strncpy(g_vk.deviceName, p.deviceName, sizeof(g_vk.deviceName) - 1);
    g_vk.vendorId = p.vendorID;
    g_vk.deviceId = p.deviceID;
    g_vk.apiVersion = p.apiVersion;
    g_vk.driverVersion = p.driverVersion;
    g_vk.isMali = p.vendorID == 0x13B5;   // ARM
    if (g_vk.isMali)
    {
        // G31/G51/G52/G76 are Bifrost; G57 and newer (G68, G77, G78, G710, G610, G510…)
        // are Valhall. The driver string also names the core.
        const char* name = p.deviceName;
        g_vk.isValhall = std::strstr(name, "G57") || std::strstr(name, "G58") || std::strstr(name, "G68") ||
                         std::strstr(name, "G77") || std::strstr(name, "G78") || std::strstr(name, "G710") ||
                         std::strstr(name, "G610") || std::strstr(name, "G510") || std::strstr(name, "G715") ||
                         std::strstr(name, "Immortalis");
    }
    else if (p.vendorID == 0x5143)   // Qualcomm
    {
        g_vk.isAdrenoSixFivePlus = p.deviceID >= 0x05000000 && p.deviceID < 0x06000000;
    }
}

}   // namespace

bool Vk_Init(bool validation)
{
    // ------------------------------------------------------------ instance
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount) vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

    // Android: VK_KHR_surface/VK_KHR_android_surface come from the platform; the loader
    // provides them. The backend requests them separately when it creates the window.
    std::vector<const char*> enabled;
    if (ExtensionAvailable(exts, "VK_EXT_debug_utils") && validation)
    {
        enabled.push_back("VK_EXT_debug_utils");
        s_extensions.push_back("VK_EXT_debug_utils");
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SonicColorsRecomp";
    app.pEngineName = "SCR GX";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> layers;
    if (validation && HasLayer(kValidationLayer)) layers.push_back(kValidationLayer);

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.ppEnabledExtensionNames = enabled.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &g_vk.instance);
    if (r != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: vkCreateInstance failed ({})", (int)r);
        return false;
    }

    if (!s_extensions.empty())
    {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = DebugCallback;
            create(g_vk.instance, &dci, nullptr, &g_vk.debugMessenger);
        }
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        g_vk.destroyDebugUtilsMessenger = destroy;
    }
    g_vk.getPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
        g_vk.instance, "vkGetPhysicalDeviceProperties2");
    g_vk.getPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(
        g_vk.instance, "vkGetPhysicalDeviceFeatures2");

    // ------------------------------------------------------------ physical device
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &gpuCount, nullptr);
    if (!gpuCount)
    {
        LOG_ERROR("vulkan: no physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(g_vk.instance, &gpuCount, gpus.data());

    VkPhysicalDeviceProperties best{};
    int bestScore = -1;
    for (auto gpu : gpus)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(gpu, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        // Discrete > integrated > other. Android phones report integrated.
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (p.apiVersion >= VK_API_VERSION_1_1) score += 1;
        // Dolphin-style "adapter selection" does not apply on Android; the system GPU is it.
        if (score > bestScore)
        {
            bestScore = score;
            g_vk.physical = gpu;
            best = p;
            memcpy(g_vk.deviceName, p.deviceName, sizeof(g_vk.deviceName));
        }
    }
    if (g_vk.physical == VK_NULL_HANDLE)
    {
        LOG_ERROR("vulkan: no usable physical device");
        return false;
    }
    ClassifyDevice(best);

    // ------------------------------------------------------------ queue family
    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.physical, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.physical, &famCount, fams.data());
    bool found = false;
    for (uint32_t i = 0; i < famCount; i++)
    {
        const bool graphics = (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (!graphics) continue;
        // Prefer a family that can also present (avoids a second queue and cross-queue sync).
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.physical, i, VK_NULL_HANDLE, &present);   // may fail without a surface
        if (!found || present)
        {
            g_vk.queueFamily = i;
            found = true;
            if (present) break;
        }
    }
    if (!found)
    {
        LOG_ERROR("vulkan: no graphics queue family");
        return false;
    }

    // ------------------------------------------------------------ device features
    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(g_vk.physical, &available);
    VkPhysicalDeviceFeatures wanted{};
    wanted.samplerAnisotropy = available.samplerAnisotropy;
    wanted.depthClamp = available.depthClamp;
    wanted.independentBlend = available.independentBlend;
    wanted.fillModeNonSolid = available.fillModeNonSolid;
    wanted.fragmentStoresAndAtomics = available.fragmentStoresAndAtomics;
    wanted.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
    // GX needs these unconditionally; every Android GPU has them.
    wanted.shaderInt64 = VK_FALSE;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_vk.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> devExts;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &wanted;

    // Query features2 when available (Mali exposes extra toggles through it).
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (g_vk.getPhysicalDeviceFeatures2) g_vk.getPhysicalDeviceFeatures2(g_vk.physical, &f2);

    // Prefer 1.1 features that matter for this renderer when the device offers them:
    // shaderDrawParameters is Vulkan 1.1 and lets the VS see the instance index; we do not
    // need it for GX. Depth-stencil resolve is optional.
    if (best.apiVersion >= VK_API_VERSION_1_1)
    {
        // ask for nothing beyond 1.0 to stay compatible; SPIR-V 1.0 is what we emit.
    }

    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();

    VkResult dr = vkCreateDevice(g_vk.physical, &dci, nullptr, &g_vk.device);
    if (dr != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: vkCreateDevice failed ({})", (int)dr);
        return false;
    }
    vkGetDeviceQueue(g_vk.device, g_vk.queueFamily, 0, &g_vk.queue);
    g_vk.getDeviceProcAddr = vkGetDeviceProcAddr;

    g_vk.samplerAnisotropy = wanted.samplerAnisotropy != 0;
    g_vk.depthClamp = wanted.depthClamp != 0;
    g_vk.independentBlend = wanted.independentBlend != 0;
    g_vk.fillModeNonSolid = wanted.fillModeNonSolid != 0;
    g_vk.maxPushConstantsSize = best.limits.maxPushConstantsSize;
    if (g_vk.samplerAnisotropy) g_vk.maxSamplerAnisotropy = best.limits.maxSamplerAnisotropy;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_vk.queueFamily;
    if (vkCreateCommandPool(g_vk.device, &pci, nullptr, &g_vk.cmdPool) != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: command pool creation failed");
        return false;
    }

    LOG_INFO("vulkan: {} ({:#06x}:{:#08x}) api {}.{}.{} driver {}.{}.{} | queue {} | "
             "aniso {} depthClamp {} timestamps {}",
             g_vk.deviceName, g_vk.vendorId, g_vk.deviceId, VK_API_VERSION_MAJOR(g_vk.apiVersion),
             VK_API_VERSION_MINOR(g_vk.apiVersion), VK_API_VERSION_PATCH(g_vk.apiVersion),
             VK_API_VERSION_MAJOR(g_vk.driverVersion) >> 0, (g_vk.driverVersion >> 12) & 0x3FF,
             g_vk.driverVersion & 0xFFF, g_vk.queueFamily, g_vk.samplerAnisotropy, g_vk.depthClamp,
             g_vk.hasTimestamps);
    if (g_vk.isMali)
        LOG_INFO("vulkan: ARM Mali GPU{} ({})", g_vk.isValhall ? ", Valhall generation" : "",
                 g_vk.isValhall ? "GX state changes are folded into the frame's single render pass"
                                : "older Midgard/Bifrost driver");
    return true;
}

void Vk_Shutdown()
{
    if (g_vk.debugMessenger && g_vk.destroyDebugUtilsMessenger)
        g_vk.destroyDebugUtilsMessenger(g_vk.instance, g_vk.debugMessenger, nullptr);
    if (g_vk.cmdPool) vkDestroyCommandPool(g_vk.device, g_vk.cmdPool, nullptr);
    if (g_vk.device) vkDestroyDevice(g_vk.device, nullptr);
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, nullptr);
    g_vk = VkContext{};
}

uint32_t Vk_MemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags, bool preferDeviceLocal)
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_vk.physical, &mp);
    if (preferDeviceLocal)
    {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((typeBits & (1u << i)) && mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                return i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return ~0u;
}

bool Vk_AllocImage(VkImage image, VkMemoryPropertyFlags flags, VkDeviceMemory* out)
{
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_vk.device, image, &req);
    const uint32_t idx = Vk_MemoryType(req.memoryTypeBits, flags, true);
    if (idx == ~0u) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(g_vk.device, &ai, nullptr, out) != VK_SUCCESS) return false;
    return vkBindImageMemory(g_vk.device, image, *out, 0) == VK_SUCCESS;
}

bool Vk_AllocBuffer(VkBuffer buffer, VkMemoryPropertyFlags flags, VkDeviceMemory* out)
{
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_vk.device, buffer, &req);
    const uint32_t idx = Vk_MemoryType(req.memoryTypeBits, flags);
    if (idx == ~0u) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(g_vk.device, &ai, nullptr, out) != VK_SUCCESS) return false;
    return vkBindBufferMemory(g_vk.device, buffer, *out, 0) == VK_SUCCESS;
}

VkCommandBuffer Vk_BeginOneShot()
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = g_vk.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_vk.device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void Vk_EndOneShot(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    vkFreeCommandBuffers(g_vk.device, g_vk.cmdPool, 1, &cmd);
}

VkFormat Vk_FindDepthFormat()
{
    static const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                           VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
    for (VkFormat f : candidates)
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, f, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat Vk_FindColorFormat(bool wantRgba8, bool wantRgb10)
{
    if (wantRgb10)
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    (void)wantRgba8;
    return VK_FORMAT_R8G8B8A8_UNORM;
}

std::string Vk_DeviceSummary()
{
    return std::string(g_vk.deviceName);
}
