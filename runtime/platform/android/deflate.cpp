// DEFLATE decompression (RFC 1951), used for PNG chunks and ZIP entries. See deflate.h.
//
// Nothing here is on a per-frame path — a PNG is decoded once when a texture pack loads, a ZIP
// entry once when the player's disc image is read — so this favours a decoder that is easy to
// check against the spec over one tuned for speed.
#include "deflate.h"

namespace
{
// ================================================================== bit reader (deflate order)
// Deflate packs bits into bytes least-significant first, but Huffman codes are transmitted
// most-significant bit first, so the reader hands out single bits and the decoder shifts them in.
struct BitReader
{
    const uint8_t* data;
    size_t size;
    size_t bit = 0;

    bool Get(int count, uint32_t& out)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; i++)
        {
            const size_t byte = bit >> 3;
            if (byte >= size) return false;
            v |= (uint32_t)((data[byte] >> (bit & 7)) & 1) << i;
            bit++;
        }
        out = v;
        return true;
    }
    bool GetOne(uint32_t& out) { return Get(1, out); }
    void AlignToByte() { bit = (bit + 7) & ~(size_t)7; }
};

// ================================================================== canonical Huffman decode
// `count[len]` is how many symbols have that code length; `symbol[]` lists them in order.
struct Huffman
{
    uint16_t count[16] = {};
    uint16_t symbol[288] = {};

    // Returns the number of code lengths in `lengths`, or -1 if the set is over-subscribed.
    int Build(const uint8_t* lengths, int n)
    {
        for (int i = 0; i < 16; i++) count[i] = 0;
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        if (count[0] == n) return n;   // no codes at all: legal for a distance set
        int left = 1;                  // remaining code space, in units of 1/2^len
        for (int len = 1; len < 16; len++)
        {
            left <<= 1;
            left -= count[len];
            if (left < 0) return -1;
        }
        uint16_t offs[16] = {};
        for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbol[offs[lengths[i]]++] = (uint16_t)i;
        return n;
    }

    // Decodes one symbol MSB-first; -1 on a truncated or invalid code.
    int Decode(BitReader& br) const
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++)
        {
            uint32_t b;
            if (!br.GetOne(b)) return -1;
            code |= (int)b;
            const int n = count[len];
            if (code - first < n) return symbol[index + (code - first)];
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// Fixed Huffman tables for deflate's "type 1" blocks (RFC 1951 3.2.6).
void BuildFixed(Huffman& lit, Huffman& dist)
{
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    lit.Build(lengths, 288);
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    dist.Build(lengths, 30);
}

const int kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                           35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const int kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const int kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const int kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool InflateBlock(BitReader& br, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out)
{
    for (;;)
    {
        const int sym = lit.Decode(br);
        if (sym < 0) return false;
        if (sym < 256)
        {
            out.push_back((uint8_t)sym);
            continue;
        }
        if (sym == 256) return true;
        const int li = sym - 257;
        if (li >= 29) return false;
        uint32_t extra;
        if (!br.Get(kLenExtra[li], extra)) return false;
        const size_t len = (size_t)(kLenBase[li] + (int)extra);
        const int dsym = dist.Decode(br);
        if (dsym < 0 || dsym >= 30) return false;
        if (!br.Get(kDistExtra[dsym], extra)) return false;
        const size_t back = (size_t)(kDistBase[dsym] + (int)extra);
        if (back > out.size()) return false;
        const size_t from = out.size() - back;
        for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);   // overlapping copies are legal
    }
}

bool InflateStream(BitReader br, std::vector<uint8_t>& out)
{
    bool final = false;
    while (!final)
    {
        uint32_t last, type;
        if (!br.GetOne(last) || !br.Get(2, type)) return false;
        final = last != 0;
        if (type == 0)
        {
            // Stored (uncompressed) block: byte-aligned, with a length and its complement.
            br.AlignToByte();
            const size_t byte = br.bit >> 3;
            if (byte + 4 > br.size) return false;
            const uint32_t len = br.data[byte] | ((uint32_t)br.data[byte + 1] << 8);
            const uint32_t nlen = br.data[byte + 2] | ((uint32_t)br.data[byte + 3] << 8);
            if ((len ^ 0xFFFFu) != nlen || byte + 4 + len > br.size) return false;
            out.insert(out.end(), br.data + byte + 4, br.data + byte + 4 + len);
            br.bit = (byte + 4 + len) * 8;
            continue;
        }
        if (type == 3) return false;
        Huffman lit, dist;
        if (type == 1)
            BuildFixed(lit, dist);
        else
        {
            uint32_t hlit, hdist, hclen;
            if (!br.Get(5, hlit) || !br.Get(5, hdist) || !br.Get(4, hclen)) return false;
            const int nlit = (int)hlit + 257, ndist = (int)hdist + 1, nclen = (int)hclen + 4;
            if (nlit > 286 || ndist > 30) return false;
            static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t codeLengths[19] = {};
            for (int i = 0; i < nclen; i++)
            {
                uint32_t v;
                if (!br.Get(3, v)) return false;
                codeLengths[order[i]] = (uint8_t)v;
            }
            Huffman codeHuff;
            if (codeHuff.Build(codeLengths, 19) < 0) return false;

            uint8_t lengths[286 + 30] = {};
            int i = 0;
            while (i < nlit + ndist)
            {
                const int sym = codeHuff.Decode(br);
                if (sym < 0) return false;
                if (sym < 16)
                {
                    lengths[i++] = (uint8_t)sym;
                    continue;
                }
                int repeat, value = 0;
                uint32_t extra;
                if (sym == 16)
                {
                    if (i == 0 || !br.Get(2, extra)) return false;
                    repeat = 3 + (int)extra;
                    value = lengths[i - 1];
                }
                else if (sym == 17)
                {
                    if (!br.Get(3, extra)) return false;
                    repeat = 3 + (int)extra;
                }
                else
                {
                    if (!br.Get(7, extra)) return false;
                    repeat = 11 + (int)extra;
                }
                if (i + repeat > nlit + ndist) return false;
                while (repeat--) lengths[i++] = (uint8_t)value;
            }
            if (lit.Build(lengths, nlit) < 0 || dist.Build(lengths + nlit, ndist) < 0) return false;
        }
        if (!InflateBlock(br, lit, dist, out)) return false;
    }
    return true;
}
}   // namespace

bool Inflate_Raw(const uint8_t* src, size_t size, std::vector<uint8_t>& out)
{
    if (!src) return false;
    return InflateStream(BitReader{ src, size }, out);
}

bool Inflate_Zlib(const uint8_t* src, size_t size, std::vector<uint8_t>& out)
{
    // zlib wrapper (RFC 1950): CMF/FLG, deflate, no preset dictionary. PNG's IDAT chunks carry
    // this; a ZIP entry carries Inflate_Raw instead.
    if (!src || size < 6) return false;
    if ((src[0] & 15) != 8 || (((uint32_t)src[0] << 8) | src[1]) % 31 != 0) return false;
    if (src[1] & 0x20) return false;
    return InflateStream(BitReader{ src + 2, size - 2 }, out);
}
