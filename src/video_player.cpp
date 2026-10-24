#include "video_player.h"
#include <cmath>

namespace port {

bool VideoPlayer::Open(const MovieAsset& m) {
    asset_ = m;
    decoder_ = qtshim::CreateMovieDecoder();
    time_ = 0;
    playing_ = false;
    return decoder_->Open(m.file);
}

void VideoPlayer::Update(double seconds) {
    if (!playing_ || !decoder_) return;
    time_ += seconds * asset_.timeScale;
    if (time_ < asset_.duration) return;
    if (asset_.loop) {
        time_ = std::fmod(time_, double(asset_.duration));
    } else {
        time_ = asset_.duration;                     // hold the last frame, like the engine
        playing_ = false;
    }
}

const qtshim::DecodedFrame* VideoPlayer::Frame() {
    if (!decoder_) return nullptr;
    return decoder_->FrameAt(int64_t(time_) * 600 / asset_.timeScale);
}

} // namespace port
