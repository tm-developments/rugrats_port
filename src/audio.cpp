#include "audio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace port {

Audio::~Audio() {
    if (dev_) SDL_CloseAudioDevice(dev_);
}

bool Audio::Init() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::printf("audio: %s (continuing without sound)\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = 22050;                             // the game's own rate
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &Audio::Callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &spec_, 0);   // exact format: SDL converts
    if (!dev_) {
        std::printf("audio: %s (continuing without sound)\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

bool Audio::Exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

std::shared_ptr<const Audio::Clip> Audio::Load(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second;
    std::shared_ptr<const Clip> result;
    SDL_AudioSpec src{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (SDL_LoadWAV(path.c_str(), &src, &buf, &len)) {
        src.freq = RateFor(path, src.freq);
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, src.format, src.channels, src.freq, spec_.format, spec_.channels, spec_.freq) >= 0) {
            std::vector<Uint8> work(size_t(len) * size_t(std::max(1, cvt.len_mult)));
            std::memcpy(work.data(), buf, len);
            cvt.buf = work.data();
            cvt.len = int(len);
            if (cvt.needed == 0 || SDL_ConvertAudio(&cvt) == 0) {
                auto c = std::make_shared<Clip>();
                const size_t bytes = cvt.needed ? size_t(cvt.len_cvt) : size_t(len);
                c->samples.resize(bytes / 2);
                std::memcpy(c->samples.data(), work.data(), c->samples.size() * 2);
                result = c;
            }
        }
        SDL_FreeWAV(buf);
    }
    if (!result) std::printf("audio: can't load %s: %s\n", path.c_str(), SDL_GetError());
    return cache_[path] = result;
}

double Audio::Duration(const std::string& path) {
    if (!dev_) {
        SDL_AudioSpec s{};
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path.c_str(), &s, &buf, &len)) return 0;
        const double sec = double(len) / (double(SDL_AUDIO_BITSIZE(s.format) / 8) * s.channels * RateFor(path, s.freq));
        SDL_FreeWAV(buf);
        return sec;
    }
    auto clip = Load(path);
    if (!clip) return 0;
    return double(clip->samples.size()) / (double(spec_.channels) * spec_.freq);
}

int Audio::Play(const std::string& path, bool loop) {
    if (!dev_) return 0;
    auto clip = Load(path);
    if (!clip || clip->samples.empty()) return 0;
    Lock g(dev_);
    const int id = next_++;
    voices_[id] = Voice{clip, 0, loop, false};
    return id;
}

void Audio::Stop(int v) { Lock g(dev_); voices_.erase(v); }
void Audio::StopAll() { Lock g(dev_); voices_.clear(); }
void Audio::SetMaster(float v) { Lock g(dev_); master_ = std::clamp(v, 0.0f, 1.0f); }
void Audio::SetVolume(float v, bool muted) { Lock g(dev_); volume_ = muted ? 0.0f : std::clamp(v, 0.0f, 1.0f); }

void Audio::Pause(int v, bool p) {
    Lock g(dev_);
    auto it = voices_.find(v);
    if (it != voices_.end()) it->second.paused = p;
}

bool Audio::Playing(int v) const {
    Lock g(dev_);
    return voices_.count(v) != 0;
}

double Audio::Position(int v) const {
    Lock g(dev_);
    auto it = voices_.find(v);
    if (it == voices_.end() || !spec_.freq || !spec_.channels) return -1;
    return double(it->second.pos) / (double(spec_.channels) * spec_.freq);
}

void Audio::Seek(int v, double seconds) {
    Lock g(dev_);
    auto it = voices_.find(v);
    if (it == voices_.end()) return;
    const size_t pos = size_t(std::max(0.0, seconds) * spec_.freq) * spec_.channels;
    it->second.pos = std::min(pos, it->second.clip->samples.size());
}

void Audio::Callback(void* self, Uint8* out, int len) {
    static_cast<Audio*>(self)->Mix(reinterpret_cast<int16_t*>(out), len / 2);
}

void Audio::Mix(int16_t* out, int samples) {
    std::vector<int32_t> acc(size_t(samples), 0);
    for (auto it = voices_.begin(); it != voices_.end();) {
        Voice& v = it->second;
        const auto& s = v.clip->samples;
        for (int i = 0; i < samples && !v.paused; ++i) {
            if (v.pos >= s.size()) { if (!v.loop) break; v.pos = 0; }
            acc[size_t(i)] += s[v.pos++];
        }
        if (!v.loop && v.pos >= s.size()) it = voices_.erase(it);
        else ++it;
    }
    const float gain = master_ * volume_ * volume_;    // squared: the steps sound even to the ear
    for (int i = 0; i < samples; ++i)
        out[i] = int16_t(std::clamp(int32_t(float(acc[size_t(i)]) * gain), -32768, 32767));
}

} // namespace port
