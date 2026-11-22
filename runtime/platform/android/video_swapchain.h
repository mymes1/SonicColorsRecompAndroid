#pragma once
// The Android window's Vulkan swapchain, owned by the renderer.
//
// The Activity publishes its Surface (an ANativeWindow) as a raw handle through
// androidapp::NativeWindowHandle(); the renderer turns the first non-zero handle into a
// VK_KHR_android_surface and keeps the swapchain until the handle changes (rotation,
// pause/resume recreates the SurfaceView).
#include <vulkan/vulkan.h>

// The window handle changed (0 = window destroyed).
void VideoSwapchain_SetWindow(uint64_t window);
// True when a swapchain exists for the current window; creates or re-creates it as needed.
bool VideoSwapchain_Ensure();
// Destroys the swapchain objects (image views first). Keeps the VkSurfaceKHR.
void VideoSwapchain_Destroy();

VkSwapchainKHR VideoSwapchain_Handle();
VkFormat VideoSwapchain_Format();
VkExtent2D VideoSwapchain_Extent();
// Index of the image acquired for this frame (UINT32_MAX when nothing was acquired).
uint32_t VideoSwapchain_Acquire();
// Semaphores for the acquire/present pair of the last VideoSwapchain_Acquire().
VkSemaphore VideoSwapchain_AcquireSemaphore();
VkSemaphore VideoSwapchain_RenderDoneSemaphore(uint32_t index);
VkImageView VideoSwapchain_ImageView(uint32_t index);
// The swapchain image itself (the blit source/destination of a present).
VkImage VideoSwapchain_Image(uint32_t index);
// Presents the acquired image; returns false when the swapchain must be re-created.
bool VideoSwapchain_Present(uint32_t index);
// The swapchain is out of date (resize, rotation); recreate before the next acquire.
void VideoSwapchain_Invalidate();
