#include "video_tbmp.h"
#include <algorithm>
#include <cstring>

namespace mohawk {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
template <class T> std::optional<T> Fail(std::string* e, const std::string& m) { if (e) *e = m; return std::nullopt; }

enum : uint16_t {
    kBppMask = 0x0007, kBpp8 = 0x0002,
    kDrawMask = 0x00F0, kDrawRaw = 0x0000, kDrawRLE8 = 0x0010,
    kPackMask = 0x0F00, kPackNone = 0x0000, kPackLZ = 0x0100,
};
constexpr size_t kMaxLength = 66;            // (63 >> 0) + 3: longest match a 6-bit length allows

bool DrawRLE8(const uint8_t* d, size_t n, int w, int h, std::vector<uint8_t>& out) {
    size_t p = 0;
    for (int y = 0; y < h; ++y) {
        if (p + 2 > n) return false;
        const size_t rowBytes = BE16(d + p);
        p += 2;
        const size_t rowStart = p, rowEnd = p + rowBytes;
        if (rowEnd > n) return false;
        uint8_t* dst = &out[size_t(y) * size_t(w)];
        int x = 0;
        while (x < w) {
            if (p >= rowEnd) return false;
            const uint8_t code = d[p++];
            const int run = std::min((code & 0x7F) + 1, w - x);
            if (code & 0x80) {
                if (p >= rowEnd) return false;
                std::memset(dst + x, d[p++], size_t(run));
            } else {
                if (p + size_t(run) > rowEnd) return false;
                std::memcpy(dst + x, d + p, size_t(run));
                p += size_t(run);
            }
            x += run;
        }
        if (p + 1 < rowEnd) return false;      // allow only the even-length pad byte
        p = rowStart + rowBytes;
    }
    return true;
}
} // namespace

bool UnpackLZSS(const uint8_t* src, size_t n, size_t window, std::vector<uint8_t>& out, size_t unpacked) {
    if (window == 0 || (window & (window - 1)) || window <= kMaxLength) return false;
    const size_t mask = window - 1;
    std::vector<uint8_t> ring(window, 0);
    size_t r = window - kMaxLength, i = 0;
    unsigned flags = 0;
    out.clear();
    out.reserve(unpacked);
    while (out.size() < unpacked) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            if (i >= n) return false;
            flags = src[i++] | 0xFF00u;
        }
        if (flags & 1) {
            if (i >= n) return false;
            const uint8_t c = src[i++];
            out.push_back(c);
            ring[r] = c; r = (r + 1) & mask;
        } else {
            if (i + 2 > n) return false;
            const uint16_t v = BE16(src + i);
            i += 2;
            const size_t pos = v & mask, len = (v >> 10) + 3;
            for (size_t k = 0; k < len && out.size() < unpacked; ++k) {
                const uint8_t c = ring[(pos + k) & mask];
                out.push_back(c);
                ring[r] = c; r = (r + 1) & mask;
            }
        }
    }
    return true;
}

std::optional<IndexedImage> DecodeBitmap(const std::vector<uint8_t>& res, std::string* err) {
    if (res.size() < 8) return Fail<IndexedImage>(err, "tBMP shorter than its header");
    IndexedImage img;
    img.width = BE16(res.data()) & 0x3FFF;
    img.height = BE16(res.data() + 2) & 0x3FFF;
    const size_t bytesPerRow = BE16(res.data() + 4);
    img.format = BE16(res.data() + 6);
    if (img.width == 0 || img.height == 0) return Fail<IndexedImage>(err, "empty tBMP");
    if ((img.format & kBppMask) != kBpp8)
        return Fail<IndexedImage>(err, "unsupported tBMP depth (format " + std::to_string(img.format) + ")");

    // 1. unpack
    std::vector<uint8_t> packedStore;
    const uint8_t* data = res.data() + 8;
    size_t size = res.size() - 8;
    switch (img.format & kPackMask) {
    case kPackNone: break;
    case kPackLZ: {
        if (size < 10) return Fail<IndexedImage>(err, "truncated LZ header");
        const uint32_t unpacked = BE32(data), packed = BE32(data + 4);
        const uint16_t window = BE16(data + 8);
        if (packed > size - 10 || unpacked > (64u << 20)) return Fail<IndexedImage>(err, "bad LZ sizes");
        if (!UnpackLZSS(data + 10, packed, window, packedStore, unpacked)) return Fail<IndexedImage>(err, "corrupt LZ data");
        data = packedStore.data();
        size = packedStore.size();
        break;
    }
    default:
        return Fail<IndexedImage>(err, "unsupported tBMP packing (format " + std::to_string(img.format) + ")");
    }

    // 2. draw rows
    img.pixels.assign(size_t(img.width) * size_t(img.height), 0);
    switch (img.format & kDrawMask) {
    case kDrawRaw:
        if (bytesPerRow < size_t(img.width) || size < bytesPerRow * size_t(img.height))
            return Fail<IndexedImage>(err, "raw tBMP too short");
        for (int y = 0; y < img.height; ++y)
            std::memcpy(&img.pixels[size_t(y) * size_t(img.width)], data + size_t(y) * bytesPerRow, size_t(img.width));
        break;
    case kDrawRLE8:
        if (!DrawRLE8(data, size, img.width, img.height, img.pixels)) return Fail<IndexedImage>(err, "corrupt RLE8 rows");
        break;
    default:
        return Fail<IndexedImage>(err, "unsupported tBMP drawing (format " + std::to_string(img.format) + ")");
    }
    return img;
}

SDL_Surface* ToSurface(const IndexedImage& img, const Palette& pal, int transparentIndex) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, img.width, img.height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    SDL_LockSurface(s);
    for (int y = 0; y < img.height; ++y) {
        auto* row = static_cast<uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch);
        const uint8_t* src = &img.pixels[size_t(y) * size_t(img.width)];
        for (int x = 0; x < img.width; ++x) {
            const uint8_t i = src[x];
            row[x * 4 + 0] = pal.rgb[i][0];          // RGBA32 = R,G,B,A bytes on any endianness
            row[x * 4 + 1] = pal.rgb[i][1];
            row[x * 4 + 2] = pal.rgb[i][2];
            row[x * 4 + 3] = (int(i) == transparentIndex) ? 0 : 255;
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

} // namespace mohawk
