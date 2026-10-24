// video_tbmp.h - Mohawk 'tBMP' bitmaps (as used by the Rugrats Adventure Game data).
//
// Format, worked out and checked against the shipped archives:
//   header (big-endian): u16 width, u16 height, u16 bytesPerRow, u16 format
//     width/height: low 14 bits;  bytesPerRow = width rounded up to even
//     format & 0x0007  bits per pixel code (2 = 8-bit indexed; the only one in this game)
//     format & 0x00F0  drawing: 0x0000 raw rows, 0x0010 RLE8 rows
//     format & 0x0F00  packing: 0x0000 none,     0x0100 LZSS
//   LZSS block: u32 unpackedSize, u32 packedSize, u16 windowSize (1024), then data
//     flag byte per 8 items, LSB first: 1 = literal byte, 0 = u16 BE (len-3)<<10 | ringPos;
//     ring starts zero-filled with the write position at windowSize - 66
//   RLE8 row: u16 BE byte count (padded to even), then codes until width pixels:
//     code & 0x80: repeat next byte (code & 0x7F) + 1 times; else copy (code + 1) bytes
//   Seen: 0x0112 (LZ + RLE8, almost everything) and 0x0102 (LZ + raw, the quit screen).
#pragma once
#include <SDL.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mohawk.h"

namespace mohawk {

struct IndexedImage {
    int width = 0, height = 0;
    uint16_t format = 0;
    std::vector<uint8_t> pixels;              // width*height palette indices, top-down
};

std::optional<IndexedImage> DecodeBitmap(const std::vector<uint8_t>& resource, std::string* error = nullptr);

// LZSS unpacker (exposed for tests)
bool UnpackLZSS(const uint8_t* src, size_t srcSize, size_t windowSize, std::vector<uint8_t>& out, size_t unpackedSize);

// 32-bit RGBA surface through a page palette. transparentIndex >= 0 gets alpha 0
// (for sprites; backgrounds pass -1). Caller frees with SDL_FreeSurface.
SDL_Surface* ToSurface(const IndexedImage& img, const Palette& pal, int transparentIndex = -1);

} // namespace mohawk
