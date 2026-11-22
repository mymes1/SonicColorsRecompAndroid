#pragma once
// Vulkan implementation of the GX renderer contract (gx/gx_state.h) for the Android
// backend, replacing the Direct3D 11 backend in gx/renderer.cpp.
//
// Structure follows the hardware: one render pass per frame that stays open for the whole
// frame (Mali/Valhall tile memory is only useful that way), an offscreen EFB attachment in
// tile-local memory, textures decoded to RGBA8 and uploaded from TMEM/RAM, and per-draw
// pipelines built from the generated GLSL (shadergen_glsl.cpp) compiled with shaderc.
//
// Diagnostics that only exist for the desktop replay harness (A2* scratch isolation,
// A3a draw attributes, TAS screenshots) keep their signatures but do nothing: the Android
// build ships the game, not the verification tooling.
#include "../../gx/gx_state.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Entry points the runtime calls besides the Render_* set in gx_state.h.
void Renderer_Init();                                   // device, swapchain-less resources
void Renderer_Shutdown();
void Renderer_RepeatLastXfb();                          // re-present the last XFB (no new data)
void Renderer_ResetSamplers();                          // anisotropy/settings changed
void Renderer_RequestScreenshot();
int Renderer_PrecompileShaders(const std::string& dir);  // offline cache build (not used on device)
uint32_t EFB_Read32(uint32_t ea);
void EFB_Write32(uint32_t ea, uint32_t v);
// Bookkeeping for a frame that was handed to the compositor: screenshots, texture eviction,
// periodic statistics. Called by the backend itself after a present.
void Renderer_OnPresentedFrame();

// ---------------------------------------------------------------- texture packs
// gx/hires.cpp decodes pack images and hands the pixels here; the renderer owns the GPU object
// and returns an id (0 when creation failed).
uint32_t Renderer_PackTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
VkImageView Renderer_PackTextureView(uint32_t id);
void Renderer_PackTextureAddRef(uint32_t id);
void Renderer_PackTextureRelease(uint32_t id);

// Present the current swapchain image (called by the EFB copy path once per frame).
void Video_Present(float u0, float v0, float u1, float v1);

extern uint32_t g_framesPresented;
