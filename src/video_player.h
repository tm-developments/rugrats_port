// video_player.h - plays one converted movie on the game clock.
#pragma once
#include "assets.h"
#include "video_decoder.h"
#include <memory>

namespace port {

class VideoPlayer {
public:
    bool Open(const MovieAsset& m);
    void Play() { if (AtEnd()) time_ = 0; playing_ = true; }
    void Pause() { playing_ = false; }
    void Toggle() { playing_ ? Pause() : Play(); }
    void Restart() { time_ = 0; playing_ = true; }
    void Update(double seconds);                     // advance the movie clock
    // follow the soundtrack: the picture is shown at the sound's position (as QuickTime
    // keeps them together), so a stall while a movie opens can't leave the picture behind
    void SyncTo(double seconds) {
        if (!playing_) return;
        time_ = seconds * asset_.timeScale;
        Update(0);
    }
    const qtshim::DecodedFrame* Frame();             // frame due at the current time
    bool Playing() const { return playing_; }
    bool AtEnd() const { return time_ >= asset_.duration; }
    double Time() const { return time_; }            // movie timescale units
    const MovieAsset& Asset() const { return asset_; }
    void SetChannelEnabled(int channel, bool on) { if (decoder_) decoder_->SetChannelEnabled(channel, on); }
    int Channels() const { return decoder_ ? decoder_->Channels() : 0; }

private:
    MovieAsset asset_;
    std::unique_ptr<qtshim::IVideoDecoder> decoder_;
    double time_ = 0;
    bool playing_ = false;
};

} // namespace port
