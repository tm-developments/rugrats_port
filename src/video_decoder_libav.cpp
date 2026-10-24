// video_decoder_libav.cpp - the game's QuickTime movies (Cinepak, Apple SMC) -> BGRA frames via FFmpeg.
#include "devenv.h"
#include "video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace qtshim {
namespace {

// One video track: frames by time, plus the track's span and size.
class TrackDecoder : public IVideoDecoder {
public:
    virtual int64_t Start600() const = 0;
    virtual int64_t End600() const = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
};

class LibavDecoder final : public TrackDecoder {
public:
    ~LibavDecoder() override {
        sws_freeContext(sws_);
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
        avformat_close_input(&fmt_);
    }

    bool Open(const std::string& path) override { return OpenStream(path, -1); }

    // stream < 0: the best video stream
    bool OpenStream(const std::string& path, int stream) {
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;
        stream_ = stream >= 0 ? stream : av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_ < 0 || stream_ >= int(fmt_->nb_streams)) return false;
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)
            if (int(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;
        AVStream* st = fmt_->streams[stream_];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        if (avcodec_parameters_to_context(ctx_, st->codecpar) < 0) return false;
        ctx_->thread_count = 0;                        // auto
        if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
        tb_ = st->time_base;
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        out_.width = ctx_->width;
        out_.height = ctx_->height;
        out_.stride = ctx_->width * 4;
        pixels_.assign(size_t(out_.stride) * out_.height, 0);
        out_.bgra = pixels_.data();
        return frame_ && pkt_;
    }

    // stream span in 1/600 s: [start, end)
    int64_t Start600() const override {
        const AVStream* st = fmt_->streams[stream_];
        return st->start_time == AV_NOPTS_VALUE ? 0 : To600(st->start_time);
    }
    int64_t End600() const override {
        const AVStream* st = fmt_->streams[stream_];
        return st->duration == AV_NOPTS_VALUE ? INT64_MAX : Start600() + To600(st->duration);
    }
    int StreamId() const { return fmt_->streams[stream_]->id; }
    int Width() const override { return out_.width; }
    int Height() const override { return out_.height; }

    const DecodedFrame* FrameAt(int64_t t600) override {
        if (t600 < 0) t600 = 0;
        // Rewind on backwards seeks, or on long jumps forward (GoToTime, palindrome loop).
        if (t600 < out_.pts600 || (out_.pts600 >= 0 && t600 - out_.pts600 > 600 * 2)) Seek(t600);
        // Decode forward while the next frame is still due at or before t.
        while (!eof_) {
            if (pending_ && pendingPts_ > t600) break;
            if (pending_) { Commit(); continue; }
            if (!Pull()) break;
        }
        if (out_.pts600 < 0 && pending_) Commit();    // nothing <= t yet: show the first frame
        return out_.pts600 >= 0 ? &out_ : nullptr;
    }

private:
    int64_t To600(int64_t ts) const { return av_rescale_q(ts, tb_, AVRational{1, 600}); }

    void Seek(int64_t t600) {
        int64_t ts = av_rescale_q(t600, AVRational{1, 600}, tb_);
        av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx_);
        eof_ = false; pending_ = false; out_.pts600 = -1; draining_ = false;
    }

    // Decode one frame into frame_ (pending). False at end of stream.
    bool Pull() {
        for (;;) {
            int r = avcodec_receive_frame(ctx_, frame_);
            if (r == 0) {
                int64_t ts = frame_->best_effort_timestamp;
                pendingPts_ = ts == AV_NOPTS_VALUE ? out_.pts600 + 1 : To600(ts);
                pending_ = true;
                return true;
            }
            if (r == AVERROR_EOF) { eof_ = true; return false; }
            if (draining_) { eof_ = true; return false; }
            // r == EAGAIN: feed another packet
            for (;;) {
                if (av_read_frame(fmt_, pkt_) < 0) {
                    avcodec_send_packet(ctx_, nullptr); draining_ = true; break;
                }
                bool ours = pkt_->stream_index == stream_;
                if (ours) avcodec_send_packet(ctx_, pkt_);
                av_packet_unref(pkt_);
                if (ours) break;
            }
        }
    }

    void Commit() {
        sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, AVPixelFormat(frame_->format),
                                    out_.width, out_.height, AV_PIX_FMT_BGRA,
                                    SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
        uint8_t* dst[4] = {pixels_.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {out_.stride, 0, 0, 0};
        sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dstStride);
        out_.pts600 = pendingPts_;
        pending_ = false;
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVRational tb_{1, 600};
    int stream_ = -1;
    bool eof_ = false, pending_ = false, draining_ = false;
    int64_t pendingPts_ = -1;
    DecodedFrame out_;
    std::vector<uint8_t> pixels_;
};


// ---- QuickTime track compositing -----------------------------------------------------------
struct TrackInfo {
    int id = 0, layer = 0, tx = 0, ty = 0;
    bool enabled = true, video = false, transparent = false;
    uint8_t key[3] = {0, 0, 0};          // op color (R, G, B) for the transparent mode
    int descs = 0;                       // sample descriptions (codecs) in the track
};

uint32_t RdBE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Walk atoms in [b, e); calls f(type, body, bodyEnd).
template <class F> void Atoms(const uint8_t* b, const uint8_t* e, F f) {
    while (e - b >= 8) {
        uint64_t sz = RdBE32(b);
        const uint32_t type = RdBE32(b + 4);
        size_t hdr = 8;
        if (sz == 1 && e - b >= 16) { sz = uint64_t(RdBE32(b + 8)) << 32 | RdBE32(b + 12); hdr = 16; }
        if (sz == 0) sz = uint64_t(e - b);
        if (sz < hdr || sz > uint64_t(e - b)) return;
        f(type, b + hdr, b + sz);
        b += sz;
    }
}
constexpr uint32_t FourCC(const char* s) { return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]); }

std::vector<TrackInfo> ReadTracks(const std::string& path) {
    std::vector<TrackInfo> out;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty()) return out;
    const uint8_t* fb = file.data();
    Atoms(fb, fb + file.size(), [&](uint32_t t, const uint8_t* b, const uint8_t* e) {
        if (t != FourCC("moov")) return;
        Atoms(b, e, [&](uint32_t t2, const uint8_t* b2, const uint8_t* e2) {
            if (t2 != FourCC("trak")) return;
            TrackInfo ti;
            Atoms(b2, e2, [&](uint32_t t3, const uint8_t* b3, const uint8_t* e3) {
                if (t3 == FourCC("tkhd") && e3 - b3 >= 84) {
                    ti.enabled = (RdBE32(b3) & 1) != 0;
                    ti.id = int(RdBE32(b3 + 12));
                    ti.layer = int16_t(uint16_t(b3[32] << 8 | b3[33]));
                    ti.tx = int32_t(RdBE32(b3 + 40 + 24)) / 65536;
                    ti.ty = int32_t(RdBE32(b3 + 40 + 28)) / 65536;
                }
                if (t3 != FourCC("mdia")) return;
                Atoms(b3, e3, [&](uint32_t t4, const uint8_t* b4, const uint8_t* e4) {
                    if (t4 == FourCC("hdlr") && e4 - b4 >= 12) ti.video = RdBE32(b4 + 8) == FourCC("vide");
                    if (t4 != FourCC("minf")) return;
                    Atoms(b4, e4, [&](uint32_t t5, const uint8_t* b5, const uint8_t* e5) {
                        if (t5 == FourCC("stbl"))
                            Atoms(b5, e5, [&](uint32_t t6, const uint8_t* b6, const uint8_t* e6) {
                                if (t6 == FourCC("stsd") && e6 - b6 >= 8) ti.descs = int(RdBE32(b6 + 4));
                            });
                        if (t5 != FourCC("vmhd") || e5 - b5 < 12) return;
                        const uint16_t mode = uint16_t(b5[4] << 8 | b5[5]);
                        ti.transparent = mode == 0x24;            // QuickTime "transparent"
                        for (int k = 0; k < 3; ++k) ti.key[k] = b5[6 + 2 * k];   // high byte of each 16-bit channel
                    });
                });
            });
            out.push_back(ti);
        });
    });
    return out;
}

// A QuickTime video track decoded straight from its sample tables. Many of the game's
// movies change codec part-way (Apple Graphics 'smc ' for the first frames, Cinepak after:
// two sample descriptions in one track), which libavformat can't follow - it keeps only the
// first codec and drops the rest, so the movie freezes on its opening frames. Here every
// sample goes to the decoder of its own description, all drawing into one picture, as
// QuickTime does.
class QtTrackDecoder final : public TrackDecoder {
public:
    ~QtTrackDecoder() override {
        for (Desc& d : descs_) avcodec_free_context(&d.ctx);
        sws_freeContext(sws_);
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
    }
    bool Open(const std::string& path) override { return OpenTrack(path, -1); }

    // trackId < 0: the first video track
    bool OpenTrack(const std::string& path, int trackId) {
        std::ifstream in(path, std::ios::binary);
        file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (file_.empty()) return false;
        const uint8_t* fb = file_.data();
        bool found = false;
        uint32_t movieScale = 600;
        Atoms(fb, fb + file_.size(), [&](uint32_t t, const uint8_t* b, const uint8_t* e) {
            if (t != FourCC("moov") || found) return;
            Atoms(b, e, [&](uint32_t t2, const uint8_t* b2, const uint8_t* e2) {
                if (t2 == FourCC("mvhd") && e2 - b2 >= 20) movieScale = std::max<uint32_t>(1, RdBE32(b2 + 12));
            });
            Atoms(b, e, [&](uint32_t t2, const uint8_t* b2, const uint8_t* e2) {
                if (t2 != FourCC("trak") || found) return;
                if (ParseTrak(b2, e2, trackId, movieScale)) found = true;
                else Reset();
            });
        });
        if (!found || samples_.empty() || descs_.empty()) return false;
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!frame_ || !pkt_) return false;
        for (Desc& d : descs_) {
            const AVCodec* c = avcodec_find_decoder(d.codec);
            if (!c) continue;
            d.ctx = avcodec_alloc_context3(c);
            d.ctx->width = d.w; d.ctx->height = d.h;
            d.ctx->bits_per_coded_sample = d.depth;
            d.ctx->codec_tag = d.fourcc;
            if (avcodec_open2(d.ctx, c, nullptr) < 0) avcodec_free_context(&d.ctx);
        }
        out_.width = width_ > 0 ? width_ : descs_[0].w;
        out_.height = height_ > 0 ? height_ : descs_[0].h;
        out_.stride = out_.width * 4;
        pixels_.assign(size_t(out_.stride) * size_t(out_.height), 0);
        out_.bgra = pixels_.data();
        return true;
    }

    int64_t Start600() const override { return start600_; }
    int64_t End600() const override { return end600_; }
    int Width() const override { return out_.width; }
    int Height() const override { return out_.height; }

    const DecodedFrame* FrameAt(int64_t t600) override {
        if (samples_.empty()) return nullptr;
        // the sample showing at t (or the first one, before the track starts)
        size_t want = 0;
        {
            size_t lo = 0, hi = samples_.size();
            while (hi - lo > 1) { const size_t mid = (lo + hi) / 2; if (samples_[mid].t600 <= t600) lo = mid; else hi = mid; }
            want = lo;
        }
        if (int64_t(want) == shown_) return &out_;
        // decode forward from the current picture, or from the key frame at or before it
        size_t from = shown_ >= 0 && int64_t(want) > shown_ ? size_t(shown_ + 1) : want;
        if (!(shown_ >= 0 && int64_t(want) > shown_)) {
            while (from > 0 && !samples_[from].key) --from;
            for (Desc& d : descs_) if (d.ctx) avcodec_flush_buffers(d.ctx);
        } else {
            // a long way ahead: start again from the nearest key frame instead
            size_t k = want;
            while (k > from && !samples_[k].key) --k;
            if (k > from) { from = k; for (Desc& d : descs_) if (d.ctx) avcodec_flush_buffers(d.ctx); }
        }
        for (size_t i = from; i <= want; ++i) Decode(i);
        shown_ = int64_t(want);
        out_.pts600 = samples_[want].t600;
        return &out_;
    }

private:
    struct Sample { uint64_t off; uint32_t size; int64_t t600; uint16_t desc; bool key; };
    struct Desc {
        uint32_t fourcc = 0;
        AVCodecID codec = AV_CODEC_ID_NONE;
        int w = 0, h = 0, depth = 0;
        std::vector<uint32_t> palette;   // ARGB, from the description's colour table
        AVCodecContext* ctx = nullptr;
    };

    void Reset() { samples_.clear(); descs_.clear(); start600_ = 0; end600_ = INT64_MAX; width_ = height_ = 0; }

    static AVCodecID CodecFor(uint32_t f) {
        if (f == FourCC("smc ")) return AV_CODEC_ID_SMC;
        if (f == FourCC("cvid")) return AV_CODEC_ID_CINEPAK;
        if (f == FourCC("rle ")) return AV_CODEC_ID_QTRLE;
        if (f == FourCC("rpza")) return AV_CODEC_ID_RPZA;
        if (f == FourCC("jpeg")) return AV_CODEC_ID_MJPEG;
        return AV_CODEC_ID_NONE;
    }

    bool ParseTrak(const uint8_t* b, const uint8_t* e, int trackId, uint32_t movieScale) {
        int id = -1;
        bool video = false;
        int64_t emptyEdit = 0;                       // leading empty edit, movie timescale
        uint32_t scale = 600;
        std::vector<std::pair<uint32_t, uint32_t>> stts, stsc3a;
        std::vector<std::array<uint32_t, 3>> stsc;
        std::vector<uint32_t> stss, sizes;
        uint32_t fixedSize = 0, sampleCount = 0;
        std::vector<uint64_t> chunks;
        Atoms(b, e, [&](uint32_t t, const uint8_t* b1, const uint8_t* e1) {
            if (t == FourCC("tkhd") && e1 - b1 >= 84) {
                id = int(RdBE32(b1 + 12));
                width_ = int(RdBE32(b1 + 76) >> 16);
                height_ = int(RdBE32(b1 + 80) >> 16);
            }
            if (t == FourCC("edts"))
                Atoms(b1, e1, [&](uint32_t t2, const uint8_t* b2, const uint8_t* e2) {
                    if (t2 != FourCC("elst") || e2 - b2 < 8) return;
                    const uint32_t n = RdBE32(b2 + 4);
                    if (n >= 1 && e2 - b2 >= 20 && int32_t(RdBE32(b2 + 12)) == -1) emptyEdit = RdBE32(b2 + 8);
                });
            if (t != FourCC("mdia")) return;
            Atoms(b1, e1, [&](uint32_t t2, const uint8_t* b2, const uint8_t* e2) {
                if (t2 == FourCC("mdhd") && e2 - b2 >= 20) scale = std::max<uint32_t>(1, RdBE32(b2 + 12));
                if (t2 == FourCC("hdlr") && e2 - b2 >= 12) video = RdBE32(b2 + 8) == FourCC("vide");
                if (t2 != FourCC("minf")) return;
                Atoms(b2, e2, [&](uint32_t t3, const uint8_t* b3, const uint8_t* e3) {
                    if (t3 != FourCC("stbl")) return;
                    Atoms(b3, e3, [&](uint32_t t4, const uint8_t* p, const uint8_t* pe) {
                        const size_t len = size_t(pe - p);
                        if (len < 8) return;
                        const uint32_t n = RdBE32(p + 4);
                        if (t4 == FourCC("stsd")) {
                            const uint8_t* q = p + 8;
                            for (uint32_t k = 0; k < n && pe - q >= 16; ++k) {
                                const uint32_t sz = RdBE32(q);
                                if (sz < 16 || sz > uint32_t(pe - q)) break;
                                Desc d;
                                d.fourcc = RdBE32(q + 4);
                                d.codec = CodecFor(d.fourcc);
                                if (sz >= 86) {
                                    d.w = int(uint16_t(q[32] << 8 | q[33]));
                                    d.h = int(uint16_t(q[34] << 8 | q[35]));
                                    d.depth = int(int16_t(uint16_t(q[82] << 8 | q[83])));
                                    const int16_t ct = int16_t(uint16_t(q[84] << 8 | q[85]));
                                    if (ct == 0 && sz >= 94 && (d.depth & 0x1f) <= 8) {
                                        const int count = int(uint16_t(q[92] << 8 | q[93])) + 1;
                                        d.palette.assign(256, 0xff000000u);
                                        for (int c = 0; c < count && 94 + size_t(c + 1) * 8 <= sz; ++c) {
                                            const uint8_t* ce = q + 94 + size_t(c) * 8;
                                            const int ix = c & 0xff;   // entries in order (the value field isn't an index in device tables)
                                            d.palette[size_t(ix)] = 0xff000000u | uint32_t(ce[2]) << 16 | uint32_t(ce[4]) << 8 | ce[6];
                                        }
                                    }
                                }
                                descs_.push_back(std::move(d));
                                q += sz;
                            }
                        } else if (t4 == FourCC("stts")) {
                            for (uint32_t k = 0; k < n && 8 + size_t(k + 1) * 8 <= len; ++k) stts.emplace_back(RdBE32(p + 8 + k * 8), RdBE32(p + 12 + k * 8));
                        } else if (t4 == FourCC("stss")) {
                            for (uint32_t k = 0; k < n && 8 + size_t(k + 1) * 4 <= len; ++k) stss.push_back(RdBE32(p + 8 + k * 4));
                        } else if (t4 == FourCC("stsc")) {
                            for (uint32_t k = 0; k < n && 8 + size_t(k + 1) * 12 <= len; ++k)
                                stsc.push_back({RdBE32(p + 8 + k * 12), RdBE32(p + 12 + k * 12), RdBE32(p + 16 + k * 12)});
                        } else if (t4 == FourCC("stsz") && len >= 12) {
                            fixedSize = RdBE32(p + 4);
                            sampleCount = RdBE32(p + 8);
                            if (!fixedSize) for (uint32_t k = 0; k < sampleCount && 12 + size_t(k + 1) * 4 <= len; ++k) sizes.push_back(RdBE32(p + 12 + k * 4));
                        } else if (t4 == FourCC("stco")) {
                            for (uint32_t k = 0; k < n && 8 + size_t(k + 1) * 4 <= len; ++k) chunks.push_back(RdBE32(p + 8 + k * 4));
                        } else if (t4 == FourCC("co64")) {
                            for (uint32_t k = 0; k < n && 8 + size_t(k + 1) * 8 <= len; ++k)
                                chunks.push_back(uint64_t(RdBE32(p + 8 + k * 8)) << 32 | RdBE32(p + 12 + k * 8));
                        }
                    });
                });
            });
        });
        (void)stsc3a;
        if (!video || (trackId >= 0 && id != trackId)) return false;
        if (descs_.empty() || chunks.empty() || stsc.empty()) return false;
        if (!sampleCount) sampleCount = uint32_t(sizes.size());
        // sample -> (chunk, description), offsets inside the chunk
        std::vector<uint64_t> offs;
        std::vector<uint16_t> descOf;
        uint32_t s = 0;
        for (size_t c = 0; c < chunks.size() && s < sampleCount; ++c) {
            size_t e2 = 0;
            while (e2 + 1 < stsc.size() && stsc[e2 + 1][0] <= c + 1) ++e2;
            uint64_t o = chunks[c];
            for (uint32_t k = 0; k < stsc[e2][1] && s < sampleCount; ++k, ++s) {
                const uint32_t sz = fixedSize ? fixedSize : sizes[s];
                offs.push_back(o);
                descOf.push_back(uint16_t(std::max<uint32_t>(1, stsc[e2][2]) - 1));
                o += sz;
            }
        }
        const int64_t base600 = emptyEdit * 600 / movieScale;
        int64_t tm = 0;
        size_t si = 0;
        std::sort(stss.begin(), stss.end());
        for (const auto& [count, delta] : stts) {
            for (uint32_t k = 0; k < count && si < offs.size(); ++k, ++si) {
                Sample smp;
                smp.off = offs[si];
                smp.size = fixedSize ? fixedSize : sizes[si];
                smp.t600 = base600 + tm * 600 / scale;
                smp.desc = descOf[si] < descs_.size() ? descOf[si] : 0;
                smp.key = stss.empty() || std::binary_search(stss.begin(), stss.end(), uint32_t(si + 1));
                samples_.push_back(smp);
                tm += delta;
            }
        }
        if (samples_.empty()) return false;
        samples_.front().key = true;
        start600_ = base600;
        end600_ = base600 + tm * 600 / scale;
        return true;
    }

    void Decode(size_t i) {
        const Sample& s = samples_[i];
        Desc& d = descs_[s.desc];
        if (!d.ctx || s.off + s.size > file_.size()) return;
        av_packet_unref(pkt_);
        if (av_new_packet(pkt_, int(s.size)) < 0) return;
        std::memcpy(pkt_->data, file_.data() + s.off, s.size);
        if (s.key) pkt_->flags |= AV_PKT_FLAG_KEY;
        if (!d.palette.empty()) {
            if (uint8_t* pal = av_packet_new_side_data(pkt_, AV_PKT_DATA_PALETTE, 1024))
                for (size_t k = 0; k < 256; ++k) { const uint32_t v = d.palette[k]; pal[k * 4] = uint8_t(v); pal[k * 4 + 1] = uint8_t(v >> 8); pal[k * 4 + 2] = uint8_t(v >> 16); pal[k * 4 + 3] = uint8_t(v >> 24); }
        }
        const int sr = avcodec_send_packet(d.ctx, pkt_);
        if (DevEnv("QTDEBUG")) std::fprintf(stderr, "sample %zu desc %u size %u key %d send %d\n", i, unsigned(s.desc), s.size, int(s.key), sr);
        if (sr < 0) return;
        while (avcodec_receive_frame(d.ctx, frame_) == 0) {
            if (DevEnv("QTDEBUG")) std::fprintf(stderr, "  frame %dx%d fmt %d\n", frame_->width, frame_->height, frame_->format);
            sws_ = sws_getCachedContext(sws_, frame_->width, frame_->height, AVPixelFormat(frame_->format),
                                        out_.width, out_.height, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
            uint8_t* dst[4] = {pixels_.data(), nullptr, nullptr, nullptr};
            int dstStride[4] = {out_.stride, 0, 0, 0};
            if (sws_) sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dstStride);
            av_frame_unref(frame_);
        }
    }

    std::vector<uint8_t> file_;
    std::vector<Sample> samples_;
    std::vector<Desc> descs_;
    int64_t start600_ = 0, end600_ = INT64_MAX, shown_ = -1;
    int width_ = 0, height_ = 0;
    SwsContext* sws_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    DecodedFrame out_;
    std::vector<uint8_t> pixels_;
};

class CompositeDecoder final : public IVideoDecoder {
public:
    bool Open(const std::string& path) override {
        const std::vector<TrackInfo> tracks = ReadTracks(path);
        int videoTracks = 0;
        for (const TrackInfo& t : tracks) videoTracks += t.video;
        if (videoTracks <= 1) {                          // the common case: one picture track
            for (const TrackInfo& t : tracks)
                if (t.video && t.descs > 1) {                // codec changes part-way: decode it ourselves
                    auto q = std::make_unique<QtTrackDecoder>();
                    if (!q->OpenTrack(path, t.id)) return false;
                    single_ = std::move(q);
                    return true;
                }
            auto l = std::make_unique<LibavDecoder>();
            if (!l->OpenStream(path, -1)) return false;
            single_ = std::move(l);
            return true;
        }
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }
        std::vector<std::pair<int, int>> streams;        // (stream index, track id), file order
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) streams.emplace_back(int(i), fmt->streams[i]->id);
        avformat_close_input(&fmt);
        for (auto [index, id] : streams) {
            Layer l;
            for (const TrackInfo& t : tracks) if (t.id == id) l.info = t;
            if (l.info.descs > 1) {
                auto q = std::make_unique<QtTrackDecoder>();
                if (!q->OpenTrack(path, id)) continue;
                l.dec = std::move(q);
            } else {
                auto d = std::make_unique<LibavDecoder>();
                if (!d->OpenStream(path, index)) continue;
                l.dec = std::move(d);
            }
            l.start = l.dec->Start600();
            if (DevEnv("QTDEBUG")) std::fprintf(stderr, "layer id %d descs %d start %lld end %lld %dx%d at %d,%d\n", id, l.info.descs, (long long)l.start, (long long)l.dec->End600(), l.dec->Width(), l.dec->Height(), l.info.tx, l.info.ty);
            l.end = l.dec->End600();
            out_.width = std::max(out_.width, l.info.tx + l.dec->Width());
            out_.height = std::max(out_.height, l.info.ty + l.dec->Height());
            layers_.push_back(std::move(l));
        }
        if (layers_.empty()) return false;
        // the base track sets the picture size
        for (const Layer& l : layers_) if (l.info.tx == 0 && l.info.ty == 0 && !l.info.transparent) { out_.width = l.dec->Width(); out_.height = l.dec->Height(); break; }
        out_.stride = out_.width * 4;
        pixels_.assign(size_t(out_.stride) * size_t(out_.height), 0);
        out_.bgra = pixels_.data();
        // draw order: larger layer numbers are further back
        order_.resize(layers_.size());
        for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
        std::stable_sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return layers_[a].info.layer > layers_[b].info.layer; });
        return true;
    }

    const DecodedFrame* FrameAt(int64_t t600) override {
        if (single_) return single_->FrameAt(t600);
        // after every track has ended (the movie runs on a little), QuickTime keeps the last picture
        bool active = false;
        for (const Layer& l : layers_) if (l.info.enabled && t600 >= l.start && t600 < l.end) active = true;
        if (!active && out_.pts600 >= 0) return &out_;
        std::fill(pixels_.begin(), pixels_.end(), 0);
        bool any = false;
        for (size_t idx : order_) {
            Layer& l = layers_[idx];
            if (!l.info.enabled || t600 < l.start || t600 >= l.end) continue;
            const DecodedFrame* f = l.dec->FrameAt(t600);
            if (!f) continue;
            Blend(*f, l.info);
            any = true;
        }
        out_.pts600 = t600;
        return any ? &out_ : nullptr;
    }

    void SetChannelEnabled(int channel, bool enabled) override {
        if (single_) return;
        if (channel >= 1 && channel <= int(layers_.size())) layers_[size_t(channel - 1)].info.enabled = enabled;
    }
    int Channels() const override { return single_ ? 1 : int(layers_.size()); }

private:
    struct Layer {
        TrackInfo info;
        std::unique_ptr<TrackDecoder> dec;
        int64_t start = 0, end = INT64_MAX;
    };

    void Blend(const DecodedFrame& f, const TrackInfo& t) {
        for (int y = 0; y < f.height; ++y) {
            const int dy = y + t.ty;
            if (dy < 0 || dy >= out_.height) continue;
            const uint8_t* s = f.bgra + size_t(y) * size_t(f.stride);
            uint8_t* d = pixels_.data() + size_t(dy) * size_t(out_.stride);
            for (int x = 0; x < f.width; ++x) {
                const int dx = x + t.tx;
                if (dx < 0 || dx >= out_.width) continue;
                const uint8_t* p = s + x * 4;
                if (t.transparent && std::abs(p[2] - t.key[0]) <= 2 && std::abs(p[1] - t.key[1]) <= 2 && std::abs(p[0] - t.key[2]) <= 2) continue;
                std::memcpy(d + dx * 4, p, 4);
                d[dx * 4 + 3] = 255;
            }
        }
    }

    std::unique_ptr<TrackDecoder> single_;
    std::vector<Layer> layers_;
    std::vector<size_t> order_;
    DecodedFrame out_;
    std::vector<uint8_t> pixels_;
};

} // namespace

std::unique_ptr<IVideoDecoder> CreateLibavDecoder() { return std::make_unique<LibavDecoder>(); }
std::unique_ptr<IVideoDecoder> CreateMovieDecoder() { return std::make_unique<CompositeDecoder>(); }

} // namespace qtshim
