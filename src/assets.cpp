#include "assets.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace port {
namespace {
std::string Trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
    return s;
}
std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string Unquote(const std::string& v, std::string* rest = nullptr) {
    if (v.size() >= 2 && v[0] == '"') {
        const size_t e = v.find('"', 1);
        if (e != std::string::npos) { if (rest) *rest = Trim(v.substr(e + 1)); return v.substr(1, e - 1); }
    }
    if (rest) rest->clear();
    return v;
}
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
} // namespace

bool Outline::Load(const std::string& path, std::string* err) {
    std::ifstream in(path);
    if (!in) { if (err) *err = "cannot open " + path; return false; }
    std::string line, section;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1)), rest;
        if (section == "Globals") {
            if (!value.empty() && value.back() == ';') value.pop_back();
            globals[key] = Unquote(Trim(value));
        } else if (section == "BookInfo") {
            bookInfo[key] = Unquote(value);
        } else if (section == "Proxies") {
            const std::string v = Unquote(value);
            const size_t semi = v.find(';');
            Proxy p;
            p.path = v.substr(0, semi);
            if (semi != std::string::npos) p.baseId = uint16_t(std::stoul(v.substr(semi + 1)));
            proxies[key] = p;
        } else if (section != "Languages" && section != "Exceptions") {
            Page p;
            p.key = key;
            p.path = Unquote(value, &rest);
            p.fade = Lower(rest) == "fade";
            pages[section].push_back(p);
        }
    }
    return true;
}

int Outline::Int(const std::string& key, int fallback) const {
    auto it = bookInfo.find(key);
    if (it == bookInfo.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

uint64_t AssetManager::Fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}

bool AssetManager::Open(const std::string& discRoot, const std::string& movieDir, std::string* err) {
    root_ = discRoot;
    movieDir_ = movieDir;
    const std::string program = FindCaseInsensitive(root_, "PROGRAM");
    const std::string outline = program.empty() ? "" : FindCaseInsensitive(program, "OUTLINE");
    if (outline.empty() || !outline_.Load(outline, err)) {
        if (err && err->empty()) *err = "no PROGRAM/OUTLINE under " + discRoot;
        return false;
    }
    std::ifstream m((fs::path(movieDir_) / "movies.manifest").string());
    std::string line;
    while (std::getline(m, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::istringstream ss(line);
        std::string key; MovieAsset a; int loop = 0;
        if (ss >> key >> a.file >> a.timeScale >> a.duration >> loop >> a.width >> a.height) {
            a.loop = loop != 0;
            a.moovHash = std::stoull(key, nullptr, 16);
            a.file = (fs::path(movieDir_) / a.file).string();
            manifest_[a.moovHash] = a;
        }
    }
    return true;
}

std::string AssetManager::FindCaseInsensitive(const std::string& dir, const std::string& name) const {
    std::error_code ec;
    const fs::path exact = fs::path(dir) / name;
    if (fs::exists(exact, ec)) return exact.string();
    const std::string want = Lower(name);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (Lower(e.path().filename().string()) == want) return e.path().string();
    return {};
}

// The installer's layout: PROGRAM\*.* (the original RUGRATS.EXE, OUTLINE, MOHAWK.W32, GLOBALS, PLAYFILE,
// SYSTEM.MHK and PROGRAM\MHK: the zoom archives Z01-Z20, ZOOMS, DETECTOR) goes to the hard
// disk; everything else stays on the CD (volume RUGRATS). OUTLINE says which: "MHK/X" is
// relative to the installed program folder, "//RUGRATS/MHK/X" is on the disc. The port's
// folder plays the disc (MHK\) with PROGRAM\ as the installed folder; if an archive isn't
// where the outline says, the other place is tried too.
std::string AssetManager::Resolve(const std::string& p) const {
    std::string rel = p;
    const bool onDisc = rel.rfind("//", 0) == 0;
    if (onDisc) {                                   // "//RUGRATS/MHK/X" : disc volume label
        const size_t slash = rel.find('/', 2);
        rel = slash == std::string::npos ? "" : rel.substr(slash + 1);
    }
    const std::string program = FindCaseInsensitive(root_, "PROGRAM");
    std::string found = ResolveIn(onDisc ? root_ : program, rel);
    if (found.empty() && !program.empty()) found = ResolveIn(onDisc ? program : root_, rel);
    return found;
}

std::string AssetManager::ResolveIn(const std::string& base, const std::string& rel) const {
    if (base.empty()) return {};
    std::string cur = base;
    std::stringstream parts(rel);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty()) continue;
        std::string next = FindCaseInsensitive(cur, part);
        if (next.empty()) return {};
        cur = next;
    }
    return cur;
}

mohawk::Archive* AssetManager::Archive(const std::string& outlinePath, std::string* err) {
    const std::string path = Resolve(outlinePath);
    if (path.empty()) { if (err) *err = "not found: " + outlinePath; return nullptr; }
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second.get();
    auto a = std::make_unique<mohawk::Archive>();
    if (!a->Open(path, err)) return nullptr;
    return (cache_[path] = std::move(a)).get();
}

void AssetManager::Push(mohawk::Archive* a) { Remove(a); chain_.push_front(a); }
void AssetManager::Remove(mohawk::Archive* a) { chain_.erase(std::remove(chain_.begin(), chain_.end(), a), chain_.end()); }

std::pair<mohawk::Archive*, const mohawk::Resource*> AssetManager::Find(uint32_t tag, uint16_t id) const {
    for (mohawk::Archive* a : chain_)
        if (const mohawk::Resource* r = a->Find(tag, id)) return {a, r};
    return {nullptr, nullptr};
}

mohawk::Archive* AssetManager::LoadProxy(const std::string& name, std::string* err) {
    auto it = outline_.proxies.find(name);
    if (it == outline_.proxies.end()) { if (err) *err = "no proxy " + name; return nullptr; }
    mohawk::Archive* a = Archive(it->second.path, err);
    if (a) Push(a);
    return a;
}

mohawk::Archive* AssetManager::LoadPage(const std::string& section, size_t index, std::string* err) {
    auto it = outline_.pages.find(section);
    if (it == outline_.pages.end() || index >= it->second.size()) { if (err) *err = "no page " + section; return nullptr; }
    mohawk::Archive* a = Archive(it->second[index].path, err);
    if (a) Push(a);
    return a;
}

std::optional<mohawk::Palette> AssetManager::PagePalette(const mohawk::Archive& a) const {
    auto pals = a.OfType(mohawk::Tag("tPAL"));
    if (pals.empty()) return std::nullopt;
    return mohawk::ParsePalette(a.Read(*pals.front()));
}

std::optional<mohawk::IndexedImage> AssetManager::Bitmap(const mohawk::Archive& a, uint16_t id, std::string* err) const {
    const mohawk::Resource* r = a.Find(mohawk::Tag("tBMP"), id);
    if (!r) { if (err) *err = "no tBMP " + std::to_string(id); return std::nullopt; }
    return mohawk::DecodeBitmap(a.Read(*r), err);
}

const mohawk::Resource* AssetManager::BackgroundBitmap(const mohawk::Archive& a) const {
    const mohawk::Resource* best = nullptr;
    long bestArea = 0;
    for (const mohawk::Resource* r : a.OfType(mohawk::Tag("tBMP"))) {
        const std::vector<uint8_t> h = a.ReadAt(r->offset, 4);
        if (h.size() < 4) continue;
        const long w = ((h[0] << 8) | h[1]) & 0x3FFF, ht = ((h[2] << 8) | h[3]) & 0x3FFF;
        if (w >= 640 && w * ht > bestArea) { bestArea = w * ht; best = r; }
    }
    return best;
}

std::optional<MovieAsset> AssetManager::Movie(const mohawk::Archive& a, uint16_t id, std::string* err) const {
    const mohawk::Resource* r = a.Find(mohawk::Tag("tMOV"), id);
    if (!r) { if (err) *err = "no tMOV " + std::to_string(id); return std::nullopt; }
    // The tMOV payload offset is where the movie's 'moov' atom starts (the original RUGRATS.EXE 0x4605AE).
    const std::vector<uint8_t> hdr = a.ReadAt(r->offset, 8);
    if (hdr.size() < 8 || BE32(hdr.data() + 4) != mohawk::Tag("moov")) { if (err) *err = "tMOV has no moov atom"; return std::nullopt; }
    const std::vector<uint8_t> moov = a.ReadAt(r->offset, BE32(hdr.data()));
    const uint64_t key = Fnv1a64(moov.data(), moov.size());
    auto it = manifest_.find(key);
    if (it == manifest_.end()) { if (err) *err = "movie not converted (run the tools on " + a.Path() + ")"; return std::nullopt; }
    MovieAsset m = it->second;
    m.name = r->name;
    std::error_code ec;
    fs::path file(m.file);
    // Prefer the original QuickTime file: it keeps every video track (QuickTime composited
    // overlay tracks; the transcoded MP4 holds only one), with lossless pictures.
    fs::path mov = fs::path(file).replace_extension(".mov");
    if (fs::exists(mov, ec)) m.file = mov.string();
    fs::path wav = fs::path(file).replace_extension(".wav");
    if (fs::exists(wav, ec)) m.audio = wav.string();
    return m;
}

} // namespace port
