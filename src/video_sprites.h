// video_sprites.h - Mohawk sprite resources: ANI, SHP#, SPR#, SCRP.
//
// Formats, worked out from RRQUIT.MHK / TOOLBAR.MHK / MAIN.MHK (all big-endian):
//   ANI   u16 1, Rect bounds (l,t,r,b, screen coords), Rect clip (l,t,r,b), 6 bytes 0,
//         u16 SHP#/SPR# id, u32 0                                           (30 bytes)
//   SHP#  u16 n, n x {u16 tBMP id, s16 regX, s16 regY}   "shapes"; reg = the bitmap's anchor
//   SPR#  n x 24-byte records of six u32: [?, seq id, 0, SCRP id, 0, flags]; record 0 holds
//         the default script, later records alternate ones (e.g. button press)
//   SCRP  ops {u8 op, u8 len, len bytes}: 05 = position (s16 x, s16 y) relative to the
//         ANI's bounds origin; 07 = select shape (u16, 1-based into SHP#); 06 = show one
//         frame; 0F = show the current picture for u32 N frames; 0A / 0B = start / stop a
//         tWAV (u16 id, u16 0); 00 = end; 09 / 0E = u16 setting (always 8 here; meaning
//         not confirmed, used as ticks per frame)
//   Placement: a frame's bitmap top-left = bounds.origin + position - shape registration.
//   Checked: the RRQUIT "Yes" frame lands exactly on its 73x66 bounds; the toolbar bar
//   lands at (0,400).
#pragma once
#include <SDL.h>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "mohawk.h"

namespace mohawk {

struct Rect16 { int16_t left = 0, top = 0, right = 0, bottom = 0; };

struct Animation {
    uint16_t id = 0;
    Rect16 bounds, clip;
    uint16_t listId = 0;                     // SHP# / SPR# id
};

struct Shape { uint16_t bitmapId = 0; int16_t regX = 0, regY = 0; };

struct SequenceRecord { uint32_t fields[6] = {}; uint16_t ScriptId() const { return uint16_t(fields[3]); } };

struct ScriptFrame {
    uint16_t shape = 0;                      // 0 = nothing drawn
    int16_t x = 0, y = 0;
    bool positioned = false;
    uint32_t hold = 1;                       // frame periods to show it (06 = 1, 0F = N)
};
struct SoundCue { size_t frame; bool start; uint16_t soundId; };   // 0A / 0B (not played yet)
struct Script {
    std::vector<ScriptFrame> frames;         // one per 06 / 0F op
    uint16_t ticksPerFrame = 8;              // from op 09/0E (meaning not confirmed)
    std::vector<SoundCue> sounds;
    std::vector<uint8_t> unknownOps;         // ops this parser does not understand
};

std::optional<Animation> ParseAnimation(uint16_t id, const std::vector<uint8_t>& d, std::string* err = nullptr);
std::optional<std::vector<Shape>> ParseShapes(const std::vector<uint8_t>& d, std::string* err = nullptr);
std::optional<std::vector<SequenceRecord>> ParseSequence(const std::vector<uint8_t>& d, std::string* err = nullptr);
std::optional<Script> ParseScript(const std::vector<uint8_t>& d, std::string* err = nullptr);

// Everything needed to draw one ANI from one archive.
struct SpriteDef {
    Animation ani;
    std::vector<Shape> shapes;
    std::vector<SequenceRecord> sequence;
    std::map<uint16_t, Script> scripts;      // the SCRPs the SPR# records refer to
};
std::optional<SpriteDef> LoadSprite(const Archive& a, uint16_t aniId, std::string* err = nullptr);

// Screen rectangle where shape `shapeIndex` (1-based) lands for a frame at (x, y).
SDL_Rect PlaceShape(const SpriteDef& s, uint16_t shapeIndex, int16_t x, int16_t y, int w, int h);

// Transparent textures for shape bitmaps, keyed by archive + bitmap + palette.
// Sprites use colour index 0 as transparent; page backgrounds are drawn opaque because
// some (RRQUIT's quit screen) use index 0 as real white.
class SpriteTextureCache {
public:
    explicit SpriteTextureCache(SDL_Renderer* r) : renderer_(r) {}
    ~SpriteTextureCache() { Clear(); }
    SDL_Texture* Get(const Archive& a, uint16_t bitmapId, const Palette& pal, int* w, int* h);
    void Clear();
private:
    struct Entry { SDL_Texture* tex; int w, h; };
    SDL_Renderer* renderer_;
    std::map<std::tuple<const Archive*, uint16_t, const Palette*>, Entry> cache_;
};

// A running animation: plays one SCRP of a SpriteDef, looping or holding the last frame.
class SpriteInstance {
public:
    SpriteInstance(const Archive* a, SpriteDef def, uint16_t scriptId, bool loop);
    void Update(double seconds);
    // (dx, dy) shifts 4:3 screen coordinates into the canvas; clipBottom >= 0 extends the
    // ANI clip rect down to that canvas row.
    void Draw(SDL_Renderer* r, SpriteTextureCache& cache, const Palette& pal, int dx = 0, int dy = 0,
              int clipBottom = -1) const;
    size_t FrameCount() const;
    size_t Frame() const { return frame_; }
    void SetFrame(size_t f) { frame_ = f; held_ = true; }  // freeze on one frame (static use)
    const SpriteDef& Def() const { return def_; }
private:
    const Script* Current() const;
    const Archive* archive_;
    SpriteDef def_;
    uint16_t scriptId_ = 0;
    bool loop_, held_ = false;
    size_t frame_ = 0;
    double clock_ = 0;
};

// Draw shape `shapeIndex` (1-based) at its registration point (top-left on the ANI's bounds
// origin), for layers placed by page scripts rather than by an SCRP (e.g. RRQUIT's dimmer).
void DrawStaticShape(SDL_Renderer* r, SpriteTextureCache& cache, const Archive& a, const SpriteDef& s,
                     uint16_t shapeIndex, const Palette& pal, int dx = 0, int dy = 0);

// Draw one SCRP frame of a sprite (nothing for shape 0), clipped to the ANI clip rect.
void DrawScriptFrame(SDL_Renderer* r, SpriteTextureCache& cache, const Archive& a, const SpriteDef& s,
                     const ScriptFrame& f, const Palette& pal, int dx = 0, int dy = 0, int clipBottom = -1);

// All tracks of an ANI, played together. SPR# record 0 holds the master script and its
// first field N says that the next N records are tracks running in parallel with it:
//   SIGNIN SignInPrompt (ANI 1082): master = Angelica's three voice clips (tWAV 1005-1007),
//     track 1 = the blinking / advancing cursor, tracks 2-9 = the letters of her name;
//   key caps: master (2 frames) + the press wiggle; Reptar's Blink: 14-frame master + eyelid.
// Timing: a frame lasts hold x ticksPerFrame ticks of 1/60 s (ticksPerFrame from op 0E).
// Sound ops 0A / 0B fire the hook when their frame is reached.
class SpriteTracks {
public:
    using SoundHook = std::function<void(uint16_t soundId, bool start)>;
    SpriteTracks(const Archive* a, SpriteDef def);
    void SetSoundHook(SoundHook h) { hook_ = std::move(h); }
    void Start();                                    // from the beginning
    void Stop();                                     // silent stop; draws nothing
    bool Running() const { return running_; }
    double Elapsed() const { return elapsed_; }      // seconds since Start
    void Update(double seconds);
    void Draw(SDL_Renderer* r, SpriteTextureCache& cache, const Palette& pal, int dx = 0, int dy = 0) const;
    const SpriteDef& Def() const { return def_; }
    size_t TrackCount() const { return tracks_.size(); }
private:
    struct Track { uint16_t script = 0; size_t frame = 0; double clock = 0; bool done = false; };
    const Script* ScriptOf(const Track& t) const;
    void Fire(const Track& t, size_t frame);
    const Archive* archive_;
    SpriteDef def_;
    std::vector<Track> tracks_;
    SoundHook hook_;
    bool running_ = false;
    double elapsed_ = 0;
};

// First SPR# script that draws at least one shape (0 if none).
uint16_t FirstDrawingScript(const SpriteDef& s);

} // namespace mohawk
