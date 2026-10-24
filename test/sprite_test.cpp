// sprite_test.cpp - ANI/SHP#/SPR#/SCRP parsing and placement against the real archives.
//   sprite_test <disc root>
#include "mohawk.h"
#include "video_sprites.h"
#include "video_tbmp.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static SDL_Rect FramePlace(const mohawk::Archive& a, const mohawk::SpriteDef& d, uint16_t script, size_t frame) {
    const mohawk::ScriptFrame& f = d.scripts.at(script).frames.at(frame);
    auto img = mohawk::DecodeBitmap(a.Read(*a.Find(mohawk::Tag("tBMP"), d.shapes.at(f.shape - 1).bitmapId)));
    return mohawk::PlaceShape(d, f.shape, f.x, f.y, img->width, img->height);
}
static bool Eq(SDL_Rect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string root = argv[1];
    // 1. every sprite resource in every staged archive parses, with no unknown ops
    const char* files[] = {"MHK/MAIN.MHK", "MHK/CURSOR.MHK", "MHK/TOOLBAR.MHK", "MHK/WARP.MHK", "MHK/RRQUIT.MHK",
                           "MHK/CHUKMAIN.MHK", "MHK/NODES.MHK", "MHK/PAL.MHK", "PROGRAM/MHK/DETECTOR.MHK", "PROGRAM/MHK/ZOOMS.MHK"};
    size_t anis = 0, scripts = 0, frames = 0, unknown = 0;
    for (const char* f : files) {
        mohawk::Archive a;
        std::string err;
        if (!a.Open(root + "/" + f, &err)) { std::printf("  skip %s (%s)\n", f, err.c_str()); continue; }
        for (const mohawk::Resource* r : a.OfType(mohawk::Tag("ANI "))) {
            auto d = mohawk::LoadSprite(a, r->id, &err);
            CHECK(d.has_value());
            if (!d) { std::printf("  %s ANI %u: %s\n", f, r->id, err.c_str()); continue; }
            ++anis;
            for (const auto& [id, sc] : d->scripts) {
                ++scripts; frames += sc.frames.size(); unknown += sc.unknownOps.size();
                for (const auto& fr : sc.frames) CHECK(fr.shape <= d->shapes.size());
            }
            for (const auto& sh : d->shapes) CHECK(a.Find(mohawk::Tag("tBMP"), sh.bitmapId) != nullptr);
        }
    }
    std::printf("  parsed %zu ANI, %zu SCRP, %zu frames across %zu archives; unknown ops: %zu\n",
                anis, scripts, frames, sizeof files / sizeof *files, unknown);
    {   // 0F holds and 0A/0B sound cues (CHUKMAIN 20096: FRAME, start 20000, hold 16, stop 20000)
        mohawk::Archive ch; std::string err;
        if (ch.Open(root + "/MHK/CHUKMAIN.MHK", &err)) {
            auto sc = mohawk::ParseScript(ch.Read(*ch.Find(mohawk::Tag("SCRP"), 20096)));
            CHECK(sc && sc->frames.size() == 2 && sc->frames[1].hold == 16);
            CHECK(sc && sc->sounds.size() == 2 && sc->sounds[0].start && sc->sounds[0].soundId == 20000 &&
                  sc->sounds[0].frame == 1 && !sc->sounds[1].start);
        }
    }
    CHECK(unknown == 0 && anis > 50);

    // 2. placements measured on the real art
    mohawk::Archive quit, tool;
    CHECK(quit.Open(root + "/MHK/RRQUIT.MHK") && tool.Open(root + "/MHK/TOOLBAR.MHK"));
    auto yes = mohawk::LoadSprite(quit, 8502), no = mohawk::LoadSprite(quit, 8504);
    CHECK(yes && no);
    CHECK(mohawk::FirstDrawingScript(*yes) == 8503 && yes->scripts.at(8503).frames.size() == 2);
    CHECK(Eq(FramePlace(quit, *yes, 8503, 1), 230, 165, 73, 66));     // exactly the ANI bounds
    CHECK(Eq(FramePlace(quit, *yes, 8503, 0), 232, 167, 69, 64));
    CHECK(Eq(FramePlace(quit, *no, 8507, 1), 320, 168, 71, 65));
    auto bar = mohawk::LoadSprite(tool, 3504), cry = mohawk::LoadSprite(tool, 3510);
    CHECK(bar && cry);
    CHECK(Eq(FramePlace(tool, *bar, 3505, 0), 0, 400, 640, 84));      // toolbar at the bottom
    CHECK(Eq(FramePlace(tool, *cry, 3517, 0), 353, 403, 73, 70));     // Cry button
    CHECK(bar->ani.clip.right == 640 && bar->ani.clip.bottom == 480);  // clip trims rows 480-483

    std::printf(fails ? "%d FAILURES\n" : "SPRITE TESTS PASSED\n", fails);
    return fails != 0;
}
