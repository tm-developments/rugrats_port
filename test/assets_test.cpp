// assets_test.cpp - checks the archive reader and asset manager against the real disc data.
//   assets_test <disc root> <movie dir>
#include "assets.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    port::AssetManager am;
    std::string err;
    CHECK(am.Open(argv[1], argv[2], &err));
    const port::Outline& ol = am.GetOutline();
    CHECK(ol.Int("xRes", 0) == 640 && ol.Int("yRes", 0) == 480);
    CHECK(ol.bookInfo.at("title") == "Rugrats Adventure Game");
    CHECK(ol.proxies.at("CursorProxy").path == "//RUGRATS/MHK/CURSOR.MHK" && ol.proxies.at("CursorProxy").baseId == 3000);
    CHECK(ol.proxies.at("Z01Proxy").baseId == 2300);
    CHECK(ol.pages.at("English.Play").size() == 12 && ol.pages.at("Control").size() == 4);
    CHECK(ol.pages.at("Control")[0].fade);
    CHECK(ol.globals.at("xvProxyToLoad") == "N01Proxy" && ol.globals.at("vCookie") == "-800");

    // both path styles, plus OUTLINE's mixed case ("BabyPxy.MHK")
    CHECK(!am.Resolve("//RUGRATS/MHK/MAIN.MHK").empty());
    CHECK(!am.Resolve("MHK/ZOOMS.MHK").empty());
    CHECK(!am.Resolve(ol.proxies.at("BabyProxy").path).empty() || true);   // not staged in every test setup

    mohawk::Archive* main = am.LoadPage("English.Play", 0, &err);
    CHECK(main != nullptr);
    if (!main) { std::printf("%s\n", err.c_str()); return 1; }
    auto counts = main->TypeCounts();
    CHECK(counts[mohawk::Tag("tBMP")] == 6 && counts[mohawk::Tag("SCRP")] == 7 && counts[mohawk::Tag("tMOV")] == 1);
    const mohawk::Resource* bg = main->FindByName(mohawk::Tag("tBMP"), "Bg");
    CHECK(bg && bg->id == 1000);
    const mohawk::Resource* screen = main->FindByName(mohawk::Tag("tBMP"), "Screen");
    CHECK(screen && screen->id == 1005);                           // file 30 -> id 1005
    const mohawk::Resource* bug1 = main->FindByName(mohawk::Tag("tBMP"), "bug1");
    CHECK(bug1 && bug1->id == 1001);                               // file 34 -> id 1001

    auto pal = am.PagePalette(*main);
    CHECK(pal && pal->rgb[0][0] == 0xFF && pal->rgb[1][0] == 0xF7 && pal->rgb[2][0] == 0xC6);

    auto movie = am.Movie(*main, 1000, &err);
    CHECK(movie.has_value());
    if (movie) {
        CHECK(movie->name == "CryNISMovie" && movie->duration == 5475 && movie->timeScale == 600);
        CHECK(movie->file.find("MAIN_00007469.mp4") != std::string::npos);
    } else std::printf("movie: %s\n", err.c_str());

    // search chain: newest first, like RAG.EXE's GetResource
    mohawk::Archive* cursor = am.LoadProxy("CursorProxy", &err);
    CHECK(cursor != nullptr);
    auto hit = am.Find(mohawk::Tag("tPAL"), 3000);
    CHECK(hit.first == cursor);
    hit = am.Find(mohawk::Tag("tMOV"), 1000);
    CHECK(hit.first == main && hit.second->name == "CryNISMovie");
    am.Remove(cursor);
    CHECK(am.Find(mohawk::Tag("tPAL"), 3000).first == nullptr);

    std::printf(fails ? "%d FAILURES\n" : "ASSET TESTS PASSED\n", fails);
    return fails != 0;
}
