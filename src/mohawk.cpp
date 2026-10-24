#include "mohawk.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

namespace mohawk {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool Fail(std::string* e, const std::string& m) { if (e) *e = m; return false; }
} // namespace

std::string TagName(uint32_t t) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) s[size_t(i)] = char((t >> (24 - 8 * i)) & 0xFF);
    return s;
}

bool Archive::Open(const std::string& path, std::string* err) {
    path_ = path;
    resources_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return Fail(err, "cannot open " + path);
    uint8_t hdr[28];
    if (!in.read(reinterpret_cast<char*>(hdr), sizeof hdr)) return Fail(err, "short header");
    if (BE32(hdr) != Tag("MHWK") || BE32(hdr + 8) != Tag("RSRC")) return Fail(err, "not a Mohawk archive");
    if (BE16(hdr + 12) != 0x0100) return Fail(err, "unsupported RSRC version");
    fileSize_ = BE32(hdr + 16);
    const uint32_t dirOff = BE32(hdr + 20);
    const uint16_t dirLen = BE16(hdr + 24), ftLen = BE16(hdr + 26);

    std::vector<uint8_t> dir(size_t(dirLen) + ftLen);
    in.seekg(dirOff);
    if (!in.read(reinterpret_cast<char*>(dir.data()), std::streamsize(dir.size()))) return Fail(err, "short directory");
    auto at = [&](size_t o, size_t n) { if (o + n > dir.size()) throw std::out_of_range("directory"); return dir.data() + o; };

    try {
        // file table
        const uint8_t* ft = at(dirLen, 4);
        const uint32_t nFiles = BE32(ft);
        struct FileEntry { uint32_t off, size; uint8_t flags; };
        std::vector<FileEntry> files(nFiles);
        std::set<uint32_t> starts{fileSize_};
        for (uint32_t i = 0; i < nFiles; ++i) {
            const uint8_t* e = at(size_t(dirLen) + 4 + i * 10, 10);
            files[i] = {BE32(e), uint32_t(BE16(e + 4)) | (uint32_t(e[6] & 7) << 16), e[7]};
            starts.insert(files[i].off);
        }
        // types / resources / names
        const uint16_t nameListOff = BE16(at(0, 2)), nTypes = BE16(at(2, 2));
        for (uint16_t t = 0; t < nTypes; ++t) {
            const uint8_t* te = at(4 + t * 8, 8);
            const uint32_t tag = BE32(te);
            const uint16_t rto = BE16(te + 4), nto = BE16(te + 6);
            const uint16_t n = BE16(at(rto, 2));
            std::map<uint16_t, uint16_t> idByFile;                 // file index -> resource id
            for (uint16_t k = 0; k < n; ++k) {
                const uint8_t* re = at(size_t(rto) + 2 + k * 4, 4);
                const uint16_t id = BE16(re), fi = BE16(re + 2);
                if (fi == 0 || fi > nFiles) continue;
                const FileEntry& f = files[fi - 1];
                Resource r;
                r.tag = tag; r.id = id; r.offset = f.off; r.declaredSize = f.size; r.flags = f.flags;
                r.extent = *starts.upper_bound(f.off) - f.off;
                resources_[{tag, id}] = r;
                idByFile[fi] = id;
            }
            const uint16_t nn = BE16(at(nto, 2));
            for (uint16_t k = 0; k < nn; ++k) {
                const uint8_t* ne = at(size_t(nto) + 2 + k * 4, 4);
                const uint16_t nameOff = BE16(ne), index = BE16(ne + 2);
                // the name table's index is the resource's (1-based) file-table index
                auto byFile = idByFile.find(index);
                if (byFile == idByFile.end()) continue;
                const uint8_t* s = at(size_t(nameListOff) + nameOff, 1);
                const uint8_t* end = std::find(s, static_cast<const uint8_t*>(dir.data() + dir.size()), uint8_t(0));
                auto it = resources_.find({tag, byFile->second});
                if (it != resources_.end()) it->second.name.assign(reinterpret_cast<const char*>(s), size_t(end - s));
            }
        }
    } catch (const std::out_of_range&) {
        return Fail(err, "corrupt directory in " + path);
    }
    return true;
}

const Resource* Archive::Find(uint32_t tag, uint16_t id) const {
    auto it = resources_.find({tag, id});
    return it == resources_.end() ? nullptr : &it->second;
}

const Resource* Archive::FindByName(uint32_t tag, const std::string& name) const {
    for (const auto& kv : resources_)
        if (kv.first.first == tag && kv.second.name == name) return &kv.second;
    return nullptr;
}

std::vector<const Resource*> Archive::OfType(uint32_t tag) const {
    std::vector<const Resource*> v;
    for (const auto& kv : resources_) if (kv.first.first == tag) v.push_back(&kv.second);
    return v;
}

std::map<uint32_t, size_t> Archive::TypeCounts() const {
    std::map<uint32_t, size_t> m;
    for (const auto& kv : resources_) ++m[kv.first.first];
    return m;
}

std::vector<uint8_t> Archive::ReadAt(uint32_t offset, uint32_t size) const {
    std::ifstream in(path_, std::ios::binary);
    std::vector<uint8_t> v(size);
    in.seekg(offset);
    in.read(reinterpret_cast<char*>(v.data()), std::streamsize(size));
    v.resize(size_t(in.gcount()));
    return v;
}

std::vector<uint8_t> Archive::Read(const Resource& r, bool useExtent) const {
    return ReadAt(r.offset, useExtent ? r.extent : r.declaredSize);
}

std::optional<Palette> ParsePalette(const std::vector<uint8_t>& d) {
    if (d.size() < 4) return std::nullopt;
    const uint16_t first = BE16(d.data()), count = BE16(d.data() + 2);
    if (first + count > 256 || d.size() < 4 + size_t(count) * 4) return std::nullopt;
    Palette p;
    for (uint16_t i = 0; i < count; ++i)
        for (int c = 0; c < 3; ++c) p.rgb[first + i][c] = d[4 + size_t(i) * 4 + size_t(c)];
    return p;
}

} // namespace mohawk
