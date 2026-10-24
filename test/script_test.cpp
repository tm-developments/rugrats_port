// script_test.cpp - BITL object tables, BCOD headers, world placement and hit-testing.
//   script_test <disc root>
#include "mohawk.h"
#include "script_interpreter.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string root = argv[1];
    const char* files[] = {"MHK/RRQUIT.MHK", "MHK/N01.MHK", "MHK/TOOLBAR.MHK", "MHK/MAIN.MHK", "MHK/CHUKMAIN.MHK",
                           "MHK/TN01.MHK", "MHK/CURSOR.MHK", "MHK/WARP.MHK", "MHK/NODES.MHK", "MHK/PAL.MHK",
                           "PROGRAM/MHK/ZOOMS.MHK", "PROGRAM/MHK/DETECTOR.MHK", "PROGRAM/MHK/Z01.MHK"};
    size_t objects = 0, archives = 0, bcods = 0;
    for (const char* f : files) {
        mohawk::Archive a;
        std::string err;
        if (!a.Open(root + "/" + f, &err)) continue;
        ++archives;
        mohawk::ScriptRuntime rt;
        CHECK(rt.Load(a, &err));
        objects += rt.Objects().objects.size();
        if (rt.Bytecode()) ++bcods;
        else std::printf("  %s: BCOD header rejected (%s)\n", f, err.c_str());
    }
    std::printf("  %zu archives: %zu BITL objects, %zu BCOD headers consistent\n", archives, objects, bcods);
    CHECK(archives == 13 && objects == 284 && bcods == 13);

    mohawk::Archive n01;
    CHECK(n01.Open(root + "/MHK/N01.MHK"));
    mohawk::ScriptRuntime rt;
    CHECK(rt.Load(n01));
    const auto& t = rt.Objects();
    CHECK(t.objects.size() == 25);
    CHECK(t.viewOrigin.at("View1") == 36 && t.viewOrigin.at("View2") == 723 &&
          t.viewOrigin.at("View3") == 1371 && t.viewOrigin.at("View4") == 1892);
    CHECK(t.viewOf.at(t.Find("BallGag")->objectId) == "View4" && t.viewOf.at(t.Find("XyloGag")->objectId) == "View3");
    CHECK(t.Find("BallGag")->resourceId == 2102 && t.Find("Screw")->resourceId == 2103);
    auto ball = t.WorldRect(*t.Find("BallGag"));      // image-matched world position: x 2358
    CHECK(ball && ball->x == 2358 && ball->y == 314 && ball->w == 57 && ball->h == 43);
    auto screw = t.WorldRect(*t.Find("Screw"));       // image-matched: x 2121
    CHECK(screw && screw->x == 2121);
    CHECK(!t.WorldRect(*t.Find("StuPatch")));          // view not known from groups: not placed

    CHECK(rt.ActiveView(1371, 2560) == "View3" && rt.ActiveView(1800, 2560) == "View4" && rt.ActiveView(2550, 2560) == "View1");
    const mohawk::ScriptObject* hit = rt.HitTest(1850, 300, "View3", 2560);   // xylophone
    CHECK(hit && hit->name == "XyloGag");
    CHECK(rt.HitTest(1850, 300, "View4", 2560) == nullptr);                   // other view: inactive
    hit = rt.HitTest(36 + 300, 100, "View1", 2560);                           // window
    CHECK(hit && hit->name == "WindowGag");
    int events = 0;
    rt.SetHandler([&](const mohawk::ScriptObject& o, mohawk::ScriptEvent e) { events += (o.name == "XyloGag" && e == mohawk::ScriptEvent::Click); });
    rt.Click(1850, 300, "View3", 2560);
    CHECK(events == 1);

    std::printf(fails ? "%d FAILURES\n" : "SCRIPT TESTS PASSED\n", fails);
    return fails != 0;
}
