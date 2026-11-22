#pragma once
#include <cstdint>
#include <string>

// The texture a replacement resolves to is a backend object: a D3D11 shader resource view on
// the desktop, a texture id from the Vulkan backend on Android (0 = none).
struct ID3D11ShaderResourceView;
#ifdef SCR_ANDROID
using HiResTexture = uint32_t;
#else
using HiResTexture = ID3D11ShaderResourceView*;
#endif

// Dolphin-compatible custom texture packs: files named
//   tex1_<w>x<h>[_m]_<xxh64 of data>[_<xxh64 of used palette>|_$]_<fmt>.png/.dds
// anywhere under <pack dir>/<game ID> (the layout of Dolphin's Load/Textures), e.g. RENE8P.
void HiRes_Init(const std::string& configuredDir);
bool HiRes_Enabled();
size_t HiRes_LoadedCount();
// Drop HD replacements unused for a long time / over a hard cap (VRAM bound on long sessions).
void HiRes_TickEvict(uint32_t frame);

// Looks up a replacement for a texture about to be decoded. `data` covers the base
// level, `tlut` the 512-byte-aligned TMEM palette (CI formats). Returns a texture the caller
// owns (release it with HiRes_Release), or none.
HiResTexture HiRes_Find(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize,
                        uint32_t fmt, uint32_t w, uint32_t h, bool mipmapped);
// Drops one reference to a texture HiRes_Find returned.
void HiRes_Release(HiResTexture texture);

// --dump-textures: writes every decoded texture as <exe>/dump/textures/<game ID>/<name>.png
extern bool g_dumpTextures;
void HiRes_Dump(const uint8_t* data, uint32_t dataSize, const uint8_t* tlut, uint32_t tlutSize, uint32_t fmt, uint32_t w, uint32_t h,
                bool mipmapped, const uint32_t* rgba);
