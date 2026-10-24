// prepare.cpp - builds the game's movie and sound folders from the original archives.
//
// The original player streams QuickTime movies and Mohawk tWAV sounds straight out of the
// .MHK archives. The port plays them from standalone files instead:
//   movies\<ARCHIVE>_<moov offset>.mov   each QuickTime movie, copied out with its chunk
//                                        offsets rebased (moov first, then its mdat)
//   movies\<same>.wav                    that movie's soundtrack, 16-bit PCM
//   movies\movies.manifest               FNV-1a-64 of the original moov -> file, timing
//   sounds\<ARCHIVE>_<id>.wav            every tWAV sound, 16-bit PCM
// This used to be three Python scripts; it is the same work done natively so an installer
// can run it (rugrats_port.exe --prepare) right after copying the disc's MHK and PROGRAM.
#include "prepare.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace port {
namespace {
namespace fs = std::filesystem;

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t BE64(const uint8_t* p) { return uint64_t(BE32(p)) << 32 | BE32(p + 4); }
void PutBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void PutBE64(uint8_t* p, uint64_t v) { PutBE32(p, uint32_t(v >> 32)); PutBE32(p + 4, uint32_t(v)); }

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    out.resize(size_t(in.tellg()));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

std::string Upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

// ---- WAV out -------------------------------------------------------------------------------
bool WriteWav(const fs::path& path, const std::vector<int16_t>& samples, int rate, int channels) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const uint32_t data = uint32_t(samples.size() * 2);
    auto u32 = [&](uint32_t v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; out.write(b, 4); };
    auto u16 = [&](uint16_t v) { char b[2] = {char(v), char(v >> 8)}; out.write(b, 2); };
    out.write("RIFF", 4); u32(36 + data); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(uint16_t(channels)); u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 2)); u16(uint16_t(channels * 2)); u16(16);
    out.write("data", 4); u32(data);
    out.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(data));   // little-endian hosts
    return bool(out);
}

// ---- Mohawk archives ------------------------------------------------------------------------
struct Res { std::string tag; uint16_t id; size_t off, size; };

std::vector<Res> Resources(const std::vector<uint8_t>& d) {
    std::vector<Res> out;
    if (d.size() < 26 || std::memcmp(d.data(), "MHWK", 4) || std::memcmp(d.data() + 8, "RSRC", 4)) return out;
    const uint32_t dir = BE32(&d[20]);
    const uint16_t ftOff = BE16(&d[24]);
    if (dir + 4 > d.size()) return out;
    const uint16_t ntypes = BE16(&d[dir + 2]);
    const size_t ft = size_t(dir) + ftOff;
    if (ft + 4 > d.size()) return out;
    const uint32_t nfiles = BE32(&d[ft]);
    std::vector<std::pair<size_t, size_t>> files;
    for (uint32_t i = 0; i < nfiles && ft + 4 + size_t(i) * 10 + 7 <= d.size(); ++i) {
        const uint8_t* f = &d[ft + 4 + size_t(i) * 10];
        files.emplace_back(BE32(f), size_t(BE16(f + 4)) | size_t(f[6]) << 16);
    }
    for (uint16_t t = 0; t < ntypes && dir + 12 + size_t(t) * 8 <= d.size(); ++t) {
        const uint8_t* e = &d[dir + 4 + size_t(t) * 8];
        const std::string tag(reinterpret_cast<const char*>(e), 4);
        const size_t rt = size_t(dir) + BE16(e + 4);
        if (rt + 2 > d.size()) continue;
        const uint16_t n = BE16(&d[rt]);
        for (uint16_t j = 0; j < n && rt + 6 + size_t(j) * 4 <= d.size(); ++j) {
            const uint16_t rid = BE16(&d[rt + 2 + size_t(j) * 4]), idx = BE16(&d[rt + 4 + size_t(j) * 4]);
            if (idx == 0 || idx > files.size()) continue;
            out.push_back(Res{tag, rid, files[idx - 1].first, files[idx - 1].second});
        }
    }
    return out;
}

// tWAV: 'MHWK' size 'WAVE', chunks; 'Data': u16 rate, u32 count, u8 bits, u8 channels,
// u16 encoding (0 PCM, 1 IMA ADPCM high nibble first, 2 MPEG), loop info, samples.
bool DecodeTWav(const uint8_t* b, size_t n, std::vector<int16_t>& pcm, int& rate, int& channels) {
    if (n < 12 || std::memcmp(b, "MHWK", 4) || std::memcmp(b + 8, "WAVE", 4)) return false;
    size_t p = 12;
    while (p + 8 <= n) {
        const uint32_t size = BE32(b + p + 4);
        if (!std::memcmp(b + p, "Data", 4) && p + 28 <= n) {
            rate = BE16(b + p + 8);
            const uint32_t count = BE32(b + p + 10);
            const int bits = b[p + 14];
            channels = std::max(1, int(b[p + 15]));
            const int enc = BE16(b + p + 16);
            const uint8_t* raw = b + p + 28;
            const size_t len = std::min(size_t(size) >= 20 ? size_t(size) - 20 : 0, n - (p + 28));
            pcm.clear();
            if (enc == 0 && bits == 8) {
                for (size_t i = 0; i < len; ++i) pcm.push_back(int16_t((int(raw[i]) - 128) << 8));
            } else if (enc == 0 && bits == 16) {
                for (size_t i = 0; i + 1 < len; i += 2) pcm.push_back(int16_t(BE16(raw + i)));
            } else if (enc == 1) {
                // IMA ADPCM, one continuous stream per channel, high nibble first, nibbles
                // alternating between the channels (ffmpeg's adpcm_ima_ssi)
                static const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
                static const int kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
                    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337,
                    371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
                    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
                    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
                int pred[2] = {0, 0}, index[2] = {0, 0};
                int ch = 0;
                auto nib = [&](int code) {
                    const int step = kStep[index[ch]];
                    int diff = step >> 3;
                    if (code & 4) diff += step;
                    if (code & 2) diff += step >> 1;
                    if (code & 1) diff += step >> 2;
                    pred[ch] = std::clamp(code & 8 ? pred[ch] - diff : pred[ch] + diff, -32768, 32767);
                    index[ch] = std::clamp(index[ch] + kIndex[code], 0, 88);
                    pcm.push_back(int16_t(pred[ch]));
                    ch = (ch + 1) % std::min(channels, 2);
                };
                for (size_t i = 0; i < len; ++i) { nib(raw[i] >> 4); nib(raw[i] & 15); }
            } else {
                return false;                       // MPEG audio: not used by this game
            }
            if (count && pcm.size() > size_t(count) * size_t(channels)) pcm.resize(size_t(count) * size_t(channels));
            return true;
        }
        p += 8 + size + (size & 1);
    }
    return false;
}

// ---- QuickTime movies inside the archives ---------------------------------------------------
uint64_t Fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}

bool IsContainer(const uint8_t* t) {
    static const char* kC[] = {"moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "udta"};
    for (const char* c : kC) if (!std::memcmp(t, c, 4)) return true;
    return false;
}
void Walk(const std::vector<uint8_t>& d, size_t o, size_t end, const std::function<void(size_t, size_t, const uint8_t*)>& fn) {
    while (o + 8 <= end) {
        const uint32_t sz = BE32(&d[o]);
        if (sz < 8 || o + sz > end) break;
        fn(o, sz, &d[o + 4]);
        if (IsContainer(&d[o + 4])) Walk(d, o + 8, o + sz, fn);
        o += sz;
    }
}

struct MovieInfo { uint32_t scale = 0, duration = 0, w = 0, h = 0; bool loop = false; };

MovieInfo Info(const std::vector<uint8_t>& m) {
    MovieInfo i;
    Walk(m, 0, m.size(), [&](size_t o, size_t sz, const uint8_t* t) {
        if (!std::memcmp(t, "mvhd", 4) && o + 28 <= m.size()) { i.scale = BE32(&m[o + 20]); i.duration = BE32(&m[o + 24]); }
        else if (!std::memcmp(t, "tkhd", 4) && o + 92 <= m.size()) {
            const uint32_t w = BE32(&m[o + 84]), h = BE32(&m[o + 88]);
            if (w) { i.w = w >> 16; i.h = h >> 16; }
        } else if (!std::memcmp(t, "LOOP", 4)) i.loop = true;
        (void)sz;
    });
    return i;
}

// ---- a movie's soundtrack, through FFmpeg ------------------------------------------------
bool MovieAudio(const fs::path& mov, const fs::path& wav) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, mov.u8string().c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<int16_t> pcm;
    int rate = 0, channels = 0;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (s < 0) break;
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[s]->codecpar->codec_id);
        if (!codec) break;
        ctx = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(ctx, fmt->streams[s]->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) break;
        auto drain = [&]() {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                const int ch = frame->ch_layout.nb_channels;
                rate = frame->sample_rate;
                channels = ch;
                const AVSampleFormat f = AVSampleFormat(frame->format);
                const bool planar = av_sample_fmt_is_planar(f);
                for (int i = 0; i < frame->nb_samples; ++i)
                    for (int c = 0; c < ch; ++c) {
                        const int plane = planar ? c : 0, idx = planar ? i : i * ch + c;
                        const uint8_t* base = frame->extended_data[plane];
                        int v = 0;
                        switch (av_get_packed_sample_fmt(f)) {
                        case AV_SAMPLE_FMT_U8: v = (int(base[idx]) - 128) << 8; break;
                        case AV_SAMPLE_FMT_S16: v = reinterpret_cast<const int16_t*>(base)[idx]; break;
                        case AV_SAMPLE_FMT_S32: v = reinterpret_cast<const int32_t*>(base)[idx] >> 16; break;
                        case AV_SAMPLE_FMT_FLT: v = int(std::clamp(reinterpret_cast<const float*>(base)[idx], -1.f, 1.f) * 32767.f); break;
                        case AV_SAMPLE_FMT_DBL: v = int(std::clamp(reinterpret_cast<const double*>(base)[idx], -1.0, 1.0) * 32767.0); break;
                        default: break;
                        }
                        pcm.push_back(int16_t(v));
                    }
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == s && avcodec_send_packet(ctx, pkt) == 0) drain();
            av_packet_unref(pkt);
        }
        avcodec_send_packet(ctx, nullptr);
        drain();
        ok = !pcm.empty() && rate > 0 && channels > 0 && WriteWav(wav, pcm, rate, channels);
    } while (false);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
}

std::vector<fs::path> Archives(const fs::path& data) {
    std::vector<fs::path> out;
    std::error_code ec;
    std::set<std::string> seen;
    std::function<void(const fs::path&, int)> scan = [&](const fs::path& dir, int depth) {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_directory(ec)) { if (depth < 2) scan(e.path(), depth + 1); continue; }
            const std::string n = Upper(e.path().filename().string());
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".MHK") == 0 && seen.insert(n).second) out.push_back(e.path());
        }
    };
    scan(data, 0);
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return Upper(a.filename().string()) < Upper(b.filename().string()); });
    return out;
}

} // namespace

bool PrepareAssets(const std::string& dataDir, const std::string& outDir, const PrepareProgress& progress) {
    const fs::path data = fs::u8path(dataDir), out = fs::u8path(outDir.empty() ? dataDir : outDir);
    const fs::path movies = out / "movies", sounds = out / "sounds";
    std::error_code ec;
    fs::create_directories(movies, ec);
    fs::create_directories(sounds, ec);
    const std::vector<fs::path> archives = Archives(data);
    if (archives.empty()) { std::printf("prepare: no .MHK archives under %s\n", dataDir.c_str()); return false; }

    struct Entry { uint64_t key; std::string base; MovieInfo info; };
    std::vector<Entry> manifest;
    size_t nSounds = 0, nMovies = 0, nTracks = 0, bad = 0;
    for (size_t a = 0; a < archives.size(); ++a) {
        const std::string stem = archives[a].stem().string();
        if (progress && !progress(int(a), int(archives.size()), stem)) return false;
        std::vector<uint8_t> d;
        if (!ReadFile(archives[a], d)) { std::printf("prepare: can't read %s\n", archives[a].u8string().c_str()); ++bad; continue; }

        // sounds
        for (const Res& r : Resources(d)) {
            if (r.tag != "tWAV" || r.off + r.size > d.size()) continue;
            std::vector<int16_t> pcm;
            int rate = 0, ch = 0;
            if (!DecodeTWav(&d[r.off], r.size, pcm, rate, ch)) { ++bad; continue; }
            if (WriteWav(sounds / (Upper(stem) + "_" + std::to_string(r.id) + ".wav"), pcm, rate, ch)) ++nSounds;
            else ++bad;
        }

        // movies: every 'moov' atom in the file, copied out with its mdat (as the original RUGRATS.EXE finds them)
        size_t pos = 0;
        for (;;) {
            const uint8_t kMoov[4] = {'m', 'o', 'o', 'v'};
            auto it = std::search(d.begin() + std::ptrdiff_t(pos), d.end(), kMoov, kMoov + 4);
            if (it == d.end()) break;
            const size_t i = size_t(it - d.begin());
            pos = i + 4;
            if (i < 4) continue;
            const size_t moovOff = i - 4;
            const uint32_t moovSz = BE32(&d[moovOff]);
            if (moovSz < 8 || moovOff + moovSz > d.size()) continue;
            std::vector<uint8_t> moov(d.begin() + std::ptrdiff_t(moovOff), d.begin() + std::ptrdiff_t(moovOff + moovSz));
            std::vector<std::pair<size_t, bool>> tables;                 // stco (false) / co64 (true)
            Walk(moov, 0, moov.size(), [&](size_t o, size_t, const uint8_t* t) {
                if (!std::memcmp(t, "stco", 4)) tables.emplace_back(o, false);
                else if (!std::memcmp(t, "co64", 4)) tables.emplace_back(o, true);
            });
            if (tables.empty()) continue;
            uint64_t lo = UINT64_MAX, hi = 0;
            bool okTables = true;
            for (auto [o, wide] : tables) {
                if (o + 16 > moov.size()) { okTables = false; break; }
                const uint32_t n = BE32(&moov[o + 12]);
                const size_t w = wide ? 8 : 4;
                if (o + 16 + size_t(n) * w > moov.size()) { okTables = false; break; }
                for (uint32_t k = 0; k < n; ++k) {
                    const uint64_t v = wide ? BE64(&moov[o + 16 + k * w]) : BE32(&moov[o + 16 + k * w]);
                    lo = std::min(lo, v); hi = std::max(hi, v);
                }
            }
            if (!okTables || lo == UINT64_MAX) continue;
            // Mohawk-compiled movies nest extra (sometimes zero-size) 'mdat' headers inside the
            // real one: take the outermost mdat whose extent covers every chunk
            size_t mdatOff = 0, mdatSz = 0;
            const uint8_t kMdat[4] = {'m', 'd', 'a', 't'};
            for (auto j = std::search(d.begin(), d.begin() + std::ptrdiff_t(std::min<uint64_t>(lo, d.size())), kMdat, kMdat + 4);
                 j != d.begin() + std::ptrdiff_t(std::min<uint64_t>(lo, d.size()));
                 j = std::search(j + 1, d.begin() + std::ptrdiff_t(std::min<uint64_t>(lo, d.size())), kMdat, kMdat + 4)) {
                const size_t jj = size_t(j - d.begin());
                if (jj < 4) continue;
                const uint32_t sz = BE32(&d[jj - 4]);
                if (sz >= 8 && jj - 4 < lo && hi < uint64_t(jj - 4) + sz && uint64_t(jj - 4) + sz <= d.size()) { mdatOff = jj - 4; mdatSz = sz; break; }
            }
            if (!mdatSz) continue;
            const uint64_t key = Fnv1a64(moov.data(), moov.size());     // of the ORIGINAL moov
            const int64_t rebase = int64_t(mdatOff) - int64_t(moovSz);   // output layout: [moov][mdat]
            for (auto [o, wide] : tables) {
                const uint32_t n = BE32(&moov[o + 12]);
                const size_t w = wide ? 8 : 4;
                for (uint32_t k = 0; k < n; ++k) {
                    uint8_t* p = &moov[o + 16 + k * w];
                    if (wide) PutBE64(p, uint64_t(int64_t(BE64(p)) - rebase));
                    else PutBE32(p, uint32_t(int64_t(BE32(p)) - rebase));
                }
            }
            char hex[16];
            std::snprintf(hex, sizeof hex, "%08zx", moovOff);
            const std::string base = stem + "_" + hex;
            {
                std::ofstream f(movies / (base + ".mov"), std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(moov.data()), std::streamsize(moov.size()));
                f.write(reinterpret_cast<const char*>(&d[mdatOff]), std::streamsize(mdatSz));
                if (!f) { ++bad; continue; }
            }
            manifest.push_back(Entry{key, base, Info(moov)});
            ++nMovies;
            if (MovieAudio(movies / (base + ".mov"), movies / (base + ".wav"))) ++nTracks;
        }
    }
    {
        std::ofstream m(movies / "movies.manifest", std::ios::binary | std::ios::trunc);
        m << "; key(fnv1a64 of moov)  file  timescale  duration  loop  width  height\r\n";
        for (const Entry& e : manifest) {
            char line[256];
            std::snprintf(line, sizeof line, "%016llx %s.mp4 %u %u %d %u %u\r\n", static_cast<unsigned long long>(e.key),
                          e.base.c_str(), e.info.scale, e.info.duration, e.info.loop ? 1 : 0, e.info.w, e.info.h);
            m << line;
        }
    }
    if (progress) progress(int(archives.size()), int(archives.size()), "");
    std::printf("prepare: %zu archives, %zu movies (%zu soundtracks), %zu sounds, %zu problems\n",
                archives.size(), nMovies, nTracks, nSounds, bad);
    return nMovies > 0 && nSounds > 0;
}

bool AssetsPrepared(const std::string& dir) {
    std::error_code ec;
    const fs::path d = fs::u8path(dir);
    return fs::exists(d / "movies" / "movies.manifest", ec) && fs::is_directory(d / "sounds", ec) &&
           !fs::is_empty(d / "sounds", ec);
}

} // namespace port
