// Android video configuration and host-display queries.
//
// The window and its swapchain belong to the Vulkan renderer
// (runtime/platform/android/renderer_vk.cpp): the Activity's Surface is published through
// androidapp::NativeWindowHandle(), and the renderer re-creates its VK_KHR_android_surface
// swapchain whenever that handle changes. This file only holds the settings the shared
// runtime reads (gx/video.h) and the display queries perf/crash logging uses.
#include "../../gx/video.h"

#include "../../log.h"

#include <algorithm>

VideoConfig g_videoConfig;

bool Video_Init()
{
    if (!g_videoConfig.windowWidth) g_videoConfig.windowWidth = 1280;
    if (!g_videoConfig.windowHeight) g_videoConfig.windowHeight = 720;
    LOG_INFO("video: Vulkan, internal scale {}x ({})", g_videoConfig.internalScale,
             g_videoConfig.widescreen ? "16:9" : "4:3");
    return true;
}

// The Activity is always fullscreen while playing; the getter keeps the settings UI and
// the crash report honest.
bool Video_IsFullscreen() { return true; }

void Video_SetFullscreen(bool) {}

void Video_SetWindowSize(int width, int height)
{
    if (width > 0) g_videoConfig.windowWidth = width;
    if (height > 0) g_videoConfig.windowHeight = height;
}

float Video_DisplayRefreshHz()
{
    // The NDK exposes the display mode only from API 30, and the game paces itself from VI
    // interrupts, so the host refresh is only used for statistics. 60 Hz is the panel rate
    // of the low-end devices this targets; a 90/120 Hz panel simply presents twice as often.
    return 60.0f;
}

bool Video_VsyncEnabled() { return g_videoConfig.vsync; }

bool Video_AllowTearing() { return !g_videoConfig.vsync; }
