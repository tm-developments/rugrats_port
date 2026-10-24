// mohawk.h - reader for Mohawk 'MHWK'/'RSRC' v0x0100 archives (.MHK).
//
// Layout (validated by the original RUGRATS.EXE at 0x43C0A9 and checked against the shipped files):
//   0x00 'MHWK' u32 size | 0x08 'RSRC' u16 version(0x0100) u16 compaction u32 fileSize
//   0x14 u32 dirOffset | 0x18 u16 dirLength | 0x1A u16 fileTableLength   (all big-endian)
//   directory: u16 nameListOff, u16 typeCount, typeCount x {tag, u16 resTableOff, u16 nameTableOff}
//   resource table: u16 n, n x {u16 id, u16 fileIndex(1-based)}
//   name table:     u16 n, n x {u16 nameOff (into name list), u16 resourceIndex}
//   file table (right after the directory): u32 n, n x {u32 offset, u16 sizeLo, u8 sizeHi:3|.., u8 flags, u16}
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mohawk {

constexpr uint32_t Tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}
std::string TagName(uint32_t tag);

struct Resource {
    uint32_t tag = 0;
    uint16_t id = 0;
    std::string name;             // optional, from the name table
    uint32_t offset = 0;          // absolute file offset of the payload
    uint32_t declaredSize = 0;    // 19-bit size field
    uint32_t extent = 0;          // bytes up to the next payload (embedded movies need this)
    uint8_t flags = 0;
};

class Archive {
public:
    bool Open(const std::string& path, std::string* error = nullptr);
    const std::string& Path() const { return path_; }

    const Resource* Find(uint32_t tag, uint16_t id) const;
    const Resource* FindByName(uint32_t tag, const std::string& name) const;
    std::vector<const Resource*> OfType(uint32_t tag) const;
    std::map<uint32_t, size_t> TypeCounts() const;

    // Payload bytes (declared size; `extent` for resources whose data runs on, e.g. tMOV).
    std::vector<uint8_t> Read(const Resource& r, bool useExtent = false) const;
    std::vector<uint8_t> ReadAt(uint32_t offset, uint32_t size) const;

private:
    std::string path_;
    uint32_t fileSize_ = 0;
    std::map<std::pair<uint32_t, uint16_t>, Resource> resources_;
};

// tPAL: u16 first, u16 count, then count x {r, g, b, pad}
struct Palette { uint8_t rgb[256][3] = {}; };
std::optional<Palette> ParsePalette(const std::vector<uint8_t>& data);

} // namespace mohawk
