// Vulkan backend for the GX GPU (Android).
//
// The Android counterpart of gx/renderer.cpp (Direct3D 11): the same contract
// (gx/gx_state.h), the same GX semantics, the same batching, but recorded onto Vulkan
// command buffers and presented through the Activity's ANativeWindow.
//
// Why it is shaped like this — this is what a tile-based Mali (the Mali-G57 MC2 this
// targets) wants, and it is correct everywhere else too:
//   * ONE render pass per frame, opened by the frame's first draw and closed by the XFB
//     copy that presents it. Valhall keeps the 640x528xN EFB in tile memory for the whole
//     pass, so the two to four thousand draws a frame of Sonic Colors issues never touch
//     system memory through the framebuffer.
//   * Anything that must read the EFB (copy-to-texture, XFB present, clear) ends the pass,
//     transfers, and reopens it. The D3D11 backend does the same with CopyResource; here it
//     costs a pass restart instead of a copy inserted behind our back.
//   * Geometry is batched exactly like the D3D path (a batch breaks on topology, vertex
//     layout or pipeline state) and appended to one host-visible vertex/index ring per
//     frame, so a frame is a handful of buffer writes plus the draw calls.
//   * Shaders come from gx/shadergen_glsl.cpp and are compiled with shaderc, which the NDK
//     ships prebuilt. Keys are cached (VSKey/PSKey) exactly like the PC build caches DXBC,
//     and a missing shader is compiled on the spot rather than on a worker thread: Android
//     has no shader cache to preload from, and a wrong draw is worse than a 20 ms hitch on
//     the first appearance of a new effect.
//
// State the D3D path stores in constant buffers is copied into per-frame uniform buffers
// before each draw (the frame's buffers are not reused until the fence for that frame has
// signalled, so a draw's constants stay valid for the whole frame).
#include "renderer_vk.h"
#include "../../gx/hires.h"

#include "../../config.h"
#include "../../debug/a2b_fifo_archive.h"
#include "../../debug/a2c_scratch_replay.h"
#include "../../debug/a2g_natural_trace.h"
#include "../../debug/a3a_draw_attr.h"
#include "../../debug/a1b_pose_capture.h"
#include "../../debug/a1c_gx_capture.h"
#include "../../gx/shadergen_glsl.h"
#include "../../gx/texture_decode.h"
#include "../../gx/vertex_loader.h"
#include "../../gx/video.h"
#include "../../kernel/memory.h"
#include "../../log.h"
#include "../../platform/platform.h"
#include "../../perf.h"
#include "platform_android.h"
#include "video_swapchain.h"
#include "vk_context.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

uint32_t g_framesPresented = 0;

namespace
{
constexpr int EFB_W = 640, EFB_H = 528;
constexpr uint32_t kFramesInFlight = 2;   // double buffering: no CPU stall per frame
constexpr uint32_t kXFWords = 0x680;      // the float window the shaders index
constexpr size_t kVertexBytes = 6u << 20;
constexpr size_t kIndexBytes = 3u << 20;

// ------------------------------------------------------------------ helpers
uint64_t Hash64(const void* data, size_t n, uint64_t seed = 0x9E3779B97F4A7C15ull)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

template <typename K>
struct KeyHash
{
    size_t operator()(const K& k) const { return (size_t)Hash64(&k, sizeof(k)); }
};

inline float AsFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

inline uint32_t BP(uint32_t r) { return g_gx.bp[r]; }

uint32_t TexReg(int map, uint32_t base03, uint32_t base47)
{
    return map < 4 ? BP(base03 + map) : BP(base47 + (map - 4));
}

uint32_t PixelFormat() { return BP(0x43) & 7; }

// TEV konstant registers (written by Render_OnBP; read by FillPSConstants).
int32_t g_tevKonst[4][4];

enum class Topo : uint32_t { Tri = 0, Line = 1, Point = 2 };

// ------------------------------------------------------------------ shader compilation
shaderc_compiler_t s_compiler = nullptr;

VkShaderModule MakeModule(const std::vector<uint32_t>& spv)
{
    if (spv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.device, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

std::vector<uint32_t> CompileGLSL(const std::string& src, bool vertex, const std::string& debugName)
{
    std::vector<uint32_t> out;
    if (!s_compiler) s_compiler = shaderc_compiler_initialize();
    if (!s_compiler)
    {
        LOG_ERROR("shaderc: initialisation failed");
        return out;
    }
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    shaderc_compile_options_set_source_language(opts, shaderc_source_language_glsl);
    shaderc_compile_options_set_target_spirv(opts, shaderc_spirv_version_1_0);
    shaderc_compilation_result_t result =
        shaderc_compile_into_spv(s_compiler, src.c_str(), src.size(), vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                                 debugName.c_str(), "main", opts);
    shaderc_compile_options_release(opts);
    if (!result) return out;
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
    {
        const char* msg = shaderc_result_get_error_message(result);
        LOG_ERROR("shaderc: {} failed: {}", debugName, msg ? msg : "(no message)");
        // A failing shader on device cannot be reproduced on a PC: keep its source.
        if (const char* dir = getenv("SCR_ANDROID_SHADER_DUMP"))
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (FILE* f = fopen((fs::path(dir) / (debugName + ".glsl")).c_str(), "wb"))
            {
                fwrite(src.data(), 1, src.size(), f);
                fclose(f);
            }
        }
        shaderc_result_release(result);
        return out;
    }
    const uint32_t* words = (const uint32_t*)shaderc_result_get_bytes(result);
    const size_t n = shaderc_result_get_length(result) / 4;
    if (words && n) out.assign(words, words + n);
    shaderc_result_release(result);
    return out;
}

struct ShaderEntry
{
    VkShaderModule module = VK_NULL_HANDLE;
};

std::unordered_map<VSKey, ShaderEntry, KeyHash<VSKey>> s_vsCache;
std::unordered_map<PSKey, ShaderEntry, KeyHash<PSKey>> s_psCache;

// ------------------------------------------------------------------ pipelines
struct PipelineKey
{
    VSKey vs{};
    PSKey ps{};
    uint32_t blend = 0;   // BP 0x41 & 0xFFFF
    uint32_t depth = 0;   // BP 0x40 & 0x1F
    uint32_t cull = 0;    // (BP 0x00 >> 14) & 3
    uint32_t topo = 0;
    bool operator==(const PipelineKey& o) const
    {
        return memcmp(&vs, &o.vs, sizeof(vs)) == 0 && memcmp(&ps, &o.ps, sizeof(ps)) == 0 && blend == o.blend &&
               depth == o.depth && cull == o.cull && topo == o.topo;
    }
};

std::unordered_map<PipelineKey, VkPipeline, KeyHash<PipelineKey>> s_pipelines;

// ------------------------------------------------------------------ device objects
// One slice of constant-buffer memory per flush. The frame's buffers are not reused until
// the fence for that frame has signalled, and every draw keeps reading the slice it was
// recorded with, so constants cannot be shared between flushes of one frame.
// A frame of Sonic Colors issues well under a thousand batches; the cap keeps the arena at
// ~11 MiB per frame while leaving room for a pathological scene.
constexpr uint32_t kMaxFlushes = 1536;

struct Frame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // XF memory snapshot and stage constants, one slice per flush.
    VkBuffer cb = VK_NULL_HANDLE;
    VkDeviceMemory cbMem = VK_NULL_HANDLE;
    void* cbMap = nullptr;

    // Geometry ring: appended to, never wrapped inside a frame.
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceMemory vbMem = VK_NULL_HANDLE, ibMem = VK_NULL_HANDLE;
    void* vbMap = nullptr;
    void* ibMap = nullptr;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    uint32_t flushes = 0;
    VkDeviceSize vbPos = 0, ibPos = 0;

    // EFB peek readback for this frame (colour and depth), lazily created.
    VkBuffer peekColor = VK_NULL_HANDLE, peekDepth = VK_NULL_HANDLE;
    VkDeviceMemory peekColorMem = VK_NULL_HANDLE, peekDepthMem = VK_NULL_HANDLE;
    void* peekColorMap = nullptr;
    void* peekDepthMap = nullptr;
    bool peekPending = false;
};

Frame s_frames[kFramesInFlight];
uint32_t s_frameIdx = 0;

VkCommandPool s_cmdPool = VK_NULL_HANDLE;
VkDescriptorSetLayout s_setLayout = VK_NULL_HANDLE;
VkPipelineLayout s_pipeLayout = VK_NULL_HANDLE;


// EFB and its snapshots.
VkImage s_efbColor = VK_NULL_HANDLE, s_efbDepth = VK_NULL_HANDLE;
VkDeviceMemory s_efbColorMem = VK_NULL_HANDLE, s_efbDepthMem = VK_NULL_HANDLE;
VkImageView s_efbColorView = VK_NULL_HANDLE, s_efbDepthView = VK_NULL_HANDLE;
VkRenderPass s_efbPass = VK_NULL_HANDLE;
VkFramebuffer s_efbFb = VK_NULL_HANDLE;
VkImage s_colorCopy = VK_NULL_HANDLE, s_depthCopy = VK_NULL_HANDLE;
VkDeviceMemory s_colorCopyMem = VK_NULL_HANDLE, s_depthCopyMem = VK_NULL_HANDLE;
VkImageView s_colorCopyView = VK_NULL_HANDLE, s_depthCopyView = VK_NULL_HANDLE;

// Copy / XFB passes. Both write one RGBA8 image and hand it to a sampler or a blit; the XFB
// pass leaves the image as a transfer source, which is what the present blit needs.
VkRenderPass s_copyPass = VK_NULL_HANDLE, s_xfbPass = VK_NULL_HANDLE;
std::vector<VkFramebuffer> s_presentFbs;

// Generic samplers for the util quad (the game's textures use GetSampler()).
VkSampler s_linearClamp = VK_NULL_HANDLE;

// Util pipelines: clear, EFB->texture copy, EFB->XFB copy.
VkPipeline s_clearPipe = VK_NULL_HANDLE, s_copyPipe = VK_NULL_HANDLE, s_xfbPipe = VK_NULL_HANDLE;

// Dummy texture for stages that bind nothing (samplers must not be undefined).
VkImage s_dummyImage = VK_NULL_HANDLE;
VkDeviceMemory s_dummyMem = VK_NULL_HANDLE;
VkImageView s_dummyView = VK_NULL_HANDLE;
VkSampler s_pointClamp = VK_NULL_HANDLE;

// The presented XFB: an EFB region copied out by the last to-XFB EFB copy.
VkImage s_xfb = VK_NULL_HANDLE;
VkDeviceMemory s_xfbMem = VK_NULL_HANDLE;
VkImageView s_xfbView = VK_NULL_HANDLE;
uint32_t s_xfbW = 0, s_xfbH = 0;

// ------------------------------------------------------------------ batching
struct BatchVertex : HostVertex
{
    BatchVertex() {}
};

std::vector<BatchVertex> s_verts;
std::vector<uint32_t> s_indices;
std::vector<uint8_t> s_skip;
Topo s_topo = Topo::Tri;
uint32_t s_batchComponents = 0;
bool s_xfDirty = true;
bool s_ready = false;

// Dynamic state the batches share: the scissor from the last flush is restored after the
// clear quad, which overrides it.
VkRect2D s_scissor{};

int s_scale = 3;
uint32_t s_drawCalls = 0;
uint32_t s_frame = 0;

// ------------------------------------------------------------------ textures
struct TexEntry
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    uint64_t hash = 0;
    uint64_t dataHash = 0;
    uint32_t dataHashFrame = ~0u;
    uint32_t hashedFrame = ~0u;
    uint32_t lastFrame = 0;
    uint32_t createFrame = 0;
    bool hasMips = false;
    bool efbCopy = false;
    uint32_t packId = 0;   // set for a texture-pack replacement (see Renderer_PackTexture)
    uint32_t ramSize = 0;
    uint64_t ramHash = 0;
};

std::unordered_map<uint64_t, TexEntry> s_texCache;      // decoded textures by source address/format
std::unordered_map<uint32_t, TexEntry> s_efbCopies;     // EFB copies by guest destination address

uint8_t s_tmem[1024 * 1024];   // TMEM (palettes), 1 MiB as on hardware

struct SamplerKey
{
    uint32_t mode0 = 0, mode1 = 0;
    bool mips = false;
    bool operator==(const SamplerKey& o) const { return mode0 == o.mode0 && mode1 == o.mode1 && mips == o.mips; }
};
struct SamplerKeyHash
{
    size_t operator()(const SamplerKey& k) const
    {
        return (size_t)((uint64_t)k.mode0 | ((uint64_t)k.mode1 << 21) | ((uint64_t)k.mips << 42));
    }
};
std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> s_samplerCache;

// One constant slice per flush: the frame's XF memory snapshot followed by the two stages'
// constants. Every range is 256-byte aligned, which every Vulkan implementation accepts for
// uniform buffers.
constexpr uint32_t Align256(uint32_t n) { return (n + 255u) & ~255u; }
constexpr uint32_t kXfBytes = Align256(kXFWords * 4);
constexpr uint32_t kVSConstBytes = Align256((uint32_t)sizeof(VSConstants));
constexpr uint32_t kPSConstBytes = Align256((uint32_t)sizeof(PSConstants));
constexpr uint32_t kConstSliceBytes = kXfBytes + kVSConstBytes + kPSConstBytes;
}   // namespace

// ================================================================== resources
namespace
{
bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** map)
{
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_vk.device, &ci, nullptr, buf) != VK_SUCCESS) return false;
    if (!Vk_AllocBuffer(*buf, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mem))
    {
        LOG_ERROR("vulkan: cannot allocate {} MiB of buffer memory", (uint64_t)(size >> 20));
        return false;
    }
    if (map && vkMapMemory(g_vk.device, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return false;
    return true;
}

bool CreateImage(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage* image,
                 VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk.device, &ci, nullptr, image) != VK_SUCCESS) return false;
    if (!Vk_AllocImage(*image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem)) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { aspect, 0, 1, 0, 1 };
    return vkCreateImageView(g_vk.device, &vi, nullptr, view) == VK_SUCCESS;
}

void DestroyTex(TexEntry& e)
{
    if (e.packId)
    {
        // A texture-pack replacement: the pack table owns the image and the view, so only the
        // reference this entry holds is dropped.
        HiRes_Release(e.packId);
        e.packId = 0;
        e = TexEntry{};   // the borrowed handles go with it
        return;
    }
    if (e.view) vkDestroyImageView(g_vk.device, e.view, nullptr);
    if (e.image) vkDestroyImage(g_vk.device, e.image, nullptr);
    if (e.mem) vkFreeMemory(g_vk.device, e.mem, nullptr);
    e = TexEntry{};
}

bool CreateSetLayout()
{
    // Binding map, matching shadergen_glsl.cpp:
    //   0 XF memory (uniform float array)        1 VS constants (std140)
    //   11 PS constants (std140)                 2..9 tex0..tex7 (combined image samplers)
    // One layout for both stages: a descriptor set is visible to the whole pipeline, so the
    // vertex and pixel constants get distinct bindings instead of sharing one.
    VkDescriptorSetLayoutBinding b[11]{};
    const uint32_t bindings[11] = { 0, 1, 11, 2, 3, 4, 5, 6, 7, 8, 9 };
    for (uint32_t i = 0; i < 11; i++)
    {
        b[i].binding = bindings[i];
        b[i].descriptorCount = 1;
        if (i < 2)
        {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        else if (i == 2)
        {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        else
        {
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
    }
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 11;
    ci.pBindings = b;
    return vkCreateDescriptorSetLayout(g_vk.device, &ci, nullptr, &s_setLayout) == VK_SUCCESS;
}

bool CreateFrameResources()
{
    for (uint32_t i = 0; i < kFramesInFlight; i++)
    {
        Frame& f = s_frames[i];
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s_cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_vk.device, &ai, &f.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(g_vk.device, &fi, nullptr, &f.fence) != VK_SUCCESS) return false;
        if (!CreateBuffer(kMaxFlushes * kConstSliceBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &f.cb, &f.cbMem, &f.cbMap))
            return false;
        if (!CreateBuffer(kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &f.vb, &f.vbMem, &f.vbMap)) return false;
        if (!CreateBuffer(kIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &f.ib, &f.ibMem, &f.ibMap)) return false;

        // One descriptor set per flush: its textures are fixed, the buffers are slices.
        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFlushes * 3 },
                                          { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxFlushes * 8 } };
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = kMaxFlushes;
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(g_vk.device, &pci, nullptr, &f.descPool) != VK_SUCCESS) return false;
    }
    return true;
}

bool CreateEfb()
{
    const VkImageUsageFlags colorUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!CreateImage(EFB_W * s_scale, EFB_H * s_scale, VK_FORMAT_R8G8B8A8_UNORM, colorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                     &s_efbColor, &s_efbColorMem, &s_efbColorView))
        return false;
    if (!CreateImage(EFB_W * s_scale, EFB_H * s_scale, Vk_FindDepthFormat(), depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT,
                     &s_efbDepth, &s_efbDepthMem, &s_efbDepthView))
        return false;
    if (!CreateImage(EFB_W * s_scale, EFB_H * s_scale, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &s_colorCopy,
                     &s_colorCopyMem, &s_colorCopyView))
        return false;
    return CreateImage(EFB_W * s_scale, EFB_H * s_scale, Vk_FindDepthFormat(),
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &s_depthCopy,
                       &s_depthCopyMem, &s_depthCopyView);
}

bool CreateRenderPasses()
{
    {
        VkAttachmentDescription att[2]{};
        att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = Vk_FindDepthFormat();
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 2;
        ci.pAttachments = att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        if (vkCreateRenderPass(g_vk.device, &ci, nullptr, &s_efbPass) != VK_SUCCESS) return false;
        VkImageView views[2] = { s_efbColorView, s_efbDepthView };
        VkFramebufferCreateInfo fbi{};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = s_efbPass;
        fbi.attachmentCount = 2;
        fbi.pAttachments = views;
        fbi.width = EFB_W * s_scale;
        fbi.height = EFB_H * s_scale;
        fbi.layers = 1;
        if (vkCreateFramebuffer(g_vk.device, &fbi, nullptr, &s_efbFb) != VK_SUCCESS) return false;
    }
    for (int variant = 0; variant < 2; variant++)
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every texel is written
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // variant 0: sampled afterwards (EFB->texture). variant 1: blitted afterwards (XFB).
        att.finalLayout = variant == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        VkRenderPass pass = VK_NULL_HANDLE;
        if (vkCreateRenderPass(g_vk.device, &ci, nullptr, &pass) != VK_SUCCESS) return false;
        if (variant == 0) s_copyPass = pass;
        else s_xfbPass = pass;
    }
    return true;
}

bool CreateSamplers()
{
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(g_vk.device, &ci, nullptr, &s_linearClamp) != VK_SUCCESS) return false;
    VkSamplerCreateInfo pi = ci;
    pi.magFilter = pi.minFilter = VK_FILTER_NEAREST;
    pi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return vkCreateSampler(g_vk.device, &pi, nullptr, &s_pointClamp) == VK_SUCCESS;
}

bool CreateDummyTexture()
{
    if (!CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &s_dummyImage, &s_dummyMem, &s_dummyView))
        return false;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!CreateBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem, &map)) return false;
    *(uint32_t*)map = 0xFFFFFFFFu;   // opaque white
    VkCommandBuffer cmd = Vk_BeginOneShot();
    if (!cmd) return false;
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = s_dummyImage;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toDst);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { 1, 1, 1 };
    vkCmdCopyBufferToImage(cmd, staging, s_dummyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toRead);
    Vk_EndOneShot(cmd);
    vkDestroyBuffer(g_vk.device, staging, nullptr);
    vkFreeMemory(g_vk.device, stagingMem, nullptr);
    return true;
}
}   // namespace

// ================================================================== shaders, pipelines
namespace
{
VkShaderModule GetVS(const VSKey& key)
{
    auto it = s_vsCache.find(key);
    if (it != s_vsCache.end()) return it->second.module;
    VkShaderModule module = MakeModule(CompileGLSL(GenerateVertexShaderGLSL(key), true, "gx_vs"));
    s_vsCache[key] = { module };
    return module;
}

VkShaderModule GetPS(const PSKey& key)
{
    auto it = s_psCache.find(key);
    if (it != s_psCache.end()) return it->second.module;
    VkShaderModule module = MakeModule(CompileGLSL(GenerateFragmentShaderGLSL(key), false, "gx_ps"));
    s_psCache[key] = { module };
    return module;
}

VkBlendFactor BlendFactor(uint32_t f, bool dest, bool hasAlpha, bool alphaChannel)
{
    // BP 0x41 blend factors. Unorm8 targets are the only ones the EFB has, so the hardware's
    // "alpha factors on an alpha-less format" cases collapse to one/zero like in renderer.cpp.
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return dest ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_DST_COLOR;
    case 3: return dest ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 4: return alphaChannel ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC1_COLOR;
    case 5: return alphaChannel ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 6: return hasAlpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
    default: return hasAlpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ZERO;
    }
}

VkPipeline BuildPipeline(const PipelineKey& key, VkShaderModule vs, VkShaderModule ps)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    // HostVertex, one interleaved binding; locations match the generated vertex shader.
    VkVertexInputBindingDescription binding{ 0, (uint32_t)sizeof(HostVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[15]{};
    uint32_t n = 0;
    auto add = [&](VkFormat format, size_t offset) {
        attrs[n].location = n;
        attrs[n].binding = 0;
        attrs[n].format = format;
        attrs[n].offset = (uint32_t)offset;
        n++;
    };
    add(VK_FORMAT_R32G32B32_SFLOAT, offsetof(HostVertex, pos));
    add(VK_FORMAT_R32G32B32_SFLOAT, offsetof(HostVertex, nrm));
    add(VK_FORMAT_R32G32B32_SFLOAT, offsetof(HostVertex, bin));
    add(VK_FORMAT_R32G32B32_SFLOAT, offsetof(HostVertex, tan));
    add(VK_FORMAT_R8G8B8A8_UNORM, offsetof(HostVertex, col[0]));
    add(VK_FORMAT_R8G8B8A8_UNORM, offsetof(HostVertex, col[1]));
    for (int i = 0; i < 8; i++) add(VK_FORMAT_R32G32_SFLOAT, offsetof(HostVertex, tex[i]));
    // The loader packs the nine matrix indices into the three 32-bit words; visible here as
    // four unsigned bytes, which the shader reads as one 32-bit word each.
    add(VK_FORMAT_R8G8B8A8_UINT, offsetof(HostVertex, mtx));

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = n;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = key.topo == (uint32_t)Topo::Line     ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                  : key.topo == (uint32_t)Topo::Point ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                      : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = key.cull == 1 ? VK_CULL_MODE_BACK_BIT : key.cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    // GX front faces are clockwise in screen space, which is Vulkan's clockwise too.
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    static const VkCompareOp ops[8] = { VK_COMPARE_OP_NEVER,      VK_COMPARE_OP_LESS,       VK_COMPARE_OP_EQUAL,
                                        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                        VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (key.depth & 1) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (key.depth & 0x10) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ops[(key.depth >> 1) & 7];
    ds.stencilTestEnable = VK_FALSE;

    bool hasAlpha = PixelFormat() == 1;   // RGBA6_Z24
    uint32_t bm = key.blend;
    VkPipelineColorBlendAttachmentState cb{};
    if ((bm >> 3) & 1) cb.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    if (((bm >> 4) & 1) && hasAlpha) cb.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    if (!cb.colorWriteMask) cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    if (bm & 1)
    {
        cb.blendEnable = VK_TRUE;
        if ((bm >> 11) & 1)   // subtract
        {
            cb.srcColorBlendFactor = cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            cb.srcAlphaBlendFactor = cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cb.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        }
        else
        {
            cb.srcColorBlendFactor = BlendFactor((bm >> 8) & 7, false, hasAlpha, false);
            cb.dstColorBlendFactor = BlendFactor((bm >> 5) & 7, true, hasAlpha, false);
            cb.colorBlendOp = VK_BLEND_OP_ADD;
            cb.srcAlphaBlendFactor = BlendFactor((bm >> 8) & 7, false, hasAlpha, true);
            cb.dstAlphaBlendFactor = BlendFactor((bm >> 5) & 7, true, hasAlpha, true);
            cb.alphaBlendOp = VK_BLEND_OP_ADD;
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dyn;
    ci.layout = s_pipeLayout;
    ci.renderPass = s_efbPass;

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
    if (r != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: pipeline creation failed ({})", (int)r);
        return VK_NULL_HANDLE;
    }
    return pipe;
}

// The clear/copy/present quad: no vertex buffer, positions come from gl_VertexIndex.
VkPipeline BuildUtilPipeline(VkShaderModule vs, VkShaderModule fs, VkRenderPass pass, VkPipelineLayout layout)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dyn;
    ci.layout = layout;
    ci.renderPass = pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) != VK_SUCCESS)
    {
        LOG_ERROR("vulkan: util pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    return pipe;
}

VkPipeline GetPipeline(const PipelineKey& key)
{
    auto it = s_pipelines.find(key);
    if (it != s_pipelines.end()) return it->second;
    VkShaderModule vs = GetVS(key.vs);
    VkShaderModule ps = GetPS(key.ps);
    if (!vs || !ps)
    {
        s_pipelines[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkPipeline pipe = BuildPipeline(key, vs, ps);
    s_pipelines[key] = pipe;
    return pipe;
}

// ---------------------------------------------------------------- GX state -> keys
void BuildVSKey(VSKey& k)
{
    memset(&k, 0, sizeof(k));
    k.components = s_batchComponents;
    k.numTexGens = BP(0x00) & 15;
    k.numColorChans = (BP(0x00) >> 4) & 7;
    k.dualTex = g_gx.xfregs[XF_DUALTEX] & 1;
    for (uint32_t i = 0; i < k.numTexGens && i < 8; i++)
    {
        k.texMtxInfo[i] = g_gx.xfregs[XF_TEXMTXINFO + i] & 0x3FFFE;
        if (k.dualTex) k.postMtxInfo[i] = g_gx.xfregs[XF_POSTMTXINFO + i] & 0x13F;
    }
    k.chan[0] = g_gx.xfregs[XF_COLOR0CNTRL] & 0x7FFF;
    k.chan[1] = g_gx.xfregs[XF_COLOR1CNTRL] & 0x7FFF;
    k.chan[2] = g_gx.xfregs[XF_ALPHA0CNTRL] & 0x7FFF;
    k.chan[3] = g_gx.xfregs[XF_ALPHA1CNTRL] & 0x7FFF;
    for (int c = 0; c < 4; c++)
        if (!(k.chan[c] & 2)) k.chan[c] &= 0x41;
}

void BuildPSKey(PSKey& k)
{
    memset(&k, 0, sizeof(k));
    uint32_t gen = BP(0x00);
    k.numTexGens = gen & 15;
    k.numTevStages = ((gen >> 10) & 15) + 1;
    k.numIndStages = (gen >> 16) & 7;
    for (uint32_t i = 0; i < k.numTevStages; i++)
    {
        k.colorComb[i] = BP(0xC0 + 2 * i) & 0xFFFFFF;
        k.alphaComb[i] = BP(0xC1 + 2 * i) & 0xFFFFFF;
        k.tevInd[i] = BP(0x10 + i) & 0x1FFFFF;
    }
    for (uint32_t i = 0; i < 8; i++)
    {
        k.tevOrder[i] = (i * 2 < k.numTevStages) ? BP(0x28 + i) & 0xFFFFFF : 0;
        k.ksel[i] = BP(0xF6 + i) & 0xFFFFFF;
    }
    k.indOrder = k.numIndStages ? BP(0x27) & 0xFFFFFF : 0;
    uint32_t af = BP(0xF3);
    k.alphaFunc = ((af >> 16) & 7) | (((af >> 19) & 7) << 3) | (((af >> 22) & 3) << 6);
    k.zTexType = (BP(0xF5) & 3) | (((BP(0xF5) >> 2) & 3) << 2);
    uint32_t fogc = BP(0xF1);
    uint32_t fsel = (fogc >> 21) & 7;
    k.fogType = fsel | (((fogc >> 20) & 1) << 3) | (((BP(0xE8) >> 10) & 1) << 4);
    if (fsel < 2) k.fogType = 0;
    k.pixelFormat = PixelFormat();
    k.dstAlphaEnable = (BP(0x42) >> 8) & 1;
    k.perPixelDepth = ((k.zTexType >> 2) & 3) != 0 && (BP(0x40) & 1) ? 1 : 0;
}
}   // namespace

// ================================================================== constants
namespace
{
void FillVSConstants(VSConstants& c, float flipX, float flipY)
{
    memset(&c, 0, sizeof(c));
    const uint32_t* p = &g_gx.xfregs[XF_PROJECTION];
    const float p0 = AsFloat(p[0]), p1 = AsFloat(p[1]), p2 = AsFloat(p[2]);
    const float p3 = AsFloat(p[3]), p4 = AsFloat(p[4]), p5 = AsFloat(p[5]);
    if (p[6] == 0)   // perspective: row-major as the D3D path's row_major float4x4
    {
        const float m[4][4] = { { p0, 0, p1, 0 }, { 0, p2, p3, 0 }, { 0, 0, p4, p5 }, { 0, 0, -1, 0 } };
        memcpy(c.proj, m, sizeof(m));
    }
    else
    {
        const float m[4][4] = { { p0, 0, 0, p1 }, { 0, p2, 0, p3 }, { 0, 0, p4, p5 }, { 0, 0, 0, 1 } };
        memcpy(c.proj, m, sizeof(m));
    }
    const float zRange = XFFloat(0x1000 + XF_VIEWPORT + 2);
    const float farZ = XFFloat(0x1000 + XF_VIEWPORT + 5);
    c.depth[0] = farZ / 16777216.0f;
    c.depth[1] = zRange / 16777216.0f;
    c.depth[2] = flipX;
    c.depth[3] = flipY;
    auto col = [](uint32_t v, int32_t* o) {
        o[0] = (int32_t)(v >> 24);
        o[1] = (int32_t)((v >> 16) & 0xFF);
        o[2] = (int32_t)((v >> 8) & 0xFF);
        o[3] = (int32_t)(v & 0xFF);
    };
    col(g_gx.xfregs[XF_AMBIENT0], c.materials[0]);
    col(g_gx.xfregs[XF_AMBIENT1], c.materials[1]);
    col(g_gx.xfregs[XF_MATERIAL0], c.materials[2]);
    col(g_gx.xfregs[XF_MATERIAL1], c.materials[3]);
    // Subpixel correction is a host-side nicety; GX positions are already at texel centres.
    c.pixelOffset[0] = 0.0f;
    c.pixelOffset[1] = 0.0f;
}

void FillPSConstants(PSConstants& c, const uint32_t* texW, const uint32_t* texH)
{
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 4; i++)
    {
        const uint32_t lo = BP(0xE0 + 2 * i), hi = BP(0xE1 + 2 * i);
        auto s11 = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
        c.colors[i][0] = s11(lo & 0x7FF);
        c.colors[i][3] = s11((lo >> 12) & 0x7FF);
        c.colors[i][2] = s11(hi & 0x7FF);
        c.colors[i][1] = s11((hi >> 12) & 0x7FF);
    }
    memcpy(c.konst, g_tevKonst, sizeof(c.konst));
    const uint32_t af = BP(0xF3);
    c.alphaRef[0] = (int32_t)(af & 0xFF);
    c.alphaRef[1] = (int32_t)((af >> 8) & 0xFF);
    c.alphaRef[2] = (int32_t)(BP(0x42) & 0xFF);
    for (int i = 0; i < 8; i++)
    {
        c.texDim[i][0] = (int32_t)texW[i];
        c.texDim[i][1] = (int32_t)texH[i];
    }
    for (int i = 0; i < 4; i++)
    {
        const uint32_t ss = BP(0x25 + (i >> 1));
        const uint32_t v = (i & 1) ? (ss >> 8) : ss;
        c.indTexScale[i][0] = (int32_t)(v & 15);
        c.indTexScale[i][1] = (int32_t)((v >> 4) & 15);
    }
    for (int m = 0; m < 3; m++)
    {
        const uint32_t ra = BP(0x06 + 3 * m), rb = BP(0x07 + 3 * m), rc = BP(0x08 + 3 * m);
        auto s11 = [](uint32_t v) { return (int32_t)(v << 21) >> 21; };
        const int32_t ma = s11(ra & 0x7FF), mb = s11((ra >> 11) & 0x7FF);
        const int32_t mc = s11(rb & 0x7FF), md = s11((rb >> 11) & 0x7FF);
        const int32_t me = s11(rc & 0x7FF), mf = s11((rc >> 11) & 0x7FF);
        const int32_t scale =
            (int32_t)(((ra >> 22) & 3) | (((rb >> 22) & 3) << 2) | (((rc >> 22) & 3) << 4)) - 17;
        c.indTexMtx[2 * m][0] = ma;
        c.indTexMtx[2 * m][1] = mc;
        c.indTexMtx[2 * m][2] = me;
        c.indTexMtx[2 * m][3] = -scale;
        c.indTexMtx[2 * m + 1][0] = mb;
        c.indTexMtx[2 * m + 1][1] = md;
        c.indTexMtx[2 * m + 1][2] = mf;
        c.indTexMtx[2 * m + 1][3] = -scale;
    }
    const uint32_t ztype = BP(0xF5) & 3;
    if (ztype == 0)
    {
        c.zBias[0][0] = 0;
        c.zBias[0][1] = 0;
        c.zBias[0][2] = 0;
        c.zBias[0][3] = 1;
    }
    else if (ztype == 1)
    {
        c.zBias[0][0] = 256;
        c.zBias[0][1] = 0;
        c.zBias[0][2] = 0;
        c.zBias[0][3] = 1;
    }
    else
    {
        c.zBias[0][0] = 65536;
        c.zBias[0][1] = 256;
        c.zBias[0][2] = 1;
        c.zBias[0][3] = 0;
    }
    c.zBias[1][3] = (int32_t)(BP(0xF4) & 0xFFFFFF);
    // Fog: 11-bit mantissa, 8-bit exponent, sign, packed into 20 bits.
    auto fogFloat = [](uint32_t v) {
        const uint32_t m = v & 0x7FF, e = (v >> 11) & 0xFF, s = (v >> 19) & 1;
        const uint32_t bits = (s << 31) | (e << 23) | (m << 12);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    c.fogF[0] = fogFloat(BP(0xEE));
    c.fogF[1] = fogFloat(BP(0xF1));
    c.fogI[1] = (int32_t)(BP(0xEF) & 0xFFFFFF);
    c.fogI[3] = (int32_t)(BP(0xF0) & 31);
    const uint32_t fcol = BP(0xF2);
    c.fogColor[0] = (int32_t)((fcol >> 16) & 0xFF);
    c.fogColor[1] = (int32_t)((fcol >> 8) & 0xFF);
    c.fogColor[2] = (int32_t)(fcol & 0xFF);
    const uint32_t rangeBase = BP(0xE8);
    const float vpw = 2.0f * XFFloat(0x1000 + XF_VIEWPORT);
    if ((rangeBase >> 10) & 1)
    {
        const int center = (int)(rangeBase & 0x3FF) - 342;
        const float ssc = vpw != 0.0f ? center / vpw : 0.5f;
        c.fogF[2] = ssc * 2.0f - 1.0f;
        c.fogF[3] = vpw * (float)s_scale;
        for (int i = 0; i < 5; i++)
        {
            const uint32_t r = BP(0xE9 + i);
            c.fogRange[(2 * i) / 4][(2 * i) % 4] = (float)((r >> 12) & 0xFFF) / 256.0f * 4.0f;
            c.fogRange[(2 * i + 1) / 4][(2 * i + 1) % 4] = (float)(r & 0xFFF) / 256.0f * 4.0f;
        }
    }
    else
    {
        c.fogF[2] = 0.0f;
        c.fogF[3] = 1.0f;
    }
    c.depthParams[0] = XFFloat(0x1000 + XF_VIEWPORT + 5);
    c.depthParams[1] = XFFloat(0x1000 + XF_VIEWPORT + 2);
}

// ---------------------------------------------------------------- textures
VkSampler GetSampler(uint32_t mode0, uint32_t mode1, bool hasMips)
{
    SamplerKey key{ mode0, mode1, hasMips };
    auto it = s_samplerCache.find(key);
    if (it != s_samplerCache.end()) return it->second;
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    static const VkSamplerAddressMode wrap[4] = { VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                  VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT };
    ci.addressModeU = wrap[mode0 & 3];
    ci.addressModeV = wrap[(mode0 >> 2) & 3];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const bool magLinear = (mode0 >> 4) & 1;
    const uint32_t minF = (mode0 >> 5) & 7;
    const bool minLinear = minF >= 4;
    const bool mipLinear = (minF & 3) == 2;
    const bool useMips = hasMips && (minF & 3) != 0;
    ci.magFilter = magLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = minLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    uint32_t aniso = 1u << ((mode0 >> 19) & 3);
    aniso = std::max<uint32_t>(aniso, (uint32_t)g_config.anisotropy);
    if (aniso > 1 && minLinear && magLinear && g_vk.samplerAnisotropy)
    {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(16.0f, std::min((float)aniso, g_vk.maxSamplerAnisotropy));
    }
    ci.mipLodBias = (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f;
    ci.minLod = useMips ? (float)(mode1 & 0xFF) / 16.0f : 0.0f;
    ci.maxLod = useMips ? (float)((mode1 >> 8) & 0xFF) / 16.0f : 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(g_vk.device, &ci, nullptr, &s) != VK_SUCCESS) return s_linearClamp;
    s_samplerCache[key] = s;
    return s;
}

}   // namespace

// ================================================================== frame lifecycle
// Forward declarations: these live in later sections but are used by the EFB copy path.
void RecordEfbPeekReadback();   // defined with the peek path below
void Renderer_OnPresentedFrame();

namespace
{
struct UtilCB
{
    float rect[4];
    float uv[4];
    float color[4];
    float params[4];
    int32_t iparams[4];
};

Frame& Cur() { return s_frames[s_frameIdx]; }

bool BeginFrameCommands()
{
    Frame& f = Cur();
    if (vkWaitForFences(g_vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    vkResetFences(g_vk.device, 1, &f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    if (f.descPool) vkResetDescriptorPool(g_vk.device, f.descPool, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(f.cmd, &bi) != VK_SUCCESS) return false;
    f.flushes = 0;
    f.vbPos = 0;
    f.ibPos = 0;
    s_xfDirty = true;
    return true;
}

// Ends the open command buffer for the current frame and submits it. `waitSem` / `signalSem`
// are the swapchain semaphores, present only when this frame's XFB actually presents.
bool SubmitFrame(VkSemaphore waitSem, VkSemaphore signalSem, VkPipelineStageFlags waitStage)
{
    Frame& f = Cur();
    if (vkEndCommandBuffer(f.cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (waitSem)
    {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &waitSem;
        si.pWaitDstStageMask = &waitStage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (signalSem)
    {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signalSem;
    }
    return vkQueueSubmit(g_vk.queue, 1, &si, f.fence) == VK_SUCCESS;
}

// The EFB render pass is opened once per frame (tile memory) and closed whenever something
// has to read the EFB, a transfer has to happen, or the frame ends.
bool s_passOpen = false;

void EndEfbPass()
{
    if (!s_passOpen) return;
    vkCmdEndRenderPass(Cur().cmd);
    s_passOpen = false;
    // PIPELINE and descriptor-set bindings do not survive a render pass, so the next pass
    // rebinds everything; nothing has to be reset here.
}

void BeginEfbPass()
{
    if (s_passOpen) return;
    Frame& f = Cur();
    VkClearValue clears[2]{};
    clears[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    clears[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = s_efbPass;
    bi.framebuffer = s_efbFb;
    bi.renderArea.extent = { (uint32_t)(EFB_W * s_scale), (uint32_t)(EFB_H * s_scale) };
    bi.clearValueCount = 2;
    bi.pClearValues = clears;
    vkCmdBeginRenderPass(f.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    s_passOpen = true;
}

void ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                  VkPipelineStageFlags dstStage)
{
    if (from == to && srcAccess == dstAccess) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(Cur().cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Current layout of the EFB colour image: inside the pass it is the render-pass layout.
bool s_efbColorSampled = false;   // EFB colour content is also in s_colorCopy
bool s_efbDepthSampled = false;   // EFB depth content is also in s_depthCopy
bool s_xfbLayoutPresent = false;  // s_xfb is in PRESENT_SRC_KHR after a present
uint32_t s_drawsAtLastXfb = 0;    // 3D draws since the previous XFB (perf + diagnostics)
int g_dbgNoCull = 0;              // --dbg-nocull (same flag the D3D backend declares)
Frame* s_peekOwner = nullptr;     // frame whose readback the last EFB_Read32 used
bool s_userScreenshot = false;    // F12 / Activity screenshot request

void DrawUtil(VkPipeline pipe, const UtilCB& cb, VkRenderPass pass, VkFramebuffer fb, VkExtent2D extent,
              VkImageView src, VkImageView srcDepth, bool half)
{
    Frame& f = Cur();
    if (f.flushes >= kMaxFlushes || !pipe) return;
    // Constants for this quad.
    const VkDeviceSize slice = (VkDeviceSize)f.flushes * kConstSliceBytes;
    memcpy((uint8_t*)f.cbMap + slice, &cb, sizeof(cb));
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = f.descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_setLayout;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) return;

    VkDescriptorBufferInfo ubo{ f.cb, slice, sizeof(cb) };
    VkDescriptorImageInfo img[2]{};
    img[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img[0].imageView = src ? src : s_dummyView;
    img[0].sampler = half ? s_linearClamp : s_pointClamp;
    img[1] = img[0];
    img[1].imageView = srcDepth ? srcDepth : s_dummyView;

    VkWriteDescriptorSet w[3]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set;
    w[0].dstBinding = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].descriptorCount = 1;
    w[0].pBufferInfo = &ubo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = set;
    w[1].dstBinding = 2;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].descriptorCount = 1;
    w[1].pImageInfo = &img[0];
    w[2] = w[1];
    w[2].dstBinding = 3;
    w[2].pImageInfo = &img[1];
    vkUpdateDescriptorSets(g_vk.device, 3, w, 0, nullptr);
    f.flushes++;

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = pass;
    bi.framebuffer = fb;
    bi.renderArea.extent = extent;
    VkClearValue clear{};
    clear.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLayout, 0, 1, &set, 0, nullptr);
    VkViewport vp{ 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    VkRect2D sc{ { 0, 0 }, extent };
    vkCmdSetViewport(f.cmd, 0, 1, &vp);
    vkCmdSetScissor(f.cmd, 0, 1, &sc);
    vkCmdDraw(f.cmd, 4, 1, 0, 0);
    vkCmdEndRenderPass(f.cmd);
}
}   // namespace

// ================================================================== textures
namespace
{
// Uploads decoded RGBA8 mip levels into a fresh device-local image.
bool UploadTexture(TexEntry& e, uint32_t w, uint32_t h, uint32_t levels, const uint32_t* const* data,
                   const uint32_t* levelW, const uint32_t* levelH)
{
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk.device, &ci, nullptr, &e.image) != VK_SUCCESS) return false;
    if (!Vk_AllocImage(e.image, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &e.mem))
    {
        LOG_ERROR("vulkan: texture upload out of memory ({}x{})", w, h);
        vkDestroyImage(g_vk.device, e.image, nullptr);
        e.image = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = e.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    if (vkCreateImageView(g_vk.device, &vi, nullptr, &e.view) != VK_SUCCESS) return false;

    // One staging buffer for every level (they are copied with one command buffer).
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> offsets(levels);
    for (uint32_t i = 0; i < levels; i++)
    {
        offsets[i] = total;
        total += (VkDeviceSize)levelW[i] * levelH[i] * 4;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMem, &map)) return false;
    for (uint32_t i = 0; i < levels; i++)
        memcpy((uint8_t*)map + offsets[i], data[i], (size_t)levelW[i] * levelH[i] * 4);

    VkCommandBuffer cmd = Vk_BeginOneShot();
    if (!cmd)
    {
        vkDestroyBuffer(g_vk.device, staging, nullptr);
        vkFreeMemory(g_vk.device, stagingMem, nullptr);
        return false;
    }
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = e.image;
    toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toDst);
    std::vector<VkBufferImageCopy> regions(levels);
    for (uint32_t i = 0; i < levels; i++)
    {
        regions[i].bufferOffset = offsets[i];
        regions[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        regions[i].imageExtent = { levelW[i], levelH[i], 1 };
    }
    vkCmdCopyBufferToImage(cmd, staging, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toRead);
    Vk_EndOneShot(cmd);
    vkDestroyBuffer(g_vk.device, staging, nullptr);
    vkFreeMemory(g_vk.device, stagingMem, nullptr);
    e.w = w;
    e.h = h;
    e.hasMips = levels > 1;
    return true;
}

// ------------------------------------------------------------------ texture packs
// RGBA8 replacements from a texture pack (gx/hires.cpp). Their lifetime is owned by that file:
// this table only holds the GPU objects, addressed by the ids it hands out.
struct PackTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t refs = 0;
};
std::unordered_map<uint32_t, PackTexture> s_packTextures;
uint32_t s_nextPackTexture = 1;

}   // namespace

// Creates a sampled texture from RGBA8 pixels and returns its id (0 on failure). The single
// level covers the whole image; texture packs that ship mip chains are reduced to level 0,
// which only costs sharpness on far-away surfaces.
uint32_t Renderer_PackTexture(const uint8_t* rgba, uint32_t w, uint32_t h)
{
    if (!rgba || !w || !h || !g_vk.device) return 0;
    PackTexture t;
    TexEntry scratch;
    const uint32_t* levels[1] = { (const uint32_t*)rgba };
    const uint32_t widths[1] = { w };
    const uint32_t heights[1] = { h };
    if (!UploadTexture(scratch, w, h, 1, levels, widths, heights))
    {
        DestroyTex(scratch);
        return 0;
    }
    t.image = scratch.image;
    t.mem = scratch.mem;
    t.view = scratch.view;
    t.refs = 1;
    const uint32_t id = s_nextPackTexture++;
    s_packTextures[id] = t;
    return id;
}

VkImageView Renderer_PackTextureView(uint32_t id)
{
    auto it = s_packTextures.find(id);
    return it == s_packTextures.end() ? VK_NULL_HANDLE : it->second.view;
}

void Renderer_PackTextureRelease(uint32_t id)
{
    auto it = s_packTextures.find(id);
    if (it == s_packTextures.end()) return;
    if (--it->second.refs) return;
    if (it->second.view) vkDestroyImageView(g_vk.device, it->second.view, nullptr);
    if (it->second.image) vkDestroyImage(g_vk.device, it->second.image, nullptr);
    if (it->second.mem) vkFreeMemory(g_vk.device, it->second.mem, nullptr);
    s_packTextures.erase(it);
}

void Renderer_PackTextureAddRef(uint32_t id)
{
    auto it = s_packTextures.find(id);
    if (it != s_packTextures.end()) it->second.refs++;
}

namespace
{

uint32_t GXTex_Bpp(uint32_t fmt)
{
    static const uint32_t bpp[16] = { 4, 8, 8, 16, 16, 16, 32, 8, 8, 8, 8, 16, 16, 16, 16, 16 };
    return bpp[fmt & 15];
}

// Decodes and caches a GX texture; returns its view (never null: falls back to the dummy).
VkImageView GetTexture(int map, uint32_t* outW, uint32_t* outH, bool* outMips)
{
    *outW = *outH = 1;
    *outMips = false;
    uint32_t mode0 = TexReg(map, 0x80, 0xA0);
    uint32_t mode1 = TexReg(map, 0x84, 0xA4);
    uint32_t img0 = TexReg(map, 0x88, 0xA8);
    uint32_t img3 = TexReg(map, 0x94, 0xB4);
    uint32_t tlutReg = TexReg(map, 0x98, 0xB8);
    uint32_t w = (img0 & 0x3FF) + 1;
    uint32_t h = ((img0 >> 10) & 0x3FF) + 1;
    uint32_t fmt = (img0 >> 20) & 15;
    uint32_t addr = (img3 & 0x00FFFFFF) << 5;

    // An EFB copy written to this RAM address: sample the copy instead of decoding RAM
    // (the RAM contents are whatever the hardware wrote, and the GPU copy is authoritative).
    auto ec = s_efbCopies.find(addr);
    if (ec != s_efbCopies.end())
    {
        TexEntry& e = ec->second;
        if (e.hashedFrame == s_frame || (e.ramSize && e.ramHash == Hash64(PhysPtr(addr), e.ramSize)))
        {
            e.lastFrame = s_frame;
            e.hashedFrame = s_frame;
            *outW = e.w;
            *outH = e.h;
            return e.view ? e.view : s_dummyView;
        }
        DestroyTex(e);
        s_efbCopies.erase(ec);
    }

    uint32_t maxLod = ((mode1 >> 8) & 0xFF) >> 4;
    uint32_t levels = 1;
    if (((mode0 >> 5) & 3) != 0 && maxLod > 0)
    {
        uint32_t lw = w, lh = h;
        while (levels <= maxLod && (lw > 1 || lh > 1))
        {
            lw = std::max(1u, lw / 2);
            lh = std::max(1u, lh / 2);
            levels++;
        }
    }
    uint32_t total = 0;
    {
        uint32_t lw = w, lh = h;
        for (uint32_t i = 0; i < levels; i++)
        {
            total += GXTex_Size(fmt, lw, lh);
            lw = std::max(1u, lw / 2);
            lh = std::max(1u, lh / 2);
        }
    }
    if (!total || (uint64_t)addr + total > 0x2000000ull) return s_dummyView;   // outside MEM1/MEM2

    const uint8_t* src = PhysPtr(addr);
    const bool isCI = fmt == TF_C4 || fmt == TF_C8 || fmt == TF_C14X2;
    const uint64_t key = (uint64_t)addr | ((uint64_t)fmt << 32) | ((uint64_t)w << 36) | ((uint64_t)h << 47) |
                         ((uint64_t)(levels > 1) << 58) | ((uint64_t)isCI << 59);
    uint32_t tlutOff = (tlutReg & 0x3FF) << 9;
    uint32_t tlutFmt = (tlutReg >> 10) & 3;
    uint32_t palSize = fmt == TF_C4 ? 32 : fmt == TF_C8 ? 512 : 0x8000;
    const uint32_t tOff = tlutOff & (uint32_t)(sizeof(s_tmem) - 1);
    const uint8_t* pal = s_tmem + tOff;

    TexEntry& e = s_texCache[key];
    uint64_t dataHash;
    if (isCI && e.dataHashFrame == s_frame)
        dataHash = e.dataHash;
    else
    {
        dataHash = Hash64(src, total);
        if (isCI)
        {
            e.dataHash = dataHash;
            e.dataHashFrame = s_frame;
        }
    }
    uint64_t hash = dataHash;
    if (isCI) hash = Hash64(pal, std::min<uint32_t>(palSize, (uint32_t)sizeof(s_tmem) - tOff), hash) ^ tlutFmt;

    *outW = w;
    *outH = h;
    *outMips = levels > 1;
    if (e.view && e.hash == hash)
    {
        e.lastFrame = s_frame;
        e.hashedFrame = s_frame;
        return e.view;
    }
    DestroyTex(e);

    std::vector<std::vector<uint32_t>> lv(levels);
    std::vector<const uint32_t*> ptrs(levels);
    std::vector<uint32_t> lw_(levels), lh_(levels);
    uint32_t lw = w, lh = h, off = 0;
    for (uint32_t i = 0; i < levels; i++)
    {
        lv[i].resize((size_t)lw * lh);
        GXTex_Decode(lv[i].data(), src + off, lw, lh, fmt, pal, tlutFmt);
        ptrs[i] = lv[i].data();
        lw_[i] = lw;
        lh_[i] = lh;
        off += GXTex_Size(fmt, lw, lh);
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    // A texture pack may replace this texture, at any size (a pack can ship 4x art for a 32x32
    // original). The replacement is cached in the same entry, keyed on the same source, so a
    // scene that keeps switching between two textures only pays for each once.
    if (g_config.texturePack)
    {
        const HiResTexture packTex = HiRes_Find(src, total, isCI ? pal : nullptr,
                                               isCI ? std::min<uint32_t>(palSize, (uint32_t)sizeof(s_tmem) - tOff) : 0,
                                               fmt, w, h, levels > 1);
        if (packTex)
        {
            const VkImageView view = Renderer_PackTextureView(packTex);
            if (view != VK_NULL_HANDLE)
            {
                e.packId = packTex;   // marks view/image/mem as borrowed, see DestroyTex
                e.view = view;
                e.w = w;
                e.h = h;
                e.hasMips = false;
                e.hash = hash;
                e.lastFrame = s_frame;
                e.hashedFrame = s_frame;
                *outW = w;
                *outH = h;
                *outMips = false;
                return view;
            }
            HiRes_Release(packTex);
        }
    }
    if (!UploadTexture(e, w, h, levels, ptrs.data(), lw_.data(), lh_.data())) return s_dummyView;
    e.hash = hash;
    e.lastFrame = s_frame;
    e.hashedFrame = s_frame;
    {
        static int logged = 0;
        if (logged < 32)
        {
            logged++;
            LOG_DEBUG("gx: tex map{} {:08x} {}x{} fmt {} levels {} tlutfmt {}", map, addr, w, h, fmt, levels, tlutFmt);
        }
    }
    return e.view;
}
}   // namespace

// ================================================================== EFB copy / clear / present
namespace
{
void ClearEFB(int x0, int y0, int x1, int y1)
{
    bool colorUpd = (BP(0x41) >> 3) & 1;
    bool alphaUpd = ((BP(0x41) >> 4) & 1) && PixelFormat() == 1;
    bool zUpd = (BP(0x40) >> 4) & 1;
    uint32_t ar = BP(0x4F), gb = BP(0x50);
    UtilCB cb{};
    cb.color[0] = (ar & 0xFF) / 255.0f;
    cb.color[3] = ((ar >> 8) & 0xFF) / 255.0f;
    cb.color[1] = ((gb >> 8) & 0xFF) / 255.0f;
    cb.color[2] = (gb & 0xFF) / 255.0f;
    if (PixelFormat() != 1) cb.color[3] = 1.0f;
    cb.params[0] = (float)(BP(0x51) & 0xFFFFFF) / 16777216.0f;   // depth
    // Full-viewport quad; the scissor limits it to the requested region. Alpha is always
    // written for formats without an alpha channel (the D3D path does the same trick with a
    // second blend state), so a cleared pixel is (r,g,b,1) rather than (r,g,b,0).
    (void)colorUpd;
    (void)alphaUpd;
    (void)zUpd;
    cb.rect[0] = -1.0f;
    cb.rect[1] = 1.0f;
    cb.rect[2] = 1.0f;
    cb.rect[3] = -1.0f;
    cb.params[1] = 0.0f;
    cb.params[2] = 0.0f;
    cb.params[3] = 0.0f;
    cb.iparams[0] = 0;

    BeginEfbPass();
    s_efbColorSampled = false;
    s_efbDepthSampled = false;
    Frame& f = Cur();
    if (f.flushes >= kMaxFlushes) return;
    const VkDeviceSize slice = (VkDeviceSize)f.flushes * kConstSliceBytes;
    memcpy((uint8_t*)f.cbMap + slice, &cb, sizeof(cb));
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = f.descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_setLayout;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) return;
    VkDescriptorBufferInfo ubo{ f.cb, slice, sizeof(cb) };
    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.imageView = s_dummyView;
    img.sampler = s_pointClamp;
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; i++)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &ubo;
    w[1].dstBinding = 2;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &img;
    w[2].dstBinding = 3;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[2].pImageInfo = &img;
    vkUpdateDescriptorSets(g_vk.device, 3, w, 0, nullptr);
    f.flushes++;

    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_clearPipe);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLayout, 0, 1, &set, 0, nullptr);
    VkRect2D sc{ { std::max(0, x0) * s_scale, std::max(0, y0) * s_scale },
                 { (uint32_t)((std::min(EFB_W, x1) - std::max(0, x0)) * s_scale),
                   (uint32_t)((std::min(EFB_H, y1) - std::max(0, y0)) * s_scale) } };
    if ((int32_t)sc.extent.width <= 0 || (int32_t)sc.extent.height <= 0) return;
    vkCmdSetScissor(f.cmd, 0, 1, &sc);
    vkCmdDraw(f.cmd, 4, 1, 0, 0);
    // Leave the scissor in a known state for the next draw.
    vkCmdSetScissor(f.cmd, 0, 1, &s_scissor);
}

void DoEFBCopy();
}   // namespace

// ================================================================== geometry flush
namespace
{
void Flush()
{
    if (!s_ready)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }
    if (s_indices.empty())
    {
        s_verts.clear();
        return;
    }
    uint32_t cull = (BP(0x00) >> 14) & 3;
    if (cull == 3 && s_topo == Topo::Tri)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }

    PipelineKey key{};
    key.cull = s_topo == Topo::Tri && !g_dbgNoCull ? cull : 0;
    key.topo = (uint32_t)s_topo;
    key.blend = BP(0x41) & 0xFFFF;
    key.depth = BP(0x40) & 0x1F;
    BuildVSKey(key.vs);
    BuildPSKey(key.ps);
    VkPipeline pipe = GetPipeline(key);
    if (!pipe)
    {
        s_verts.clear();
        s_indices.clear();
        return;
    }

    Frame& f = Cur();
    BeginEfbPass();
    // The pass has drawn since the snapshot was taken: the copy in s_colorCopy is stale now.
    s_efbColorSampled = false;
    s_efbDepthSampled = false;
    if (f.flushes >= kMaxFlushes)
    {
        LOG_WARN("vulkan: {} draw batches in one frame, dropping the rest", f.flushes);
        s_verts.clear();
        s_indices.clear();
        return;
    }

    // ---- viewport, as in the D3D backend: GX viewport plus the BP 0x59 offset.
    float wd = XFFloat(0x1000 + XF_VIEWPORT + 0);
    float ht = XFFloat(0x1000 + XF_VIEWPORT + 1);
    float xOrig = XFFloat(0x1000 + XF_VIEWPORT + 3);
    float yOrig = XFFloat(0x1000 + XF_VIEWPORT + 4);
    uint32_t so = BP(0x59);
    float xoff = (float)((so & 0x3FF) * 2);
    float yoff = (float)(((so >> 10) & 0x3FF) * 2);
    float vx = xOrig - wd - xoff;
    float vy = yOrig + ht - yoff;
    float vw = 2.0f * wd;
    float vh = -2.0f * ht;
    float flipX = 1.0f, flipY = 1.0f;
    if (vw < 0) { vx += vw; vw = -vw; flipX = -1.0f; }
    if (vh < 0) { vy += vh; vh = -vh; flipY = -1.0f; }
    VkViewport vp{ vx * s_scale, vy * s_scale, vw * s_scale, vh * s_scale, 0.0f, 1.0f };
    vkCmdSetViewport(f.cmd, 0, 1, &vp);

    uint32_t tl = BP(0x20), br = BP(0x21);
    int sx0 = (int)((tl >> 12) & 0x7FF) - (int)xoff, sy0 = (int)(tl & 0x7FF) - (int)yoff;
    int sx1 = (int)((br >> 12) & 0x7FF) - (int)xoff + 1, sy1 = (int)(br & 0x7FF) - (int)yoff + 1;
    VkRect2D sc{ { std::max(0, sx0) * s_scale, std::max(0, sy0) * s_scale },
                 { (uint32_t)((std::min(EFB_W, sx1) - std::max(0, sx0)) * s_scale),
                   (uint32_t)((std::min(EFB_H, sy1) - std::max(0, sy0)) * s_scale) } };
    if ((int32_t)sc.extent.width <= 0) sc.extent.width = 1;
    if ((int32_t)sc.extent.height <= 0) sc.extent.height = 1;
    vkCmdSetScissor(f.cmd, 0, 1, &sc);

    // ---- constants (XF snapshot, then the two stages)
    const VkDeviceSize slice = (VkDeviceSize)f.flushes * kConstSliceBytes;
    memcpy((uint8_t*)f.cbMap + slice, g_gx.xfmem, kXFWords * 4);
    s_xfDirty = false;
    VSConstants* vs = (VSConstants*)((uint8_t*)f.cbMap + slice + kXfBytes);
    PSConstants* ps = (PSConstants*)((uint8_t*)f.cbMap + slice + kXfBytes + kVSConstBytes);
    FillVSConstants(*vs, flipX, flipY);

    uint32_t texW[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }, texH[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    VkImageView views[8];
    VkSampler samplers[8];
    for (int i = 0; i < 8; i++)
    {
        views[i] = s_dummyView;
        samplers[i] = s_pointClamp;
    }
    uint32_t usedMaps = 0;
    for (uint32_t st = 0; st < key.ps.numTevStages; st++)
    {
        uint32_t ord = (key.ps.tevOrder[st / 2] >> ((st & 1) * 12)) & 0xFFF;
        if ((ord >> 6) & 1) usedMaps |= 1u << (ord & 7);
    }
    for (uint32_t is = 0; is < key.ps.numIndStages; is++)
        usedMaps |= 1u << ((key.ps.indOrder >> (6 * is)) & 7);
    if (key.ps.numTexGens == 0) usedMaps = 0;
    for (int m = 0; m < 8; m++)
    {
        if (!(usedMaps & (1u << m))) continue;
        bool mips = false;
        views[m] = GetTexture(m, &texW[m], &texH[m], &mips);
        if (!views[m]) views[m] = s_dummyView;
        samplers[m] = GetSampler(TexReg(m, 0x80, 0xA0), TexReg(m, 0x84, 0xA4), mips);
    }
    FillPSConstants(*ps, texW, texH);

    // ---- geometry
    const VkDeviceSize vbytes = (VkDeviceSize)s_verts.size() * sizeof(HostVertex);
    const VkDeviceSize ibytes = (VkDeviceSize)s_indices.size() * 4;
    if (f.vbPos + vbytes > kVertexBytes || f.ibPos + ibytes > kIndexBytes)
    {
        LOG_WARN("vulkan: vertex ring exhausted, dropping a frame's worth of draws");
        s_verts.clear();
        s_indices.clear();
        return;
    }
    const VkDeviceSize vbOffset = f.vbPos;
    memcpy((uint8_t*)f.vbMap + vbOffset, s_verts.data(), (size_t)vbytes);
    f.vbPos += (vbytes + 255) & ~(VkDeviceSize)255;
    const VkDeviceSize ibOffset = f.ibPos;
    memcpy((uint8_t*)f.ibMap + ibOffset, s_indices.data(), (size_t)ibytes);
    f.ibPos += (ibytes + 255) & ~(VkDeviceSize)255;

    // ---- descriptor set
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = f.descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_setLayout;
    if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) != VK_SUCCESS) return;

    VkDescriptorBufferInfo xfInfo{ f.cb, slice, kXFWords * 4 };
    VkDescriptorBufferInfo vsInfo{ f.cb, slice + kXfBytes, kVSConstBytes };
    VkDescriptorBufferInfo psInfo{ f.cb, slice + kXfBytes + kVSConstBytes, kPSConstBytes };
    VkWriteDescriptorSet w[3 + 8]{};
    for (int i = 0; i < 3; i++)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].pBufferInfo = &xfInfo;
    w[1].dstBinding = 1;
    w[1].pBufferInfo = &vsInfo;
    w[2].dstBinding = 11;
    w[2].pBufferInfo = &psInfo;
    VkDescriptorImageInfo imgs[8]{};
    for (int i = 0; i < 8; i++)
    {
        imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[i].imageView = views[i];
        imgs[i].sampler = samplers[i];
        VkWriteDescriptorSet& dw = w[3 + i];
        dw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dw.dstSet = set;
        dw.dstBinding = 2 + i;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw.descriptorCount = 1;
        dw.pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(g_vk.device, 11, w, 0, nullptr);
    f.flushes++;

    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLayout, 0, 1, &set, 0, nullptr);
    vkCmdBindVertexBuffers(f.cmd, 0, 1, &f.vb, &vbOffset);
    vkCmdBindIndexBuffer(f.cmd, f.ib, ibOffset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(f.cmd, (uint32_t)s_indices.size(), 1, 0, 0, 0);
    s_drawCalls++;
    s_scissor = sc;

    s_verts.clear();
    s_indices.clear();
}
}   // namespace

// ================================================================== XFB and presentation
namespace
{
// The XFB image is re-created when the requested size changes; it is a transfer destination
// and a copy source, never a render target, so presenting costs one blit into the swapchain.
bool EnsureXfb(uint32_t w, uint32_t h)
{
    if (s_xfb && s_xfbW == w && s_xfbH == h) return true;
    if (s_xfbView) vkDestroyImageView(g_vk.device, s_xfbView, nullptr);
    if (s_xfb) vkDestroyImage(g_vk.device, s_xfb, nullptr);
    if (s_xfbMem) vkFreeMemory(g_vk.device, s_xfbMem, nullptr);
    s_xfbView = VK_NULL_HANDLE;
    s_xfb = VK_NULL_HANDLE;
    s_xfbMem = VK_NULL_HANDLE;
    if (!CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, &s_xfb, &s_xfbMem, &s_xfbView))
        return false;
    s_xfbW = w;
    s_xfbH = h;
    s_xfbLayoutPresent = false;
    return true;
}

// Image index acquired for the presentation of this frame (UINT32_MAX: nothing acquired).
uint32_t s_presentIndex = UINT32_MAX;

// Presents the XFB and leaves the frame's command buffer valid again by restarting the
// recording after the blit. Returns false when there is nothing to present (no window, no
// swapchain) so the frame can finish as an offscreen frame instead.
bool PresentXfb()
{
    if (!s_xfbView || !s_xfbW || !s_xfbH) return false;
    uint32_t index = VideoSwapchain_Acquire();
    if (index == UINT32_MAX) return false;
    const VkExtent2D extent = VideoSwapchain_Extent();
    const VkImage swapImage = VideoSwapchain_Image(index);
    if (!extent.width || !extent.height || !swapImage) return false;

    Frame& f = Cur();
    // The XFB was written by a transfer earlier in this command buffer and is the blit's
    // source; the swapchain image came from the acquire semaphore, so its contents are
    // undefined and it only needs to become a transfer destination.
    ImageBarrier(s_xfb, VK_IMAGE_ASPECT_COLOR_BIT,
                 s_xfbLayoutPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);
    ImageBarrier(swapImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // Letterbox: keep the XFB's aspect ratio, integer-scaled when the config asks for it.
    float srcAspect = (float)s_xfbW / (float)s_xfbH;
    float dstAspect = (float)extent.width / (float)extent.height;
    int32_t dw = (int32_t)extent.width, dh = (int32_t)extent.height, dx = 0, dy = 0;
    if (dstAspect > srcAspect)
    {
        dw = (int32_t)((float)extent.height * srcAspect + 0.5f);
        dx = ((int32_t)extent.width - dw) / 2;
    }
    else if (dstAspect < srcAspect)
    {
        dh = (int32_t)((float)extent.width / srcAspect + 0.5f);
        dy = ((int32_t)extent.height - dh) / 2;
    }
    if (g_videoConfig.integerScale)
    {
        int32_t scale = std::max(1, std::min(dw / (int32_t)s_xfbW, dh / (int32_t)s_xfbH));
        dw = (int32_t)s_xfbW * scale;
        dh = (int32_t)s_xfbH * scale;
        dx = ((int32_t)extent.width - dw) / 2;
        dy = ((int32_t)extent.height - dh) / 2;
    }
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { (int32_t)s_xfbW, (int32_t)s_xfbH, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { dx, dy, 0 };
    blit.dstOffsets[1] = { dx + dw, dy + dh, 1 };
    vkCmdBlitImage(f.cmd, s_xfb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    s_xfbLayoutPresent = true;
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = swapImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    }
    s_presentIndex = index;
    return true;
}
}   // namespace

// ================================================================== EFB copy
namespace
{
// Copies the EFB attachments into the snapshot images the util quads sample. Cheap to call
// repeatedly: it does nothing when the snapshot is already current for this frame, and the
// flag is cleared whenever the EFB pass has drawn again.
void EnsureEfbSnapshot()
{
    Frame& f = Cur();
    if (!s_efbColorSampled)
    {
        ImageBarrier(s_efbColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        ImageBarrier(s_colorCopy, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy region{};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.extent = { (uint32_t)(EFB_W * s_scale), (uint32_t)(EFB_H * s_scale), 1 };
        vkCmdCopyImage(f.cmd, s_efbColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_colorCopy,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(s_colorCopy, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ImageBarrier(s_efbColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        s_efbColorSampled = true;
    }
    if (PixelFormat() == 3 && !s_efbDepthSampled)
    {
        // Z-textures read the depth buffer as a 24-bit fixed-point colour, so the depth copy
        // is only made when the guest is actually in a Z-texture pass.
        ImageBarrier(s_efbDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        ImageBarrier(s_depthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy dregion{};
        dregion.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        dregion.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        dregion.extent = { (uint32_t)(EFB_W * s_scale), (uint32_t)(EFB_H * s_scale), 1 };
        vkCmdCopyImage(f.cmd, s_efbDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_depthCopy,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dregion);
        ImageBarrier(s_depthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ImageBarrier(s_efbDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        s_efbDepthSampled = true;
    }
}

// Drops textures that have not been sampled for 600 frames, then enforces the hard caps the
// D3D backend uses so a long session cannot exhaust a phone's memory.
void EvictTextures()
{
    for (auto it = s_texCache.begin(); it != s_texCache.end();)
    {
        if (s_frame - it->second.lastFrame > 600)
        {
            DestroyTex(it->second);
            it = s_texCache.erase(it);
        }
        else ++it;
    }
    for (auto it = s_efbCopies.begin(); it != s_efbCopies.end();)
    {
        if (s_frame - it->second.lastFrame > 600)
        {
            DestroyTex(it->second);
            it = s_efbCopies.erase(it);
        }
        else ++it;
    }
    auto evictCap = [](auto& cache, size_t cap) {
        while (cache.size() > cap)
        {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->second.lastFrame < oldest->second.lastFrame) oldest = it;
            DestroyTex(oldest->second);
            cache.erase(oldest);
        }
    };
    evictCap(s_texCache, 4096);
    evictCap(s_efbCopies, 256);
}

// EFB -> RAM texture or EFB -> XFB, per BP 0x52. The D3D backend copies the EFB into a
// snapshot and draws a quad from it; here the copy reads the live EFB attachment directly
// after ending the render pass, which is one pass restart instead of two full copies.
void DoEFBCopy()
{
    uint32_t ctl = BP(0x52);
    uint32_t src = BP(0x49), size = BP(0x4A);
    int sx = src & 0x3FF, sy = (src >> 10) & 0x3FF;
    int sw = (size & 0x3FF) + 1, sh = ((size >> 10) & 0x3FF) + 1;
    if (sx + sw > EFB_W) sw = EFB_W - sx;
    if (sy + sh > EFB_H) sh = EFB_H - sy;
    if (sw <= 0 || sh <= 0) return;
    const bool toXFB = (ctl >> 14) & 1;
    const bool clear = (ctl >> 11) & 1;
    const bool half = (ctl >> 9) & 1;
    const uint32_t destAddr = (BP(0x4B) & 0xFFFFFF) << 5;

    EndEfbPass();
    EnsureEfbSnapshot();

    const uint32_t tp = (ctl >> 3) & 0xF;
    const uint32_t fmt = (tp >> 1) | ((tp & 1) << 3);
    const bool intensity = (ctl >> 15) & 1;
    const bool depthSrc = PixelFormat() == 3;
    const uint32_t tw = half ? std::max(1, sw / 2) : sw;
    const uint32_t th = half ? std::max(1, sh / 2) : sh;
    const uint32_t rw = tw * s_scale, rh = th * s_scale;

    float u0 = (float)sx / EFB_W, v0 = (float)sy / EFB_H;
    float u1 = (float)(sx + sw) / EFB_W, v1 = (float)(sy + sh) / EFB_H;

    UtilCB cb{};
    cb.rect[0] = -1.0f;
    cb.rect[1] = 1.0f;
    cb.rect[2] = 1.0f;
    cb.rect[3] = -1.0f;
    cb.uv[0] = u0;
    cb.uv[1] = v0;
    cb.uv[2] = u1;
    cb.uv[3] = v1;
    cb.params[2] = 1.0f / EFB_W;
    cb.params[3] = 1.0f / EFB_H;
    cb.iparams[0] = (int32_t)fmt;
    cb.iparams[1] = intensity && fmt < 4 ? 1 : 0;
    cb.iparams[2] = depthSrc ? 1 : 0;
    cb.iparams[3] = half ? 1 : 0;

    if (toXFB)
    {
        const uint32_t w = (uint32_t)sw * s_scale, h = (uint32_t)sh * s_scale;
        if (EnsureXfb(w, h))
        {
            VkFramebuffer fb = VK_NULL_HANDLE;
            VkFramebufferCreateInfo fbi{};
            fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbi.renderPass = s_xfbPass;
            fbi.attachmentCount = 1;
            fbi.pAttachments = &s_xfbView;
            fbi.width = w;
            fbi.height = h;
            fbi.layers = 1;
            if (vkCreateFramebuffer(g_vk.device, &fbi, nullptr, &fb) == VK_SUCCESS)
            {
                // s_xfbPass leaves the image as a blit source, which is exactly what the
                // present needs next, so no further barrier is required here.
                ImageBarrier(s_xfb, VK_IMAGE_ASPECT_COLOR_BIT,
                             s_xfbLayoutPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT, 0,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                s_xfbLayoutPresent = false;
                cb.iparams[0] = 0;   // the XFB is a copy of the EFB's RGBA8, not a format conversion
                cb.iparams[1] = 0;
                cb.iparams[2] = 0;
                DrawUtil(s_xfbPipe, cb, s_xfbPass, fb, { w, h }, s_colorCopyView, s_depthCopyView, half);
                vkDestroyFramebuffer(g_vk.device, fb, nullptr);
            }
        }
        Perf_OnXfbCopy();
        Perf_NoteXfbDraws(s_drawCalls - s_drawsAtLastXfb);
        s_drawsAtLastXfb = s_drawCalls;
        // Occlusion peeks answer from this frame once it has finished on the GPU (never a wait).
        RecordEfbPeekReadback();
        if (clear) ClearEFB(sx, sy, sx + sw, sy + sh);
        // The XFB is the frame: submit it, hand the image to the compositor, and start
        // recording the next frame (the frame slot advances only once its fence is free).
        const bool presented = PresentXfb();
        VkSemaphore wait = presented ? VideoSwapchain_AcquireSemaphore() : VK_NULL_HANDLE;
        VkSemaphore signal = presented ? VideoSwapchain_RenderDoneSemaphore(s_presentIndex) : VK_NULL_HANDLE;
        Perf_OnPresentEnd();
        if (!SubmitFrame(wait, signal, VK_PIPELINE_STAGE_TRANSFER_BIT)) LOG_ERROR("vulkan: frame submission failed");
        if (presented)
        {
            if (!VideoSwapchain_Present(s_presentIndex))
                LOG_DEBUG("vulkan: present failed, swapchain will be re-created");
            g_framesPresented++;
            s_frame++;
            Renderer_OnPresentedFrame();   // screenshot, texture eviction, logs
            if ((s_frame % 300) == 0) EvictTextures();
        }
        s_presentIndex = UINT32_MAX;
        s_frameIdx = (s_frameIdx + 1) % kFramesInFlight;
        BeginFrameCommands();
        Perf_OnPresentBegin();
        return;
    }

    // EFB -> RAM texture: filter the snapshot into the destination image with the copy quad.
    TexEntry& e = s_efbCopies[destAddr];
    if (!e.image || e.w != rw || e.h != rh)
    {
        DestroyTex(e);
        if (!CreateImage(rw, rh, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, &e.image, &e.mem, &e.view))
        {
            s_efbCopies.erase(destAddr);
            if (clear) ClearEFB(sx, sy, sx + sw, sy + sh);
            BeginEfbPass();
            return;
        }
        e.w = rw;
        e.h = rh;
        e.efbCopy = true;
    }
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fbi{};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = s_copyPass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &e.view;
    fbi.width = rw;
    fbi.height = rh;
    fbi.layers = 1;
    if (vkCreateFramebuffer(g_vk.device, &fbi, nullptr, &fb) != VK_SUCCESS)
    {
        if (clear) ClearEFB(sx, sy, sx + sw, sy + sh);
        BeginEfbPass();
        return;
    }
    // s_copyPass discards the previous contents and leaves the copy readable by a sampler.
    ImageBarrier(e.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 0, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    DrawUtil(s_copyPipe, cb, s_copyPass, fb, { rw, rh }, s_colorCopyView, s_depthCopyView, half);
    vkDestroyFramebuffer(g_vk.device, fb, nullptr);
    // The guest may write this RAM buffer again; remember what the copy saw so it is dropped
    // once the bytes change (the same rule the D3D backend uses).
    e.createFrame = s_frame;
    e.ramSize = std::max<uint32_t>(32, tw * th * GXTex_Bpp(fmt) / 8);
    e.ramHash = Hash64(PhysPtr(destAddr), e.ramSize);
    e.hashedFrame = s_frame;
    e.lastFrame = s_frame;
    e.createFrame = s_frame;

    if (clear) ClearEFB(sx, sy, sx + sw, sy + sh);
    BeginEfbPass();
}

}   // namespace

// ================================================================== GX callbacks
void Render_OnCP(uint32_t reg)
{
    // A vertex-format change only affects later draws (the layout is decoded per draw), but
    // the batch's vertices were decoded with the old one, so it has to go out first.
    if (reg >= CP_MATINDEX_A && reg < CP_VCD_HI + 0x10) Flush();
}

void Render_OnXF(uint32_t addr, uint32_t count, bool indexed)
{
    (void)indexed;
    (void)count;
    // Every shader reads XF memory from its own flush's constant slice, so nothing has to be
    // tracked beyond the "the registers changed" flag the flush path reads.
    if (addr < 0x1000) s_xfDirty = true;
}

void Render_OnBP(uint32_t reg, uint32_t oldValue, uint32_t newValue)
{
    if (reg == 0x52)
    {
        DoEFBCopy();
        return;
    }
    if (reg == 0x65)   // TLUT load: copy the palette into TMEM
    {
        uint32_t ramAddr = (g_gx.bp[0x64] & 0xFFFFFF) << 5;
        uint32_t tmemAddr = (newValue & 0x3FF) << 9;
        uint32_t count = ((newValue >> 10) & 0x7FF) * 32;
        if (tmemAddr + count <= sizeof(s_tmem)) memcpy(s_tmem + tmemAddr, PhysPtr(ramAddr), count);
        return;
    }
    if (reg >= 0xE0 && reg <= 0xE7)
    {
        // TEV colour/konst registers: bit 23 selects the konstant register, whose write does
        // not change the colour register the guest thinks it wrote (as in the D3D backend).
        uint32_t idx = (reg - 0xE0) >> 1;
        if ((newValue >> 23) & 1)
        {
            if ((reg & 1) == 0)
            {
                g_tevKonst[idx][0] = newValue & 0xFF;
                g_tevKonst[idx][3] = (newValue >> 12) & 0xFF;
            }
            else
            {
                g_tevKonst[idx][2] = newValue & 0xFF;
                g_tevKonst[idx][1] = (newValue >> 12) & 0xFF;
            }
            g_gx.bp[reg] = oldValue;
        }
        return;
    }
}

void Render_Draw(int prim, int vat, uint32_t count, const uint8_t* data, const VertexLayout& layout)
{
    (void)vat;
    if (!s_ready || !count) return;
    const uint32_t comps = VertexLoader_Components(layout);
    const Topo topo = prim == 5 || prim == 6 ? Topo::Line : prim == 7 ? Topo::Point : Topo::Tri;
    if (!s_indices.empty() && (topo != s_topo || comps != s_batchComponents)) Flush();
    s_topo = topo;
    s_batchComponents = comps;

    const uint32_t base = (uint32_t)s_verts.size();
    s_verts.resize(base + count);
    VertexLoader_Decode(layout, data, count, s_verts.data() + base, s_skip);
    const bool checkSkip = !s_skip.empty();

    const size_t start = s_indices.size();
    s_indices.resize(start + (size_t)count * 3 + 3);
    uint32_t* out = s_indices.data() + start;
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (checkSkip && (s_skip[a] || s_skip[b] || s_skip[c])) return;
        *out++ = base + a;
        *out++ = base + b;
        *out++ = base + c;
    };
    switch (prim)
    {
    case 0: case 1:   // quads
        for (uint32_t i = 0; i + 3 < count; i += 4)
        {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
        break;
    case 2:   // triangles
        for (uint32_t i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2);
        break;
    case 3:   // strip
        for (uint32_t i = 0; i + 2 < count; i++)
        {
            if (i & 1) tri(i + 1, i, i + 2);
            else tri(i, i + 1, i + 2);
        }
        break;
    case 4:   // fan
        for (uint32_t i = 1; i + 1 < count; i++) tri(0, i, i + 1);
        break;
    case 5:   // lines
        for (uint32_t i = 0; i + 1 < count; i += 2)
        {
            *out++ = base + i;
            *out++ = base + i + 1;
        }
        break;
    case 6:   // line strip
        for (uint32_t i = 0; i + 1 < count; i++)
        {
            *out++ = base + i;
            *out++ = base + i + 1;
        }
        break;
    case 7:   // points
        for (uint32_t i = 0; i < count; i++) *out++ = base + i;
        break;
    default: break;
    }
    s_indices.resize((size_t)(out - s_indices.data()));
    // A batch that has grown large is better sent now than grown further: the vertex ring is
    // much smaller than the D3D backend's (a phone cannot spare 32 MiB of vertex memory).
    if (s_verts.size() > 60000) Flush();
}

void Render_Flush() { Flush(); }

// ================================================================== entry points
namespace
{
bool CreatePipeLayout()
{
    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &s_setLayout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(g_vk.device, &ci, nullptr, &layout) != VK_SUCCESS) return false;
    s_pipeLayout = layout;
    return true;
}

void DestroyTextures()
{
    for (auto& kv : s_texCache) DestroyTex(kv.second);
    s_texCache.clear();
    for (auto& kv : s_efbCopies) DestroyTex(kv.second);
    s_efbCopies.clear();
    for (auto& kv : s_samplerCache)
        if (kv.second) vkDestroySampler(g_vk.device, kv.second, nullptr);
    s_samplerCache.clear();
}

void DestroyPipelines()
{
    for (auto& kv : s_pipelines)
        if (kv.second) vkDestroyPipeline(g_vk.device, kv.second, nullptr);
    s_pipelines.clear();
    auto dropPipe = [](VkPipeline& p) {
        if (p) vkDestroyPipeline(g_vk.device, p, nullptr);
        p = VK_NULL_HANDLE;
    };
    dropPipe(s_clearPipe);
    dropPipe(s_copyPipe);
    dropPipe(s_xfbPipe);
    for (auto& kv : s_vsCache)
        if (kv.second.module) vkDestroyShaderModule(g_vk.device, kv.second.module, nullptr);
    s_vsCache.clear();
    for (auto& kv : s_psCache)
        if (kv.second.module) vkDestroyShaderModule(g_vk.device, kv.second.module, nullptr);
    s_psCache.clear();
}
}   // namespace

void Renderer_Init()
{
    s_scale = std::max(1, g_videoConfig.internalScale);
    if (s_scale > 4) s_scale = 4;   // 2560x2112 EFB: beyond this no phone this targets wins

    const bool validation = getenv("SCR_ANDROID_VULKAN_VALIDATION") != nullptr;
    if (!Vk_Init(validation))
    {
        FATAL("Vulkan is not available on this device");
        return;
    }
    LOG_INFO("vulkan: {}", Vk_DeviceSummary());

    if (!CreateSetLayout() || !CreatePipeLayout())
    {
        FATAL("vulkan: descriptor/pipeline layout creation failed");
        return;
    }
    s_cmdPool = g_vk.cmdPool;   // the context owns it; frames allocate from it
    if (!CreateFrameResources() || !CreateEfb() || !CreateRenderPasses() || !CreateSamplers() || !CreateDummyTexture())
    {
        FATAL("vulkan: renderer resource creation failed");
        return;
    }
    if (!s_xfbPass || !s_copyPass)
    {
        FATAL("vulkan: render passes missing");
        return;
    }

    // Util shaders: one vertex stage, two fragment stages (solid fill, EFB copy).
    s_compiler = shaderc_compiler_initialize();
    VkShaderModule utilVS = MakeModule(CompileGLSL(GenerateUtilShaderGLSL(), true, "util_vs"));
    VkShaderModule clearFS = MakeModule(CompileGLSL(GenerateUtilFragmentGLSL(false), false, "util_clear_ps"));
    VkShaderModule copyFS = MakeModule(CompileGLSL(GenerateUtilFragmentGLSL(true), false, "util_copy_ps"));
    if (utilVS && clearFS) s_clearPipe = BuildUtilPipeline(utilVS, clearFS, s_efbPass, s_pipeLayout);
    if (utilVS && copyFS)
    {
        s_copyPipe = BuildUtilPipeline(utilVS, copyFS, s_copyPass, s_pipeLayout);
        s_xfbPipe = BuildUtilPipeline(utilVS, copyFS, s_xfbPass, s_pipeLayout);
    }
    if (utilVS) vkDestroyShaderModule(g_vk.device, utilVS, nullptr);
    if (clearFS) vkDestroyShaderModule(g_vk.device, clearFS, nullptr);
    if (copyFS) vkDestroyShaderModule(g_vk.device, copyFS, nullptr);
    if (!s_clearPipe || !s_copyPipe || !s_xfbPipe)
    {
        FATAL("vulkan: the clear/copy shaders did not compile");
        return;
    }

    s_frameIdx = 0;
    if (!BeginFrameCommands())
    {
        FATAL("vulkan: cannot begin the first frame");
        return;
    }
    s_ready = true;
    Renderer_ResetSamplers();
    LOG_INFO("vulkan: renderer ready, EFB {}x{} ({}x), {} frame slots", EFB_W * s_scale, EFB_H * s_scale, s_scale,
             (int)kFramesInFlight);
}

void Renderer_Shutdown()
{
    if (!g_vk.device) return;
    vkDeviceWaitIdle(g_vk.device);
    s_ready = false;
    DestroyPipelines();
    DestroyTextures();
    if (s_xfbView) vkDestroyImageView(g_vk.device, s_xfbView, nullptr);
    if (s_xfb) vkDestroyImage(g_vk.device, s_xfb, nullptr);
    if (s_xfbMem) vkFreeMemory(g_vk.device, s_xfbMem, nullptr);
    s_xfbView = VK_NULL_HANDLE;
    s_xfb = VK_NULL_HANDLE;
    s_xfbMem = VK_NULL_HANDLE;
    for (int i = 0; i < 2; i++)
    {
        auto drop = [](VkImageView& v, VkImage& img, VkDeviceMemory& mem) {
            if (v) vkDestroyImageView(g_vk.device, v, nullptr);
            if (img) vkDestroyImage(g_vk.device, img, nullptr);
            if (mem) vkFreeMemory(g_vk.device, mem, nullptr);
            v = VK_NULL_HANDLE;
            img = VK_NULL_HANDLE;
            mem = VK_NULL_HANDLE;
        };
        drop(i ? s_depthCopyView : s_colorCopyView, i ? s_depthCopy : s_colorCopy, i ? s_depthCopyMem : s_colorCopyMem);
    }
    {
        if (s_efbFb) vkDestroyFramebuffer(g_vk.device, s_efbFb, nullptr);
        s_efbFb = VK_NULL_HANDLE;
        if (s_efbColorView) vkDestroyImageView(g_vk.device, s_efbColorView, nullptr);
        if (s_efbDepthView) vkDestroyImageView(g_vk.device, s_efbDepthView, nullptr);
        if (s_efbColor) vkDestroyImage(g_vk.device, s_efbColor, nullptr);
        if (s_efbDepth) vkDestroyImage(g_vk.device, s_efbDepth, nullptr);
        if (s_efbColorMem) vkFreeMemory(g_vk.device, s_efbColorMem, nullptr);
        if (s_efbDepthMem) vkFreeMemory(g_vk.device, s_efbDepthMem, nullptr);
        s_efbColorView = s_efbDepthView = VK_NULL_HANDLE;
        s_efbColor = s_efbDepth = VK_NULL_HANDLE;
        s_efbColorMem = s_efbDepthMem = VK_NULL_HANDLE;
    }
    if (s_efbPass) vkDestroyRenderPass(g_vk.device, s_efbPass, nullptr);
    if (s_copyPass) vkDestroyRenderPass(g_vk.device, s_copyPass, nullptr);
    if (s_xfbPass) vkDestroyRenderPass(g_vk.device, s_xfbPass, nullptr);
    s_efbPass = s_copyPass = s_xfbPass = VK_NULL_HANDLE;
    if (s_dummyView) vkDestroyImageView(g_vk.device, s_dummyView, nullptr);
    if (s_dummyImage) vkDestroyImage(g_vk.device, s_dummyImage, nullptr);
    if (s_dummyMem) vkFreeMemory(g_vk.device, s_dummyMem, nullptr);
    s_dummyView = VK_NULL_HANDLE;
    s_dummyImage = VK_NULL_HANDLE;
    s_dummyMem = VK_NULL_HANDLE;
    if (s_linearClamp) vkDestroySampler(g_vk.device, s_linearClamp, nullptr);
    if (s_pointClamp) vkDestroySampler(g_vk.device, s_pointClamp, nullptr);
    s_linearClamp = s_pointClamp = VK_NULL_HANDLE;
    for (auto& f : s_frames)
    {
        auto dropBuf = [](VkBuffer& b, VkDeviceMemory& m, void*& map) {
            if (map) {
                vkUnmapMemory(g_vk.device, m);
                map = nullptr;
            }
            if (b) vkDestroyBuffer(g_vk.device, b, nullptr);
            if (m) vkFreeMemory(g_vk.device, m, nullptr);
            b = VK_NULL_HANDLE;
            m = VK_NULL_HANDLE;
        };
        dropBuf(f.cb, f.cbMem, f.cbMap);
        dropBuf(f.vb, f.vbMem, f.vbMap);
        dropBuf(f.ib, f.ibMem, f.ibMap);
        dropBuf(f.peekColor, f.peekColorMem, f.peekColorMap);
        dropBuf(f.peekDepth, f.peekDepthMem, f.peekDepthMap);
        if (f.descPool) vkDestroyDescriptorPool(g_vk.device, f.descPool, nullptr);
        if (f.fence) vkDestroyFence(g_vk.device, f.fence, nullptr);
        f = Frame{};
    }
    if (s_pipeLayout) vkDestroyPipelineLayout(g_vk.device, s_pipeLayout, nullptr);
    if (s_setLayout) vkDestroyDescriptorSetLayout(g_vk.device, s_setLayout, nullptr);
    s_pipeLayout = VK_NULL_HANDLE;
    s_setLayout = VK_NULL_HANDLE;
    VideoSwapchain_Destroy();
    if (s_compiler)
    {
        shaderc_compiler_release(s_compiler);
        s_compiler = nullptr;
    }
    Vk_Shutdown();
}

void Renderer_ResetSamplers()
{
    if (!g_vk.device) return;
    for (auto& kv : s_samplerCache)
        if (kv.second) vkDestroySampler(g_vk.device, kv.second, nullptr);
    s_samplerCache.clear();
}

void Renderer_RepeatLastXfb()
{
    // A re-present (host refresh faster than the guest's VI rate) must not disturb the frame
    // being recorded, so it gets its own short command buffer instead of the frame's.
    if (!s_xfbView || !s_xfbW || !s_xfbH || !s_ready) return;
    uint32_t index = VideoSwapchain_Acquire();
    if (index == UINT32_MAX) return;
    const VkImage swapImage = VideoSwapchain_Image(index);
    const VkExtent2D extent = VideoSwapchain_Extent();
    if (!swapImage || !extent.width || !extent.height) return;
    VkCommandBuffer cmd = Vk_BeginOneShot();
    if (!cmd) return;
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = swapImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkImageMemoryBarrier x{};
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.oldLayout = s_xfbLayoutPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        x.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.image = s_xfb;
        x.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        x.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        x.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &x);
    }
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { (int32_t)s_xfbW, (int32_t)s_xfbH, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1] = { (int32_t)extent.width, (int32_t)extent.height, 1 };
    vkCmdBlitImage(cmd, s_xfb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    Vk_EndOneShot(cmd);
    s_xfbLayoutPresent = true;
    if (VideoSwapchain_Present(index)) g_framesPresented++;
}

void Renderer_RequestScreenshot()
{
    s_userScreenshot = true;
}

// ================================================================== recording helpers
namespace
{
// PNG/BMP writing would need a decoder the Android build does not ship; the screenshot is
// taken from the XFB through the same readback path the peeks use and written as a BMP (No
// compression, no dependencies) into the user's screenshots directory.
void SaveXfbScreenshot(const fs::path& path)
{
    if (!s_xfb || !s_xfbW || !s_xfbH) return;
    const VkDeviceSize bytes = (VkDeviceSize)s_xfbW * s_xfbH * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging, &mem, &map)) return;
    VkCommandBuffer cmd = Vk_BeginOneShot();
    if (cmd)
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = s_xfbLayoutPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s_xfb;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { s_xfbW, s_xfbH, 1 };
        vkCmdCopyImageToBuffer(cmd, s_xfb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
        Vk_EndOneShot(cmd);   // waits for the queue
    }
    if (FILE* f = fopen(path.string().c_str(), "wb"))
    {
        const uint32_t rowBytes = s_xfbW * 3;
        const uint32_t pad = (4 - rowBytes % 4) % 4;
        const uint32_t imageBytes = (rowBytes + pad) * s_xfbH;
        uint8_t hdr[54] = { 'B', 'M' };
        auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
        put32(2, 54 + imageBytes);
        put32(10, 54);
        put32(14, 40);
        put32(18, s_xfbW);
        put32(22, s_xfbH);
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, imageBytes);
        fwrite(hdr, 1, 54, f);
        std::vector<uint8_t> row(rowBytes + pad, 0);
        const uint8_t* src = (const uint8_t*)map;
        for (int32_t y = (int32_t)s_xfbH - 1; y >= 0; y--)
        {
            const uint8_t* line = src + (size_t)y * s_xfbW * 4;
            for (uint32_t x = 0; x < s_xfbW; x++)
            {
                row[x * 3 + 0] = line[x * 4 + 2];   // BMP is BGR
                row[x * 3 + 1] = line[x * 4 + 1];
                row[x * 3 + 2] = line[x * 4 + 0];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        LOG_INFO("screenshot saved to {}", path.string());
    }
    vkDestroyBuffer(g_vk.device, staging, nullptr);
    vkFreeMemory(g_vk.device, mem, nullptr);
}
}   // namespace

// The frame has been presented: run the per-present bookkeeping (screenshot, texture
// eviction, capture tooling) that the D3D backend keeps in its EFB copy path.
void Renderer_OnPresentedFrame()
{
    namespace fs2 = std::filesystem;
    if (s_userScreenshot)
    {
        s_userScreenshot = false;
        fs2::path dir = Platform_UserDir() / "screenshots";
        std::error_code ec;
        fs2::create_directories(dir, ec);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
        localtime_r(&now, &tm);
        char name[64];
        snprintf(name, sizeof(name), "scr_%04d%02d%02d_%02d%02d%02d.bmp", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        SaveXfbScreenshot(dir / name);
        if (g_config.screenshotKeep > 0)
        {
            std::vector<fs2::path> shots;
            for (const auto& e : fs2::directory_iterator(dir, ec))
                if (!ec && e.is_regular_file() && e.path().extension() == ".bmp") shots.push_back(e.path());
            std::sort(shots.begin(), shots.end(), [](const fs2::path& a, const fs2::path& b) { return a.filename() > b.filename(); });
            for (size_t i = (size_t)g_config.screenshotKeep; i < shots.size(); i++)
            {
                fs2::remove(shots[i], ec);
                ec.clear();
            }
        }
    }
    if ((g_framesPresented % 300) == 0)
        LOG_INFO("vulkan: {} frames, {} pipelines, {} vertex shaders, {} pixel shaders, {} textures, {} EFB copies",
                 g_framesPresented, s_pipelines.size(), s_vsCache.size(), s_psCache.size(), s_texCache.size(),
                 s_efbCopies.size());
}

int Renderer_PrecompileShaders(const std::string& dir)
{
    // Shaders are compiled on demand with shaderc; there is no offline cache to warm on a
    // device, and a phone has no HLSL/SPIR-V toolchain to produce one from.
    (void)dir;
    LOG_INFO("vulkan: shaders are compiled on demand (shaderc); nothing to precompile");
    return 0;
}

// ================================================================== capture tooling
// The desktop build links runtime/debug, which implements the A1/A1b/A1c/A2b/A2c/A3a and
// A2g capture hooks (environment-gated FIFO archives, pose capture, scratch replay). None of
// that belongs on a phone — the archives are tens of megabytes and exist to debug the PC
// backend against the hardware — so the Android build provides the same symbols as no-ops.
// gx_fifo.cpp, perf.cpp, the UI and the audio mod call them unconditionally.

// A1: XF projection capture
bool A1_XfCaptureEnabled() { return false; }
void A1_OnXfbCopy() {}
uint64_t A1_SnapCount() { return 0; }
bool A1_HasPair() { return false; }
bool A1_GetProjectionPair(float*, float*) { return false; }
bool A1_ProjectionChanged() { return false; }
void A1_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }

// A1b: pose capture
bool A1b_PoseCaptureEnabled() { return false; }
void A1b_OnSimFrameEnd() {}
void A1b_OnXfbCopy() {}
void A1b_OnXfPosMtx0(const float*) {}
void A1b_CachePlayerShell(uint32_t) {}
uint64_t A1b_SnapCount() { return 0; }
bool A1b_HasPair() { return false; }
void A1b_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }

// A1c: GX capture
bool A1c_GxCaptureEnabled() { return false; }
void A1c_OnXfLoad(uint32_t, uint32_t, bool) {}
void A1c_OnBodyWorldMtx(uint32_t, const float*, uint32_t) {}
void A1c_OnSimFrameEnd() {}
void A1c_OnXfbCopy() {}
uint64_t A1c_FrameCount() { return 0; }
bool A1c_HasPair() { return false; }
void A1c_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }

// A2b: FIFO archive
bool A2b_FifoCaptureEnabled() { return false; }
void A2b_OnFifoSubmission(const uint8_t*, size_t) {}
void A2b_OnDisplayList(uint32_t, uint32_t) {}
void A2b_OnIndexedXf(uint32_t, uint32_t) {}
void A2b_OnVertexArray(uint32_t, uint32_t, uint32_t, uint32_t) {}
void A2b_OnTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void A2b_OnTlutRam(uint32_t, uint32_t) {}
void A2b_OnTmemWindow(uint32_t, uint32_t, const uint8_t*) {}
void A2b_NoteDraw() {}
void A2b_OnSimFrameEnd() {}
void A2b_OnXfbCopy() {}
void A2b_OnPassEntryEfb() {}
uint32_t A2b_CountEfbCopiesInCmd(const uint8_t*, uint32_t) { return 0; }
uint64_t A2b_FrameCount() { return 0; }
bool A2b_HasPair() { return false; }
bool A2b_CloneCurrentPacket(A2bArchivePacket&) { return false; }
bool A2b_ClonePreviousPacket(A2bArchivePacket&) { return false; }
uint32_t A2b_GetCrossEfbEdgeCount() { return 0; }
bool A2b_GetCrossEfbEdge(uint32_t, A2bCrossEfbEdge&) { return false; }
void A2b_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }
bool A2b_ExportLastFrame(const char*) { return false; }
uint32_t A2b_FindFirstBp52(const uint8_t*, uint32_t) { return 0; }

// A2c / A2d scratch replay and EFB comparison: the Android backend has no second target set,
// so every entry point reports "not available" instead of pretending to run.
bool A2c_ScratchReplayEnabled() { return false; }
bool A2c_ClosureCaptureEnabled() { return false; }
void A2c_OnDraw(int, uint32_t, const uint8_t*, uint32_t) {}
void A2c_OnTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void A2c_OnTlutRam(uint32_t, uint32_t) {}
void A2c_OnSimFrameEnd() {}
void A2c_OnXfbCopy() {}
void A2c_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }
bool Render_ScratchBeginFromIncoming(int, uint32_t*, uint32_t*) { return false; }
bool Render_ScratchBeginFromIncomingEx(int, bool, uint32_t*, uint32_t*) { return false; }
bool Render_ScratchBeginCleared(uint32_t*, uint32_t*) { return false; }
void Render_ScratchDropEfbCopies() {}
uint32_t Render_ScratchEfbCopyCount() { return 0; }
bool Render_ScratchHasEfbCopy(uint32_t) { return false; }
uint64_t Render_ScratchEfbCopyGen(uint32_t) { return 0; }
uint32_t Render_HashLiveEfbCopy(uint32_t) { return 0; }
uint32_t Render_HashScratchEfbCopy(uint32_t) { return 0; }
void Render_SetScratchSkipEfbTexCopy(bool) {}
bool Render_ScratchSkipEfbTexCopy() { return false; }
uint32_t Render_ScratchEndHash() { return 0; }
uint32_t Render_ScratchEndDepthHash() { return 0; }
uint32_t Render_HashScratchColor() { return 0; }
uint32_t Render_HashScratchDepth() { return 0; }
uint32_t Render_HashLiveEfb() { return 0; }
uint32_t Render_HashLiveDepth() { return 0; }
uint32_t Render_HashScratchBlack() { return 0; }
bool Render_CompareLiveVsScratch(RenderEfbCompare&, bool) { return false; }
int Render_CompareBeginAsync(bool) { return 0; }
bool Render_ComparePoll(int, RenderEfbCompare&, bool* stillPending)
{
    if (stillPending) *stillPending = false;
    return false;
}
void Render_CompareCancel(int) {}
uint32_t Render_CompareQueueDepth() { return 0; }

// A3a: draw attribute capture
bool A3a_DrawAttrEnabled() { return false; }
bool A3a_MatrixOverrideEnabled() { return false; }
void A3a_OnXfbCopy() {}
void A3a_OnSimFrameEnd() {}
void A3a_OnXfLoadIsolation(uint32_t, uint32_t) {}
bool A3a_ApplyMatrixOverride() { return false; }
void A3a_ResetOverrideStats() {}
void A3a_NoteReplayHashes(uint32_t, uint32_t, bool) {}
void A3a_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }
uint8_t A3a_OverrideRow() { return 0; }
uint32_t A3a_OverrideMode() { return 0; }
void A3a_SuspendOverride(bool) {}
bool A3a_OverrideSuspended() { return false; }
void A3a_OnDraw(int, int, uint32_t, const HostVertex*, uint32_t, const VertexLayout&) {}

// A2g: natural-trace counters (used by the overlay and the disc/stage hint hooks)
bool A2gNat_Enabled() { return false; }
void A2gNat_OnInvalidate(uint32_t, uint64_t, bool) {}
void A2gNat_OnPresent() {}
void A2gNat_OnGuestFrame() {}
void A2gNat_OnStageHint(const char*, const char*) {}
void A2gNat_OnDiscPathHint(const char*) {}
uint32_t A2gNat_GetEdgeCount() { return 0; }
bool A2gNat_GetEdge(uint32_t, A2gNatEdge&) { return false; }
void A2gNat_OverlayLine(char* buf, size_t bufSize) { if (bufSize) buf[0] = 0; }
void A2gNat_LogSummary(const char*) {}

// The renderer's own replay-isolation contract (gx_state.h): the Android backend keeps one
// EFB, so isolation is a no-op that reports the live state as the scratch state.
void Render_SetReplayIsolation(bool) {}
bool Render_ReplayIsolationActive() { return false; }
void Render_ForceXfDirty() { s_xfDirty = true; }

// ================================================================== EFB peeks
// The guest polls single EFB pixels for occlusion tests (laser flares, particle fades).
// Reading the EFB on demand would stall the GPU thread for a whole frame, so each frame's
// XFB copy leaves a colour (and occasionally depth) readback in flight and the peeks answer
// from the previous frame's result. One frame of lag is invisible for an occlusion test and
// keeps audio and input timing intact.
void RecordEfbPeekReadback()
{
    Frame& f = Cur();
    if (!f.peekColor)
    {
        if (!CreateBuffer((VkDeviceSize)EFB_W * EFB_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f.peekColor, &f.peekColorMem,
                          &f.peekColorMap))
            return;
        if (!CreateBuffer((VkDeviceSize)EFB_W * EFB_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f.peekDepth, &f.peekDepthMem,
                          &f.peekDepthMap))
            return;
    }
    // The snapshot copy above already brought the colour image back to the attachment layout;
    // copying it into a host-visible buffer costs one more transfer.
    ImageBarrier(s_efbColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { (uint32_t)(EFB_W * s_scale), (uint32_t)(EFB_H * s_scale), 1 };
    vkCmdCopyImageToBuffer(f.cmd, s_efbColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.peekColor, 1, &region);
    if (f.peekDepth && Vk_FindDepthFormat() != VK_FORMAT_UNDEFINED)
    {
        ImageBarrier(s_efbDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy dregion{};
        dregion.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        dregion.imageExtent = { (uint32_t)(EFB_W * s_scale), (uint32_t)(EFB_H * s_scale), 1 };
        vkCmdCopyImageToBuffer(f.cmd, s_efbDepth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.peekDepth, 1, &dregion);
        ImageBarrier(s_efbDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    }
    ImageBarrier(s_efbColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    f.peekPending = true;
}

uint32_t EFB_Read32(uint32_t ea)
{
    const uint32_t x = (ea >> 2) & 0x3FF, y = (ea >> 12) & 0x3FF;
    const bool depth = (ea & 0x00400000) != 0;
    if (x >= EFB_W || y >= EFB_H) return depth ? 0x00FFFFFFu : 0xFF000000u;
    if (!s_ready) return depth ? 0x00FFFFFFu : 0xFF000000u;   // nothing rendered yet

    // The most recent finished frame whose readback is already visible (never wait: a stall
    // here delays interrupts, which the game turns into audio dropouts).
    for (int i = 0; i < 2; i++)
    {
        Frame& f = s_frames[(s_frameIdx + kFramesInFlight - 1 - i) % kFramesInFlight];
        if (!f.peekPending || !f.peekColorMap) continue;
        if (vkGetFenceStatus(g_vk.device, f.fence) != VK_SUCCESS) continue;
        f.peekPending = false;
        s_peekOwner = &f;
        break;
    }
    Frame* f = s_peekOwner;
    if (!f || !f->peekColorMap) return depth ? 0x00FFFFFFu : 0xFF000000u;

    const uint32_t sx = x * s_scale + s_scale / 2, sy = y * s_scale + s_scale / 2;
    const size_t index = (size_t)sy * (EFB_W * s_scale) + sx;
    if (depth)
    {
        if (!f->peekDepthMap) return 0x00FFFFFFu;
        float d;
        memcpy(&d, (const uint8_t*)f->peekDepthMap + index * 4, 4);
        if (Vk_FindDepthFormat() == VK_FORMAT_D24_UNORM_S8_UINT ||
            Vk_FindDepthFormat() == VK_FORMAT_D32_SFLOAT_S8_UINT)
            return ((const uint32_t*)f->peekDepthMap)[index] & 0x00FFFFFFu;
        return (uint32_t)std::clamp(d * 16777216.0f, 0.0f, 16777215.0f);
    }
    const uint32_t c = ((const uint32_t*)f->peekColorMap)[index];   // R,G,B,A bytes
    const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    const uint32_t a = PixelFormat() == 1 ? (c >> 24) : 0xFF;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void EFB_Write32(uint32_t, uint32_t) {}
