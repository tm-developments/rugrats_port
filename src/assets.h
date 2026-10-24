// assets.h - OUTLINE manifest, archive cache, resource search and movie lookup.
#pragma once
#include "mohawk.h"
#include "video_tbmp.h"
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace port {

// PROGRAM/OUTLINE: the engine's "book" description.
struct Outline {
    struct Proxy { std::string path; uint16_t baseId = 0; };
    struct Page  { std::string key, path; bool fade = false; };
    std::map<std::string, std::string> bookInfo;           // [BookInfo]
    std::map<std::string, Proxy> proxies;                  // [Proxies]  Name = "path;baseId"
    std::map<std::string, std::vector<Page>> pages;        // [Intro] [Control] [English.Play] ...
    std::map<std::string, std::string> globals;            // [Globals]  name = value;
    bool Load(const std::string& path, std::string* error = nullptr);
    int Int(const std::string& key, int fallback) const;   // BookInfo integer
};

struct MovieAsset {
    std::string name;               // tMOV resource name, e.g. "CryNISMovie"
    std::string file;               // the movie extracted by prepare.cpp (listed in movies.manifest)
    std::string audio;              // <stem>.wav soundtrack (made by prepare.cpp), or ""
    int32_t timeScale = 600, duration = 0;
    bool loop = false;
    int width = 0, height = 0;
    uint64_t moovHash = 0;
};

class AssetManager {
public:
    // discRoot: folder holding MHK/ and PROGRAM/ (e.g. C:\RAG).
    // movieDir: the movies\ folder made by prepare.cpp (--prepare or the first run).
    bool Open(const std::string& discRoot, const std::string& movieDir, std::string* error = nullptr);

    const Outline& GetOutline() const { return outline_; }

    // OUTLINE paths: "//RUGRATS/MHK/X.MHK" is on the disc, "MHK/X.MHK" is under PROGRAM/.
    std::string Resolve(const std::string& outlinePath) const;
    mohawk::Archive* Archive(const std::string& outlinePath, std::string* error = nullptr);

    // Search chain, newest first (the engine's GetResource at 0x43A452 works the same way).
    void Push(mohawk::Archive* a);
    void Remove(mohawk::Archive* a);
    std::pair<mohawk::Archive*, const mohawk::Resource*> Find(uint32_t tag, uint16_t id) const;

    mohawk::Archive* LoadProxy(const std::string& name, std::string* error = nullptr);
    mohawk::Archive* LoadPage(const std::string& section, size_t index, std::string* error = nullptr);

    std::optional<mohawk::Palette> PagePalette(const mohawk::Archive& a) const;
    std::optional<mohawk::IndexedImage> Bitmap(const mohawk::Archive& a, uint16_t id, std::string* error = nullptr) const;
    // Background heuristic: the largest tBMP at least 640 wide (MAIN "Bg", N01 "View4",
    // RRQUIT "Quitbg"). Only reads the 8-byte headers. nullptr if the archive has none.
    const mohawk::Resource* BackgroundBitmap(const mohawk::Archive& a) const;
    // tMOV -> original moov bytes -> FNV-1a-64 -> manifest entry
    std::optional<MovieAsset> Movie(const mohawk::Archive& a, uint16_t tmovId, std::string* error = nullptr) const;

    static uint64_t Fnv1a64(const uint8_t* p, size_t n);

private:
    std::string ResolveIn(const std::string& base, const std::string& rel) const;
    std::string FindCaseInsensitive(const std::string& dir, const std::string& name) const;
    std::string root_, movieDir_;
    Outline outline_;
    std::map<uint64_t, MovieAsset> manifest_;
    std::map<std::string, std::unique_ptr<mohawk::Archive>> cache_;   // by resolved path
    std::deque<mohawk::Archive*> chain_;
};

} // namespace port
