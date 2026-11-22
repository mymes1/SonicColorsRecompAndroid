// Checks the Android PNG codec (runtime/platform/android/image.cpp) against reference pixels.
//
// The fixtures in fixtures/ were produced independently of that file by Python's zlib, so a
// decoder bug shows up as a pixel mismatch rather than as agreement with itself. They cover every
// colour type PNG allows at 8 bits (0/2/3/4/6), all five row filters, palette transparency, a
// dynamic-Huffman stream and a large image whose matches are long. Run from this directory so the
// relative fixture paths resolve:
//   cmake --build build --target scr_image_test && (cd tools/image_test && ../../build/scr_image_test)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "platform/image.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                       \
    do                                                                                                                        \
    {                                                                                                                         \
        if (!(cond))                                                                                                           \
        {                                                                                                                      \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                                                                         \
            printf(__VA_ARGS__);                                                                                               \
            printf("\n");                                                                                                      \
            g_fail++;                                                                                                          \
        }                                                                                                                      \
    } while (0)

static std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::vector<uint8_t> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static const char* kFixtureDir = "fixtures/";

static void CheckPng(const std::string& name)
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    const bool ok = Image_LoadPNG(kFixtureDir + name + ".png", w, h, rgba);
    CHECK(ok, "%s: decode failed", name.c_str());
    if (!ok) return;
    const std::vector<uint8_t> want = ReadAll(kFixtureDir + name + ".raw");
    CHECK(!want.empty(), "%s: missing reference pixels", name.c_str());
    CHECK(rgba.size() == want.size(), "%s: %zu bytes, expected %zu", name.c_str(), rgba.size(), want.size());
    if (rgba.size() != want.size()) return;
    CHECK(memcmp(rgba.data(), want.data(), rgba.size()) == 0, "%s: pixels differ", name.c_str());
    printf("%s: %ux%u ok\n", name.c_str(), w, h);
}

int main()
{
    // Each of these has a matching <name>.raw holding the expected RGBA8 pixels.
    for (const char* n : { "rgba", "rgb", "pal", "gray", "graya", "big" }) CheckPng(n);

    // Round trip: encode with our writer, read back with our reader, then verify the bytes.
    // --dump-textures writes these, so the encoder has to be lossless.
    std::vector<uint32_t> px(64 * 48);
    for (uint32_t i = 0; i < px.size(); i++) px[i] = 0xFF000000u | (i * 2654435761u & 0x00FFFFFFu);
    const char* roundTrip = "roundtrip.png";
    CHECK(Image_SavePNG(roundTrip, 64, 48, px.data()), "encode failed");
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> back;
    CHECK(Image_LoadPNG(roundTrip, w, h, back), "round-trip decode failed");
    CHECK(w == 64 && h == 48, "round-trip size %ux%u", w, h);
    CHECK(back.size() == px.size() * 4 && memcmp(back.data(), px.data(), back.size()) == 0, "round-trip pixels differ");

    // A truncated or damaged file must fail rather than return half an image.
    std::vector<uint8_t> bad = ReadAll(kFixtureDir + std::string("rgba.png"));
    CHECK(!bad.empty(), "missing rgba.png");
    bad.resize(bad.size() * 3 / 4);
    uint32_t bw = 0, bh = 0;
    std::vector<uint8_t> brgba;
    CHECK(!Image_Decode(bad.data(), bad.size(), bw, bh, brgba), "truncated PNG accepted");
    CHECK(!Image_Decode("not a png at all", 16, bw, bh, brgba), "text accepted as PNG");

    printf(g_fail ? "%d check(s) failed\n" : "all image checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
