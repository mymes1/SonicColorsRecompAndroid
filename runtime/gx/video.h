#pragma once
#include <cstdint>

struct VideoConfig
{
    int windowWidth = 1280;
    int windowHeight = 720;
    int internalScale = 3;     // EFB resolution multiplier
    bool vsync = false;        // the game paces itself from VI interrupts
    bool widescreen = true;    // present 16:9
    bool integerScale = false; // present at integer multiples of 640×(360|480); letterbox remainder
    bool fullscreen = false;
};

extern VideoConfig g_videoConfig;

// The Direct3D 11 device belongs to the desktop backends only; the Android backend is
// Vulkan and never defines these. ShaderResourceView is an alias for its COM interface so
// that this header stays free of d3d11.h (shared code includes it on every platform).
#ifndef SCR_ANDROID
#include <d3d11.h>
extern ID3D11Device* g_device;
extern ID3D11DeviceContext* g_ctx;
typedef ID3D11ShaderResourceView VideoTextureView;
#else
struct ID3D11ShaderResourceView;
typedef ID3D11ShaderResourceView VideoTextureView;
#endif

bool Video_Init();
// Presents the given texture region (an XFB copy) to the window. Android: the region of the
// renderer's own EFB, so the view is unused there.
void Video_Present(VideoTextureView* srv, float u0, float v0, float u1, float v1);
// Applied on the window thread (settings changed while the game runs)
void Video_SetFullscreen(bool on);
void Video_SetWindowSize(int width, int height);
// Host monitor refresh (Hz) for the game window; 0 if unknown. Independent of guest VI.
float Video_DisplayRefreshHz();
bool Video_VsyncEnabled();
bool Video_AllowTearing();   // DXGI ALLOW_TEARING when VSync is off (may be false)
