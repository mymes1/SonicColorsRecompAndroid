#pragma once
// DEFLATE decompression, shared by the PNG codec (image.cpp) and the ZIP reader
// (iso_install.cpp). Both are Android-only: the desktop build uses SDL3/WIC for images and nod for
// disc images, so neither needs this.
//
// This is a straightforward canonical-Huffman decoder (RFC 1951) with no zlib-style streaming API:
// the whole compressed input is in memory and the whole result comes back, which is how a PNG
// chunk and a ZIP entry are both read. It is not on a per-frame path.
#include <cstddef>
#include <cstdint>
#include <vector>

// A raw DEFLATE stream (what ZIP stores for method 8).
bool Inflate_Raw(const uint8_t* src, size_t size, std::vector<uint8_t>& out);
// A zlib stream: two header bytes and an Adler-32 trailer around a DEFLATE stream (what PNG's
// IDAT chunks contain). The checksum is not verified.
bool Inflate_Zlib(const uint8_t* src, size_t size, std::vector<uint8_t>& out);
