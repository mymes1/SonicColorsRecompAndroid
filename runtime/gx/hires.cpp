// Custom texture packs in Dolphin's format (see hires.h).
#include "hires.h"
#include "video.h"
#include "texture_decode.h"
#include "../log.h"
#include "../region.h"
#include "../platform/compat.h"
#include "../platform/image.h"
#include "../platform/platform.h"
#ifdef SCR_ANDROID
#include "../platform/android/renderer_vk.h"   // Renderer_PackTexture* (Vulkan texture ids)
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#ifndef __ANDROID__
#include <format>   // on Android, platform/compat.h provides it
#endif
#include <fstream>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

#define FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

// File-scope so HiRes_TickEvict / HiRes_LoadedCount can manage VRAM across long sessions.
static std::unordered_map<std::string, fs::path> s_files;   // lowercase name -> file
struct LoadedTex
{
    HiResTexture tex = HiResTexture{};   // unset = failed load (negative cache)
    uint32_t lastFrame = 0;
};
static std::unordered_map<std::string, LoadedTex> s_loaded;
static uint32_t s_hiresFrame = 1;

namespace
{
// ---------------------------------------------------------------- XXH64 (seed 0 use)
constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                   P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t Rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
inline uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint64_t Round(uint64_t acc, uint64_t in) { acc += in * P2; acc = Rotl(acc, 31); return acc * P1; }
inline uint64_t Merge(uint64_t acc, uint64_t v) { acc ^= Round(0, v); return acc * P1 + P4; }

uint64_t XXH64(const uint8_t* p, size_t len, uint64_t seed = 0)
{
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32)
    {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 32;
        do
        {
            v1 = Round(v1, Rd64(p)); v2 = Round(v2, Rd64(p + 8)); v3 = Round(v3, Rd64(p + 16)); v4 = Round(v4, Rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = Merge(h, v1); h = Merge(h, v2); h = Merge(h, v3); h = Merge(h, v4);
    }
    else
        h = seed + P5;
    h += len;
    while (p + 8 <= end) { h ^= Round(0, Rd64(p)); h = Rotl(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= (uint64_t)Rd32(p) * P1; h = Rotl(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p++) * P5; h = Rotl(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

void Scan(const fs::path& dir)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string ext = Lower(it->path().extension().string());
        if (ext != ".png" && ext != ".dds") continue;
        std::string stem = Lower(it->path().stem().string());
        if (stem.rfind("tex1_", 0) != 0 || stem.find("_mip") != std::string::npos) continue;
        // .dds preferred over .png when both exist
        auto [pos, inserted] = s_files.emplace(stem, it->path());
        if (!inserted && ext == ".dds") pos->second = it->path();
    }
}

// A decoded image: RGBA8 pixels plus the source's dimensions. Block-compressed DDS files are
// expanded here on Android, where there is no driver-side BC support to rely on; on the desktop
// the compressed blocks are handed to D3D11 untouched.
struct Image
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> pixels;   // RGBA8, rows top to bottom
};

// ---- BC1/BC2/BC3 block decoding (DDS files in texture packs use these heavily).
// BC7 is rejected rather than half-decoded: it has 8 modes with their own partition tables, and
// getting one wrong shows up as corrupted art that is far harder to notice than a load failure.
void DecodeBC1Block(const uint8_t* src, bool allowTransparent, uint8_t* out)
{
    const uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
    const uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));
    auto expand = [](uint16_t c, uint8_t* rgb) {
        const int r5 = c >> 11, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        rgb[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        rgb[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        rgb[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    };
    uint8_t colour[4][3];
    uint8_t alpha[4] = { 255, 255, 255, 255 };
    expand(c0, colour[0]);
    expand(c1, colour[1]);
    if (c0 > c1 || !allowTransparent)
    {
        // Four-colour mode: two interpolated colours, no transparency.
        for (int i = 0; i < 3; i++)
        {
            colour[2][i] = (uint8_t)((2 * colour[0][i] + colour[1][i]) / 3);
            colour[3][i] = (uint8_t)((colour[0][i] + 2 * colour[1][i]) / 3);
        }
    }
    else
    {
        // Three-colour mode: the midpoint plus a transparent black.
        for (int i = 0; i < 3; i++)
        {
            colour[2][i] = (uint8_t)((colour[0][i] + colour[1][i]) / 2);
            colour[3][i] = 0;
        }
        alpha[2] = 255;
        alpha[3] = 0;
    }
    uint32_t bits = (uint32_t)src[4] | ((uint32_t)src[5] << 8) | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    for (int i = 0; i < 16; i++)
    {
        const int idx = (int)((bits >> (2 * i)) & 3);
        out[i * 4 + 0] = colour[idx][0];
        out[i * 4 + 1] = colour[idx][1];
        out[i * 4 + 2] = colour[idx][2];
        out[i * 4 + 3] = alpha[idx];
    }
}

// BC4: one channel, used both for BC3's alpha and on its own. 8-byte block, 3-bit indices.
void DecodeBC4Block(const uint8_t* src, uint8_t* out)
{
    const uint8_t a0 = src[0], a1 = src[1];
    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1)
    {
        for (int i = 1; i <= 6; i++) pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
    }
    else
    {
        for (int i = 1; i <= 4; i++) pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)src[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
}

void DecodeBCBlock(const uint8_t* src, size_t size, uint32_t format, uint32_t w, uint32_t h, Image& img)
{
    // format: 1 = BC1, 2 = BC2, 3 = BC3.
    const uint32_t blockBytes = (format == 1) ? 8 : 16;
    const uint32_t blocksX = (w + 3) / 4, blocksY = (h + 3) / 4;
    if (size < (size_t)blocksX * blocksY * blockBytes) return;
    img.pixels.assign((size_t)w * h * 4, 255);
    std::vector<uint8_t> block(64);
    for (uint32_t by = 0; by < blocksY; by++)
        for (uint32_t bx = 0; bx < blocksX; bx++)
        {
            const uint8_t* s = src + ((size_t)by * blocksX + bx) * blockBytes;
            if (format == 1)
                DecodeBC1Block(s, true, block.data());
            else if (format == 2)
            {
                DecodeBC1Block(s + 8, false, block.data());
                for (int i = 0; i < 16; i++) block[i * 4 + 3] = (uint8_t)((s[i / 2] >> ((i & 1) * 4)) & 15) * 17;
            }
            else if (format == 3)
            {
                DecodeBC1Block(s + 8, false, block.data());
                uint8_t alpha[16];
                DecodeBC4Block(s, alpha);
                for (int i = 0; i < 16; i++) block[i * 4 + 3] = alpha[i];
            }
            for (int y = 0; y < 4; y++)
            {
                const uint32_t py = by * 4 + y;
                if (py >= h) break;
                for (int x = 0; x < 4; x++)
                {
                    const uint32_t px = bx * 4 + x;
                    if (px >= w) break;
                    memcpy(img.pixels.data() + ((size_t)py * w + px) * 4, block.data() + (y * 4 + x) * 4, 4);
                }
            }
        }
}

bool ReadDDS(const fs::path& path, Image& img)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return false;
    auto u32 = [&](size_t o) { return Rd32(d.data() + o); };
    img.h = u32(12);
    img.w = u32(16);
    const uint32_t pfFlags = u32(80), fourcc = u32(84), bpp = u32(88);
    size_t off = 128;
    uint32_t format = 0;   // 1 BC1, 2 BC2, 3 BC3, 0 = uncompressed RGBA
    if (pfFlags & 4)
    {
        if (fourcc == FOURCC('D', 'X', 'T', '1')) format = 1;
        else if (fourcc == FOURCC('D', 'X', 'T', '3')) format = 2;
        else if (fourcc == FOURCC('D', 'X', 'T', '5')) format = 3;
        else if (fourcc == FOURCC('D', 'X', '1', '0') && d.size() >= 148)
        {
            const uint32_t dxgi = u32(128);
            if (dxgi >= 95 && dxgi <= 99) return false;   // BC7: see the note above
            else if (dxgi >= 70 && dxgi <= 72) format = 1;
            else if (dxgi >= 73 && dxgi <= 75) format = 2;
            else if (dxgi >= 76 && dxgi <= 78) format = 3;
            else if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 91) format = 0;
            else return false;
            off = 148;
        }
        else return false;
    }
    else if ((pfFlags & 0x40) && bpp == 32)
        format = 0;
    else
        return false;

    if (!img.w || !img.h) return false;
    const size_t size = (size_t)img.w * img.h * 4;
    if (format != 0)
    {
        DecodeBCBlock(d.data() + off, d.size() - off, format, img.w, img.h, img);
        return !img.pixels.empty();
    }
    // Uncompressed 32-bit: the channel masks say whether the bytes are RGBA or BGRA.
    if (off + size > d.size()) return false;
    const bool bgra = u32(92) == 0x00FF0000u && u32(96) == 0x0000FF00u;
    auto convert = [bgra](const uint8_t* in, uint8_t* out) {
        out[0] = bgra ? in[2] : in[0];
        out[1] = in[1];
        out[2] = bgra ? in[0] : in[2];
        out[3] = in[3];
    };
    img.pixels.resize(size);
    for (size_t i = 0; i < size; i += 4) convert(d.data() + off + i, img.pixels.data() + i);
    return true;
}

#ifndef SCR_ANDROID
// The desktop backend takes the pixels straight into a D3D11 texture, optionally asking the driver
// to build the mip chain, so nothing is decoded in software there.
ID3D11ShaderResourceView* MakeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const std::vector<std::vector<uint8_t>>& levels,
                                      const std::vector<uint32_t>& pitches, bool genMips)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = genMips ? 0 : (UINT)levels.size();
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (genMips ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = genMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    ID3D11Texture2D* tex = nullptr;
    if (genMips)
    {
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
        g_ctx->UpdateSubresource(tex, 0, nullptr, levels[0].data(), pitches[0], 0);
    }
    else
    {
        std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
        for (size_t i = 0; i < levels.size(); i++) init[i] = { levels[i].data(), pitches[i], 0 };
        if (FAILED(g_device->CreateTexture2D(&td, init.data(), &tex))) return nullptr;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    g_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (genMips && srv) g_ctx->GenerateMips(srv);
    return srv;
}
#endif

// A fresh texture, or none: pointers and ids cannot be tested the same way.
inline bool IsSet(HiResTexture t)
{
#ifdef SCR_ANDROID
    return t != 0;
#else
    return t != nullptr;
#endif
}

// Turns decoded pixels into a texture for the active backend.
HiResTexture Upload(const Image& img, bool mipmapped)
{
    if (!img.w || !img.h || img.pixels.empty()) return HiResTexture{};
#ifdef SCR_ANDROID
    (void)mipmapped;   // the Vulkan path samples one level; pack mips would need a chain upload
    return Renderer_PackTexture(img.pixels.data(), img.w, img.h);
#else
    std::vector<std::vector<uint8_t>> lv(1);
    lv[0] = img.pixels;
    return MakeTexture(img.w, img.h, DXGI_FORMAT_R8G8B8A8_UNORM, lv, { img.w * 4 }, mipmapped);
#endif
}

void Release(HiResTexture& tex)
{
    if (IsSet(tex))
    {
#ifdef SCR_ANDROID
        Renderer_PackTextureRelease(tex);
#else
        tex->Release();
#endif
    }
    tex = HiResTexture{};
}

HiResTexture LoadFile(const fs::path& path, bool mipmapped)
{
    Image img;
    if (Lower(path.extension().string()) == ".dds")
    {
        if (!ReadDDS(path, img)) return HiResTexture{};
    }
    else
    {
        std::vector<uint8_t> rgba;
        if (!Image_LoadPNG(path, img.w, img.h, rgba)) return HiResTexture{};
        img.pixels = std::move(rgba);
    }
    return Upload(img, mipmapped);
}
}   // namespace

size_t HiRes_LoadedCount() { return s_loaded.size(); }

void HiRes_TickEvict(uint32_t frame)
{
    s_hiresFrame = frame ? frame : 1;
    if ((frame % 300u) != 0u) return;
    constexpr uint32_t kAge = 1200;       // unused for ~20s at 60 XFB/s
    constexpr size_t kHardCap = 512;
    for (auto it = s_loaded.begin(); it != s_loaded.end();)
    {
        if (s_hiresFrame - it->second.lastFrame > kAge)
        {
            Release(it->second.tex);
            it = s_loaded.erase(it);
        }
        else
            ++it;
    }
    while (s_loaded.size() > kHardCap)
    {
        auto oldest = s_loaded.begin();
        for (auto it = s_loaded.begin(); it != s_loaded.end(); ++it)
            if (it->second.lastFrame < oldest->second.lastFrame) oldest = it;
        Release(oldest->second.tex);
        s_loaded.erase(oldest);
    }
}

void HiRes_Init(const std::string& configuredDir)
{
    std::vector<fs::path> roots;
    if (!configuredDir.empty())
        roots.push_back(configuredDir);
    else
    {
        fs::path user = Platform_UserDir();   // the exe directory on Windows
        roots.push_back(user / "textures");
        roots.push_back(user / "game" / "textures");
        roots.push_back(user / ".." / "game" / "textures");
        if (fs::path dolphin = Platform_DolphinTextureDir(); !dolphin.empty()) roots.push_back(dolphin);
    }
    // the installed release's pack (Dolphin's folder names: game ID, its first four or three
    // characters); else another release's: the textures are the same
    std::error_code ec;
    std::string id = Region_Current().gameId;
    std::vector<std::string> ids = { id, id.substr(0, 4), id.substr(0, 3) };
    for (auto& r : kReleases)
        if (r.gameId != id) ids.push_back(r.gameId);
    for (size_t own = 3, i = 0; i < ids.size() && (i < own || s_files.empty()); i++)
        for (auto& r : roots)
            if (fs::is_directory(r / ids[i], ec)) Scan(r / ids[i]);
    if (s_files.empty()) return;
    Image_Init();
    LOG_INFO("texture pack: {} replacement textures", s_files.size());
}

bool HiRes_Enabled() { return !s_files.empty(); }

// Dolphin's texture names for this texture: [0] exact, [1] palette wildcard (CI only).
static void MakeNames(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                      bool mipmapped, std::string names[2])
{
    // Palette range actually referenced, exactly as Dolphin computes it (including its
    // byte-swap quirk for C14X2).
    uint32_t palSize = fmt == TF_C4 ? 32 : fmt == TF_C8 ? 512 : fmt == TF_C14X2 ? 32768 : 0;
    uint64_t tlutHash = 0;
    if (palSize)
    {
        uint32_t mn = 0xFFFF, mx = 0;
        if (fmt == TF_C4)
            for (uint32_t i = 0; i < dataSize; i++) { mn = std::min({ mn, data[i] & 15u, (uint32_t)data[i] >> 4 }); mx = std::max({ mx, data[i] & 15u, (uint32_t)data[i] >> 4 }); }
        else if (fmt == TF_C8)
            for (uint32_t i = 0; i < dataSize; i++) { mn = std::min(mn, (uint32_t)data[i]); mx = std::max(mx, (uint32_t)data[i]); }
        else
            for (uint32_t i = 0; i < dataSize; i += 2) { uint32_t v = _byteswap_ushort(data[i]) & 0x3FFF; mn = std::min(mn, v); mx = std::max(mx, v); }
        uint32_t used = 2 * (mx + 1 - mn);
        if (2 * mn + used > tlutSize) return;
        tlutHash = XXH64(tlut + 2 * mn, used);
    }
    std::string base = std::format("tex1_{}x{}{}_{:016x}", w, h, mipmapped ? "_m" : "", XXH64(data, dataSize));
    if (palSize)
    {
        names[0] = std::format("{}_{:016x}_{}", base, tlutHash, fmt);
        names[1] = std::format("{}_$_{}", base, fmt);
    }
    else
        names[0] = std::format("{}_{}", base, fmt);
}

HiResTexture HiRes_Find(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize,
                        uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped)
{
    std::string names[2];
    MakeNames(data, dataSize, tlut, tlutSize, fmt, w, h, mipmapped, names);
    for (auto& n : names)
    {
        if (n.empty()) continue;
        auto f = s_files.find(n);
        if (f == s_files.end()) continue;
        auto l = s_loaded.find(n);
        if (l == s_loaded.end())
        {
            const HiResTexture tex = LoadFile(f->second, mipmapped);
            if (!IsSet(tex)) LOG_WARN("texture pack: failed to load {}", f->second.string());
            l = s_loaded.emplace(n, LoadedTex{ tex, s_hiresFrame }).first;
        }
        l->second.lastFrame = s_hiresFrame;
        if (IsSet(l->second.tex))
        {
#ifdef SCR_ANDROID
            Renderer_PackTextureAddRef(l->second.tex);
#else
            l->second.tex->AddRef();
#endif
        }
        return l->second.tex;
    }
    return HiResTexture{};
}

void HiRes_Release(HiResTexture tex)
{
    if (!IsSet(tex)) return;
#ifdef SCR_ANDROID
    Renderer_PackTextureRelease(tex);
#else
    tex->Release();
#endif
}

bool g_dumpTextures = false;

void HiRes_Dump(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                bool mipmapped, const uint32_t* rgba)
{
    static std::unordered_map<std::string, bool> done;
    static fs::path dir;
    if (dir.empty())
    {
        dir = Platform_UserDir() / "dump" / "textures" / Region_Current().gameId;
        std::error_code ec;
        fs::create_directories(dir, ec);
        Image_Init();
    }
    std::string names[2];
    MakeNames(data, dataSize, tlut, tlutSize, fmt, w, h, mipmapped, names);
    if (names[0].empty() || done[names[0]]) return;
    done[names[0]] = true;
    Image_SavePNG(dir / (names[0] + ".png"), w, h, rgba);
}
