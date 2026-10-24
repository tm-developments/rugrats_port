// video_decoder.h - decoder interface consumed by the shim core.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace qtshim {

// One decoded picture, always top-down BGRA (B,G,R,A bytes in memory).
struct DecodedFrame {
    const uint8_t* bgra = nullptr;
    int width = 0, height = 0, stride = 0;
    int64_t pts600 = -1;            // presentation time, 1/600 s (the movies' timescale)
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;
    virtual bool Open(const std::string& path) = 0;
    // Latest frame whose pts <= t (seeking backwards to a keyframe if needed).
    // Returns nullptr only if nothing can be decoded.
    virtual const DecodedFrame* FrameAt(int64_t t600) = 0;
    // QuickTime "channels": video tracks in file order, 1-based (showchannel / hidechannel).
    virtual void SetChannelEnabled(int /*channel*/, bool /*enabled*/) {}
    virtual int Channels() const { return 1; }
};

// Implemented in video_decoder_libav.cpp (libavformat + libavcodec + libswscale).
std::unique_ptr<IVideoDecoder> CreateLibavDecoder();
// Plays a QuickTime movie the way QuickTime composites it: every video track at its
// matrix offset, back to front by layer, "transparent" graphics mode keyed on the op color,
// each track only during its own (edit-list) time span. Falls back to one stream for
// single-track files.
std::unique_ptr<IVideoDecoder> CreateMovieDecoder();

} // namespace qtshim
