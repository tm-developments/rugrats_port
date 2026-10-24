#include "video_sprites.h"
#include "video_tbmp.h"
#include <algorithm>

namespace mohawk {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
int16_t S16(const uint8_t* p) { return int16_t(BE16(p)); }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
template <class T> std::optional<T> Fail(std::string* e, const std::string& m) { if (e) *e = m; return std::nullopt; }
Rect16 ReadRect(const uint8_t* p) { return Rect16{S16(p), S16(p + 2), S16(p + 4), S16(p + 6)}; }
constexpr double kTicksPerSecond = 60.0;
} // namespace

std::optional<Animation> ParseAnimation(uint16_t id, const std::vector<uint8_t>& d, std::string* err) {
    if (d.size() < 26) return Fail<Animation>(err, "ANI shorter than 26 bytes");
    Animation a;
    a.id = id;
    a.bounds = ReadRect(d.data() + 2);
    a.clip = ReadRect(d.data() + 10);
    a.listId = BE16(d.data() + 24);
    return a;
}

std::optional<std::vector<Shape>> ParseShapes(const std::vector<uint8_t>& d, std::string* err) {
    if (d.size() < 2) return Fail<std::vector<Shape>>(err, "empty SHP#");
    const size_t n = BE16(d.data());
    if (d.size() < 2 + n * 6) return Fail<std::vector<Shape>>(err, "truncated SHP#");
    std::vector<Shape> v(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = d.data() + 2 + i * 6;
        v[i] = Shape{BE16(p), S16(p + 2), S16(p + 4)};
    }
    return v;
}

std::optional<std::vector<SequenceRecord>> ParseSequence(const std::vector<uint8_t>& d, std::string* err) {
    if (d.empty() || d.size() % 24) return Fail<std::vector<SequenceRecord>>(err, "SPR# is not whole 24-byte records");
    std::vector<SequenceRecord> v(d.size() / 24);
    for (size_t i = 0; i < v.size(); ++i)
        for (int f = 0; f < 6; ++f) v[i].fields[f] = BE32(d.data() + i * 24 + size_t(f) * 4);
    return v;
}

std::optional<Script> ParseScript(const std::vector<uint8_t>& d, std::string* err) {
    Script s;
    ScriptFrame cur;
    size_t p = 0;
    while (p + 2 <= d.size()) {
        const uint8_t op = d[p], len = d[p + 1];
        if (p + 2 + len > d.size()) return Fail<Script>(err, "SCRP op runs past the end");
        const uint8_t* arg = d.data() + p + 2;
        p += 2 + size_t(len);
        switch (op) {
        case 0x00: return s;
        case 0x05: if (len >= 4) { cur.x = S16(arg); cur.y = S16(arg + 2); cur.positioned = true; } break;
        case 0x06: cur.hold = 1; s.frames.push_back(cur); break;
        case 0x0F:
            if (len >= 4) { cur.hold = std::max<uint32_t>(1, BE32(arg)); s.frames.push_back(cur); }
            break;
        case 0x0A: case 0x0B:
            if (len >= 2) s.sounds.push_back(SoundCue{s.frames.size(), op == 0x0A, BE16(arg)});
            break;
        case 0x07: if (len >= 2) cur.shape = BE16(arg); break;
        case 0x09: case 0x0E: if (len >= 2 && BE16(arg)) s.ticksPerFrame = BE16(arg); break;
        default: s.unknownOps.push_back(op); break;
        }
    }
    return s;                                  // no explicit end: treat end of data as end
}

std::optional<SpriteDef> LoadSprite(const Archive& a, uint16_t aniId, std::string* err) {
    const Resource* ar = a.Find(Tag("ANI "), aniId);
    if (!ar) return Fail<SpriteDef>(err, "no ANI " + std::to_string(aniId));
    SpriteDef s;
    auto ani = ParseAnimation(aniId, a.Read(*ar), err);
    if (!ani) return std::nullopt;
    s.ani = *ani;
    const Resource* sr = a.Find(Tag("SHP#"), s.ani.listId);
    if (!sr) return Fail<SpriteDef>(err, "no SHP# " + std::to_string(s.ani.listId));
    auto shapes = ParseShapes(a.Read(*sr), err);
    if (!shapes) return std::nullopt;
    s.shapes = std::move(*shapes);
    if (const Resource* qr = a.Find(Tag("SPR#"), s.ani.listId)) {
        auto seq = ParseSequence(a.Read(*qr), err);
        if (!seq) return std::nullopt;
        s.sequence = std::move(*seq);
        for (const SequenceRecord& rec : s.sequence) {
            const uint16_t id = rec.ScriptId();
            if (!id || s.scripts.count(id)) continue;
            if (const Resource* cr = a.Find(Tag("SCRP"), id)) {
                auto sc = ParseScript(a.Read(*cr), err);
                if (!sc) return std::nullopt;
                s.scripts[id] = std::move(*sc);
            }
        }
    }
    return s;
}

SDL_Rect PlaceShape(const SpriteDef& s, uint16_t shapeIndex, int16_t x, int16_t y, int w, int h) {
    const Shape& sh = s.shapes.at(size_t(shapeIndex) - 1);
    return SDL_Rect{s.ani.bounds.left + x - sh.regX, s.ani.bounds.top + y - sh.regY, w, h};
}

SDL_Texture* SpriteTextureCache::Get(const Archive& a, uint16_t bitmapId, const Palette& pal, int* w, int* h) {
    const auto key = std::make_tuple(&a, bitmapId, &pal);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        Entry e{nullptr, 0, 0};
        if (const Resource* r = a.Find(Tag("tBMP"), bitmapId)) {
            if (auto img = DecodeBitmap(a.Read(*r))) {
                if (SDL_Surface* s = ToSurface(*img, pal, /*transparentIndex=*/0)) {
                    e = Entry{SDL_CreateTextureFromSurface(renderer_, s), img->width, img->height};
                    if (e.tex) SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
                    SDL_FreeSurface(s);
                }
            }
        }
        it = cache_.emplace(key, e).first;
    }
    if (w) *w = it->second.w;
    if (h) *h = it->second.h;
    return it->second.tex;
}

void SpriteTextureCache::Clear() {
    for (auto& kv : cache_) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
}

SpriteInstance::SpriteInstance(const Archive* a, SpriteDef def, uint16_t scriptId, bool loop)
    : archive_(a), def_(std::move(def)), scriptId_(scriptId), loop_(loop) {}

const Script* SpriteInstance::Current() const {
    auto it = def_.scripts.find(scriptId_);              // looked up each time: copy-safe
    return it == def_.scripts.end() ? nullptr : &it->second;
}

size_t SpriteInstance::FrameCount() const { const Script* s = Current(); return s ? s->frames.size() : 0; }

void SpriteInstance::Update(double seconds) {
    const Script* script = Current();
    if (!script || held_ || script->frames.size() < 2) return;
    clock_ += seconds * kTicksPerSecond;
    for (;;) {
        const double due = double(script->ticksPerFrame) * script->frames[frame_].hold;
        if (clock_ < due) break;
        clock_ -= due;
        if (frame_ + 1 < script->frames.size()) ++frame_;
        else if (loop_) frame_ = 0;
        else { clock_ = 0; break; }
    }
}

void SpriteInstance::Draw(SDL_Renderer* r, SpriteTextureCache& cache, const Palette& pal, int dx, int dy,
                          int clipBottom) const {
    const Script* script = Current();
    if (!script || frame_ >= script->frames.size()) return;
    DrawScriptFrame(r, cache, *archive_, def_, script->frames[frame_], pal, dx, dy, clipBottom);
}

void DrawScriptFrame(SDL_Renderer* r, SpriteTextureCache& cache, const Archive& a, const SpriteDef& s,
                     const ScriptFrame& f, const Palette& pal, int dx, int dy, int clipBottom) {
    if (f.shape == 0 || f.shape > s.shapes.size()) return;            // frame draws nothing
    int w = 0, h = 0;
    SDL_Texture* tex = cache.Get(a, s.shapes[f.shape - 1].bitmapId, pal, &w, &h);
    if (!tex) return;
    // Without an explicit position the frame sits at its registration point,
    // i.e. top-left on the bounds origin.
    const int16_t x = f.positioned ? f.x : s.shapes[f.shape - 1].regX;
    const int16_t y = f.positioned ? f.y : s.shapes[f.shape - 1].regY;
    SDL_Rect dst = PlaceShape(s, f.shape, x, y, w, h);
    dst.x += dx; dst.y += dy;
    const Rect16& c = s.ani.clip;
    SDL_Rect clip{c.left + dx, c.top + dy, c.right - c.left, c.bottom - c.top};
    if (clipBottom >= 0) clip.h = std::max(clip.h, clipBottom - clip.y);
    const bool clipped = clip.w > 0 && clip.h > 0;
    if (clipped) SDL_RenderSetClipRect(r, &clip);
    SDL_RenderCopy(r, tex, nullptr, &dst);
    if (clipped) SDL_RenderSetClipRect(r, nullptr);
}

SpriteTracks::SpriteTracks(const Archive* a, SpriteDef def) : archive_(a), def_(std::move(def)) {
    if (def_.sequence.empty()) return;
    const size_t extra = std::min<size_t>(def_.sequence[0].fields[0], def_.sequence.size() - 1);
    for (size_t i = 0; i <= extra; ++i) tracks_.push_back(Track{def_.sequence[i].ScriptId()});
}

const Script* SpriteTracks::ScriptOf(const Track& t) const {
    auto it = def_.scripts.find(t.script);
    return it == def_.scripts.end() ? nullptr : &it->second;
}

void SpriteTracks::Fire(const Track& t, size_t frame) {
    const Script* s = ScriptOf(t);
    if (!s || !hook_) return;
    for (const SoundCue& c : s->sounds)
        if (c.frame == frame) hook_(c.soundId, c.start);
}

void SpriteTracks::Start() {
    running_ = !tracks_.empty();
    elapsed_ = 0;
    for (Track& t : tracks_) {
        t.frame = 0; t.clock = 0;
        const Script* s = ScriptOf(t);
        t.done = !s || s->frames.empty();
        Fire(t, 0);
        if (t.done && s) Fire(t, s->frames.size());
    }
}

void SpriteTracks::Stop() { running_ = false; }

void SpriteTracks::Update(double seconds) {
    if (!running_) return;
    elapsed_ += seconds;
    bool any = false;
    for (Track& t : tracks_) {
        const Script* s = ScriptOf(t);
        if (!t.done && s) {
            t.clock += seconds * kTicksPerSecond;
            for (;;) {
                const double due = double(s->ticksPerFrame) * s->frames[t.frame].hold;
                if (t.clock < due) break;
                t.clock -= due;
                if (t.frame + 1 < s->frames.size()) { ++t.frame; Fire(t, t.frame); }
                else { t.done = true; Fire(t, s->frames.size()); break; }
            }
        }
        any = any || !t.done;
    }
    running_ = any;
}

void SpriteTracks::Draw(SDL_Renderer* r, SpriteTextureCache& cache, const Palette& pal, int dx, int dy) const {
    if (!running_) return;
    for (const Track& t : tracks_) {
        const Script* s = ScriptOf(t);
        if (s && t.frame < s->frames.size())
            DrawScriptFrame(r, cache, *archive_, def_, s->frames[t.frame], pal, dx, dy);
    }
}

void DrawStaticShape(SDL_Renderer* r, SpriteTextureCache& cache, const Archive& a, const SpriteDef& s,
                     uint16_t shapeIndex, const Palette& pal, int dx, int dy) {
    if (shapeIndex == 0 || shapeIndex > s.shapes.size()) return;
    const Shape& sh = s.shapes[shapeIndex - 1];
    int w = 0, h = 0;
    SDL_Texture* tex = cache.Get(a, sh.bitmapId, pal, &w, &h);
    if (!tex) return;
    SDL_Rect dst = PlaceShape(s, shapeIndex, sh.regX, sh.regY, w, h);
    dst.x += dx; dst.y += dy;
    SDL_RenderCopy(r, tex, nullptr, &dst);
}

uint16_t FirstDrawingScript(const SpriteDef& s) {
    for (const SequenceRecord& rec : s.sequence) {
        auto it = s.scripts.find(rec.ScriptId());
        if (it == s.scripts.end()) continue;
        for (const ScriptFrame& f : it->second.frames)
            if (f.shape) return rec.ScriptId();
    }
    return 0;
}

} // namespace mohawk
