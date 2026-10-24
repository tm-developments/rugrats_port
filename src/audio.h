// audio.h - sound output for the port: plays .wav files (movie soundtracks and the
// sounds extracted from the archives' tWAV resources) through one SDL audio device.
//
// Everything is decoded ahead of time (prepare.cpp: --prepare or the first run), so this
// is only a small mixer: each playing sound is a voice,
// voices are summed with a master volume (used for fades) and clipped.
#pragma once
#include <SDL.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace port {

class Audio {
public:
    ~Audio();
    bool Init();                                   // false: no sound device (game still runs)
    bool Ready() const { return dev_ != 0; }
    // Starts a sound; returns a voice handle (0 = file missing / no device).
    int Play(const std::string& wavPath, bool loop = false);
    void Stop(int voice);
    void StopAll();
    void Pause(int voice, bool paused);
    bool Playing(int voice) const;
    void Seek(int voice, double seconds);          // e.g. to follow a restarted movie
    double Position(int voice) const;              // seconds played (< 0: not playing)
    void SetMaster(float v);                       // 0..1, for fades
    // the player's Options > Volume (Mute, Increase, Decrease): a level on top of the fades
    void SetVolume(float v, bool muted);
    // the whole output stops while the game itself is held (Windows' menu and move loops), so
    // sounds and the game clock stay together
    // (reasons: 1 a modal loop, 2 the window lost focus; the output runs when none is set)
    void PauseOutput(bool paused, int reason = 1) {
        pauseReasons_ = paused ? (pauseReasons_ | reason) : (pauseReasons_ & ~reason);
        if (dev_) SDL_PauseAudioDevice(dev_, pauseReasons_ ? 1 : 0);
    }
    double Duration(const std::string& wavPath);   // seconds (0 if missing); loads and caches the clip
    // MOHAWK.W32 [Audio.WaveRateTranslations]: the player plays sounds recorded at Mac rates
    // (11127, 22254 Hz) at the PC rates (11025, 22050 Hz). Set before sounds are loaded.
    void SetRateTranslation(int from, int to) { rateMap_[from] = to; }
    int TranslateRate(int hz) const { auto f = rateMap_.find(hz); return f == rateMap_.end() ? hz : f->second; }
    // The translation is the Mohawk player's, for its own sounds (tWAV, extracted under this
    // folder). Movie soundtracks are QuickTime's and play at their true rate, in step with
    // the picture (translated, a long cut scene drifted half a second behind its video).
    void SetTranslatedFolder(const std::string& prefix) { translatedPrefix_ = prefix; }
    int RateFor(const std::string& path, int hz) const {
        return translatedPrefix_.empty() || path.rfind(translatedPrefix_, 0) == 0 ? TranslateRate(hz) : hz;
    }
    static bool Exists(const std::string& path);

private:
    struct Clip { std::vector<int16_t> samples; };              // device format, interleaved
    struct Voice { std::shared_ptr<const Clip> clip; size_t pos = 0; bool loop = false, paused = false; };
    std::shared_ptr<const Clip> Load(const std::string& path);
    static void Callback(void* self, Uint8* out, int len);
    void Mix(int16_t* out, int samples);
    // Voices are shared with the audio callback; SDL's device lock guards them (the
    // callback itself always runs with that lock held).
    struct Lock {
        SDL_AudioDeviceID d;
        explicit Lock(SDL_AudioDeviceID dev) : d(dev) { if (d) SDL_LockAudioDevice(d); }
        ~Lock() { if (d) SDL_UnlockAudioDevice(d); }
    };

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
    std::map<std::string, std::shared_ptr<const Clip>> cache_;
    std::map<int, Voice> voices_;
    int next_ = 1;
    float master_ = 1.0f, volume_ = 1.0f;
    int pauseReasons_ = 0;
    std::map<int, int> rateMap_;
    std::string translatedPrefix_;
};

} // namespace port
