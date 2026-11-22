// PNG codec for the Android build: texture packs, texture dumps and the installer's preview.
//
// Android has neither SDL3 nor WIC, and libpng/zlib are not part of the NDK's stable API surface,
// so the decoder and encoder live here (the DEFLATE stage is platform/android/deflate.cpp, shared
// with the ZIP reader). Scope is deliberately the subset a texture pack uses:
// 8-bit PNGs (colour types 0/2/3/4/6, not interlaced) in, RGBA8 out; RGBA8 back out as a valid
// PNG using stored deflate blocks. Anything else reports failure instead of guessing.
//
// Nothing here is on a per-frame path: packs are read once at startup and dumps are written
// behind a flag, so clarity beats speed.
#include "../image.h"
#include "deflate.h"   // Inflate_Zlib: the DEFLATE decoder, shared with the ZIP reader

#include <cstring>

namespace
{
// ================================================================== PNG chunks
uint32_t Crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t ReadBE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// One pass over the chunk stream; fills in whatever the caller asked for.
struct PngInfo
{
    uint32_t width = 0, height = 0;
    int bitDepth = 0, colourType = -1, interlace = 0;
    std::vector<uint8_t> palette;   // PLTE, RGB triples
    std::vector<uint8_t> paletteAlpha;   // tRNS for colour type 3
    std::vector<uint8_t> idat;
    bool sawIhdr = false, sawIend = false;
};

bool ParseChunks(const uint8_t* data, size_t size, PngInfo& info)
{
    static const uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (size < 8 || memcmp(data, kSig, 8) != 0) return false;
    size_t pos = 8;
    while (pos + 8 <= size)
    {
        const uint32_t len = ReadBE32(data + pos);
        const char* type = (const char*)data + pos + 4;
        if (pos + 12 + (size_t)len > size) return false;
        const uint8_t* body = data + pos + 8;
        if (memcmp(type, "IHDR", 4) == 0)
        {
            if (len != 13) return false;
            info.width = ReadBE32(body);
            info.height = ReadBE32(body + 4);
            info.bitDepth = body[8];
            info.colourType = body[9];
            info.interlace = body[12];
            if (body[10] != 0 || body[11] != 0) return false;   // compression/filter methods are always 0
            info.sawIhdr = true;
        }
        else if (memcmp(type, "PLTE", 4) == 0)
        {
            if (len % 3 || len > 768) return false;
            info.palette.assign(body, body + len);
        }
        else if (memcmp(type, "tRNS", 4) == 0)
        {
            if (info.colourType == 3) info.paletteAlpha.assign(body, body + len);
        }
        else if (memcmp(type, "IDAT", 4) == 0)
        {
            info.idat.insert(info.idat.end(), body, body + len);
        }
        else if (memcmp(type, "IEND", 4) == 0)
        {
            info.sawIend = true;
            break;
        }
        pos += 12 + (size_t)len;
    }
    return info.sawIhdr && !info.idat.empty();
}

int Paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Undoes the per-row filters in place, writing RGBA8 into `rgba`.
bool Unfilter(const PngInfo& info, const std::vector<uint8_t>& raw, std::vector<uint8_t>& rgba)
{
    const uint32_t w = info.width, h = info.height;
    if (!w || !h || w > 16384 || h > 16384) return false;
    if (info.bitDepth != 8 || info.interlace != 0) return false;

    int channels;
    switch (info.colourType)
    {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return false;
    }
    if (info.colourType == 3 && info.palette.empty()) return false;

    const size_t stride = (size_t)w * channels;
    if (raw.size() < (stride + 1) * h) return false;
    rgba.assign((size_t)w * h * 4, 0xFF);

    std::vector<uint8_t> prev(stride, 0), line(stride, 0);
    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* src = raw.data() + (stride + 1) * y;
        const int filter = src[0];
        src++;
        for (size_t i = 0; i < stride; i++)
        {
            const int a = i >= (size_t)channels ? line[i - channels] : 0;
            const int b = prev[i];
            const int c = i >= (size_t)channels ? prev[i - channels] : 0;
            const int x = src[i];
            int v;
            switch (filter)
            {
            case 0: v = x; break;
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + ((a + b) >> 1); break;
            case 4: v = x + Paeth(a, b, c); break;
            default: return false;
            }
            line[i] = (uint8_t)v;
        }
        uint8_t* dst = rgba.data() + (size_t)w * 4 * y;
        for (uint32_t x = 0; x < w; x++)
        {
            const uint8_t* p = line.data() + (size_t)x * channels;
            uint8_t* d = dst + (size_t)x * 4;
            switch (info.colourType)
            {
            case 0: d[0] = d[1] = d[2] = p[0]; d[3] = 0xFF; break;
            case 2: d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 0xFF; break;
            case 3:
            {
                const size_t idx = p[0];
                if (idx * 3 + 2 >= info.palette.size()) return false;
                d[0] = info.palette[idx * 3];
                d[1] = info.palette[idx * 3 + 1];
                d[2] = info.palette[idx * 3 + 2];
                d[3] = idx < info.paletteAlpha.size() ? info.paletteAlpha[idx] : 0xFF;
                break;
            }
            case 4: d[0] = d[1] = d[2] = p[0]; d[3] = p[1]; break;
            case 6: memcpy(d, p, 4); break;
            }
        }
        prev = line;
    }
    return true;
}

bool DecodePng(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    PngInfo info;
    if (!ParseChunks(data, size, info)) return false;
    std::vector<uint8_t> raw;
    if (!Inflate_Zlib(info.idat.data(), info.idat.size(), raw)) return false;
    if (!Unfilter(info, raw, rgba)) return false;
    width = info.width;
    height = info.height;
    return true;
}

// ================================================================== writer
void PutBE32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void PutChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* body, size_t len)
{
    PutBE32(out, (uint32_t)len);
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), body, body + len);
    PutBE32(out, Crc32(out.data() + start, len + 4));
}

// A zlib stream of stored (uncompressed) deflate blocks, which needs no Huffman encoder.
void PutStoredDeflate(std::vector<uint8_t>& out, const std::vector<uint8_t>& data)
{
    out.push_back(0x78);   // CMF: deflate, 32K window
    out.push_back(0x01);   // FLG: no dictionary, fastest; (0x7801 % 31 == 0)
    size_t pos = 0;
    do
    {
        const size_t n = data.size() - pos < 65535 ? data.size() - pos : 65535;
        const bool last = pos + n >= data.size();
        out.push_back(last ? 1 : 0);
        out.push_back((uint8_t)(n & 0xFF));
        out.push_back((uint8_t)(n >> 8));
        out.push_back((uint8_t)(~n & 0xFF));
        out.push_back((uint8_t)((~n >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + pos, data.begin() + pos + n);
        pos += n;
    } while (pos < data.size());

    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : data)
    {
        s1 = (s1 + b) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    PutBE32(out, (s2 << 16) | s1);   // Adler-32
}

bool EncodePng(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint32_t* rgba)
{
    if (!w || !h || !rgba) return false;
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)w * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++)
    {
        raw.push_back(0);   // filter: none
        const uint8_t* row = (const uint8_t*)(rgba + (size_t)w * y);
        raw.insert(raw.end(), row, row + (size_t)w * 4);
    }

    std::vector<uint8_t> png;
    static const uint8_t kSig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    png.insert(png.end(), kSig, kSig + 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 6;    // RGBA
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    PutChunk(png, "IHDR", ihdr, sizeof(ihdr));

    std::vector<uint8_t> z;
    PutStoredDeflate(z, raw);
    PutChunk(png, "IDAT", z.data(), z.size());
    PutChunk(png, "IEND", nullptr, 0);

    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}
}   // namespace

void Image_Init() {}

bool Image_LoadPNG(const std::filesystem::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    FILE* f = fopen(path.string().c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    uint8_t buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    if (file.empty()) return false;
    return DecodePng(file.data(), file.size(), width, height, rgba);
}

bool Image_SavePNG(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint32_t* rgba)
{
    return EncodePng(path, width, height, rgba);
}

// PNG only; the mod browser (which is what wants JPEG/WebP) is not part of the Android build.
bool Image_Decode(const void* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba)
{
    if (!data || size < 8) return false;
    return DecodePng((const uint8_t*)data, size, width, height, rgba);
}
