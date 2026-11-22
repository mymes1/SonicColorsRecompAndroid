// Vulkan swapchain for the Activity's Surface (VK_KHR_android_surface).
//
// Present mode: FIFO when the config asks for vsync, otherwise IMMEDIATE (tearing allowed,
// lowest latency) with MAILBOX as the fallback; the game paces itself from VI interrupts,
// so an extra queued frame would only add input lag. The surface is created from the
// ANativeWindow the Activity publishes, and everything is thrown away when that window is
// replaced, because a VkSurfaceKHR is bound to one ANativeWindow for its whole life.
#include "video_swapchain.h"

#include "../../config.h"
#include "../../gx/video.h"
#include "../../log.h"
#include "platform_android.h"
#include "vk_context.h"

#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <vector>

namespace
{
VkSurfaceKHR s_surface = VK_NULL_HANDLE;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkFormat s_format = VK_FORMAT_UNDEFINED;
VkColorSpaceKHR s_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
VkExtent2D s_extent{};
std::vector<VkImage> s_images;
std::vector<VkImageView> s_views;
// One "render finished" semaphore per image (reused every frame that image is chosen), and
// a small ring of "image available" semaphores: acquire needs the semaphore before the image
// index is known, so they cannot be per image.
std::vector<VkSemaphore> s_renderDone;
VkSemaphore s_acquireSems[4] = {};
uint32_t s_nextAcquire = 0;
VkSemaphore s_acquiredSem = VK_NULL_HANDLE;
uint64_t s_window = 0;
bool s_needsRecreate = false;

VkCompositeAlphaFlagBitsKHR PickAlpha(const VkSurfaceCapabilitiesKHR& caps)
{
    // Android surfaces are usually opaque; INHERIT is next (the composition decides).
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
}

bool PickFormat()
{
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical, s_surface, &n, nullptr) != VK_SUCCESS || !n) return false;
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical, s_surface, &n, formats.data());
    // The EFB is RGBA8; presenting to an RGBA8 or BGRA8 UNORM surface needs no conversion.
    // SRGB_NONLINEAR is what every Android compositor offers.
    const VkFormat want[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32 };
    for (VkFormat f : want)
        for (auto& s : formats)
            if (s.format == f)
            {
                s_format = s.format;
                s_colorSpace = s.colorSpace;
                return true;
            }
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
    {
        s_format = VK_FORMAT_R8G8B8A8_UNORM;   // "you choose"
        s_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return true;
    }
    s_format = formats[0].format;
    s_colorSpace = formats[0].colorSpace;
    return s_format != VK_FORMAT_UNDEFINED;
}

VkPresentModeKHR PickPresentMode()
{
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical, s_surface, &n, nullptr) != VK_SUCCESS || !n)
        return VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical, s_surface, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (g_videoConfig.vsync) return VK_PRESENT_MODE_FIFO_KHR;
    if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

void DestroyImageViews()
{
    for (VkImageView v : s_views)
        if (v) vkDestroyImageView(g_vk.device, v, nullptr);
    s_views.clear();
    s_images.clear();
    for (VkSemaphore s : s_renderDone)
        if (s) vkDestroySemaphore(g_vk.device, s, nullptr);
    s_renderDone.clear();
}

bool CreateSurface()
{
    if (!s_window) return false;
    auto create = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(g_vk.instance, "vkCreateAndroidSurfaceKHR");
    if (!create)
    {
        LOG_ERROR("vulkan: vkCreateAndroidSurfaceKHR unavailable (VK_KHR_android_surface missing)");
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = (ANativeWindow*)s_window;
    VkResult r = create(g_vk.instance, &ci, nullptr, &s_surface);
    if (r != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: cannot create the Android surface ({})", (int)r);
        s_surface = VK_NULL_HANDLE;
        return false;
    }

    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.physical, g_vk.queueFamily, s_surface, &present);
    if (!present)
    {
        // Every Android GPU we target has one graphics+present family; if not, the device
        // was created without a presentable queue and nothing can be shown.
        LOG_ERROR("vulkan: queue family {} cannot present to this window", g_vk.queueFamily);
        vkDestroySurfaceKHR(g_vk.instance, s_surface, nullptr);
        s_surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool CreateSwapchain()
{
    if (s_surface == VK_NULL_HANDLE && !CreateSurface()) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.physical, s_surface, &caps) != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: cannot query surface capabilities");
        return false;
    }
    if (!PickFormat()) return false;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX)
    {
        ANativeWindow* window = (ANativeWindow*)s_window;
        extent.width = (uint32_t)std::max(1, ANativeWindow_getWidth(window));
        extent.height = (uint32_t)std::max(1, ANativeWindow_getHeight(window));
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;
    s_extent = extent;

    uint32_t imageCount = caps.minImageCount + 1;   // one more than the minimum: no stalls
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = s_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = s_format;
    ci.imageColorSpace = s_colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    // Transfer destination: the XFB blit is a vkCmdBlitImage/vkCmdCopyImage into it, so the
    // render pass never has to be reopened for the present.
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.compositeAlpha = PickAlpha(caps);
    ci.presentMode = PickPresentMode();
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;
    // The ANativeWindow already carries the display rotation, so the swapchain images are
    // pre-rotated: ask for IDENTITY when the surface supports it.
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;

    VkSwapchainKHR chain = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(g_vk.device, &ci, nullptr, &chain);
    if (r != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: vkCreateSwapchainKHR failed ({})", (int)r);
        return false;
    }
    DestroyImageViews();
    if (s_swapchain) vkDestroySwapchainKHR(g_vk.device, s_swapchain, nullptr);
    s_swapchain = chain;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g_vk.device, s_swapchain, &count, nullptr);
    s_images.resize(count);
    vkGetSwapchainImagesKHR(g_vk.device, s_swapchain, &count, s_images.data());
    s_views.resize(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; i++)
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s_format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(g_vk.device, &vi, nullptr, &s_views[i]) != VK_SUCCESS)
        {
            LOG_ERROR("vulkan: swapchain image view failed");
            DestroyImageViews();
            return false;
        }
    }
    s_renderDone.assign(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; i++)
    {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(g_vk.device, &si, nullptr, &s_renderDone[i]) != VK_SUCCESS)
        {
            LOG_ERROR("vulkan: present semaphore creation failed");
            return false;
        }
    }
    if (!s_acquireSems[0])
        for (auto& sem : s_acquireSems)
        {
            VkSemaphoreCreateInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(g_vk.device, &si, nullptr, &sem);
        }
    s_needsRecreate = false;
    LOG_INFO("vulkan: swapchain {}x{} {} images, present mode {}", extent.width, extent.height, count, (int)ci.presentMode);
    return true;
}
}   // namespace

void VideoSwapchain_SetWindow(uint64_t window)
{
    if (window == s_window) return;
    s_window = window;
    // The surface belongs to the old window; drop everything and start over lazily.
    VideoSwapchain_Destroy();
    if (s_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(g_vk.instance, s_surface, nullptr);
        s_surface = VK_NULL_HANDLE;
    }
    s_needsRecreate = true;
}

bool VideoSwapchain_Ensure()
{
    if (s_swapchain && !s_needsRecreate) return true;
    return CreateSwapchain();
}

void VideoSwapchain_Destroy()
{
    if (!g_vk.device) return;
    DestroyImageViews();
    if (s_swapchain) vkDestroySwapchainKHR(g_vk.device, s_swapchain, nullptr);
    s_swapchain = VK_NULL_HANDLE;
    s_needsRecreate = true;
}

VkSwapchainKHR VideoSwapchain_Handle() { return s_swapchain; }
VkFormat VideoSwapchain_Format() { return s_format; }
VkExtent2D VideoSwapchain_Extent() { return s_extent; }
VkImageView VideoSwapchain_ImageView(uint32_t index) { return index < s_views.size() ? s_views[index] : VK_NULL_HANDLE; }
VkImage VideoSwapchain_Image(uint32_t index) { return index < s_images.size() ? s_images[index] : VK_NULL_HANDLE; }

uint32_t VideoSwapchain_Acquire()
{
    if (!VideoSwapchain_Ensure()) return UINT32_MAX;
    VkSemaphore sem = s_acquireSems[s_nextAcquire++ % 4];
    if (!sem) return UINT32_MAX;
    s_acquiredSem = sem;
    uint32_t index = UINT32_MAX;
    VkResult r = vkAcquireNextImageKHR(g_vk.device, s_swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        s_needsRecreate = true;
        if (r == VK_SUBOPTIMAL_KHR && s_swapchain) return index;   // usable for this frame
        return UINT32_MAX;
    }
    if (r != VK_SUCCESS) return UINT32_MAX;
    return index;
}

VkSemaphore VideoSwapchain_AcquireSemaphore() { return s_acquiredSem; }
VkSemaphore VideoSwapchain_RenderDoneSemaphore(uint32_t index)
{
    return index < s_renderDone.size() ? s_renderDone[index] : VK_NULL_HANDLE;
}

bool VideoSwapchain_Present(uint32_t index)
{
    if (s_swapchain == VK_NULL_HANDLE || index >= s_images.size()) return false;
    VkSemaphore done = VideoSwapchain_RenderDoneSemaphore(index);
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = done ? 1u : 0u;
    pi.pWaitSemaphores = done ? &done : nullptr;
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_swapchain;
    pi.pImageIndices = &index;
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
    {
        s_needsRecreate = true;
        if (r == VK_ERROR_SURFACE_LOST_KHR)
        {
            VideoSwapchain_Destroy();
            if (s_surface != VK_NULL_HANDLE)
            {
                vkDestroySurfaceKHR(g_vk.instance, s_surface, nullptr);
                s_surface = VK_NULL_HANDLE;
            }
        }
        return false;
    }
    return r == VK_SUCCESS;
}

void VideoSwapchain_Invalidate() { s_needsRecreate = true; }
