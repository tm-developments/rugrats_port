// tbmp_test.cpp - tBMP decoder vs. reference indices, plus corrupted-input robustness.
//   tbmp_test <disc root> <reference dir with list.txt + <id>.idx>
#include "mohawk.h"
#include "video_tbmp.h"
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string root = argv[1], ref = argv[2];
    std::ifstream list(ref + "/list.txt");
    std::string line;
    int checked = 0;
    std::vector<std::vector<uint8_t>> samples;
    while (std::getline(list, line)) {
        std::istringstream ss(line);
        std::string file; int id, w, h, fmt;
        if (!(ss >> file >> id >> w >> h >> fmt)) continue;
        mohawk::Archive a;
        std::string err;
        CHECK(a.Open(root + "/" + file, &err));
        const mohawk::Resource* r = a.Find(mohawk::Tag("tBMP"), uint16_t(id));
        CHECK(r != nullptr);
        if (!r) continue;
        samples.push_back(a.Read(*r));
        auto img = mohawk::DecodeBitmap(samples.back(), &err);
        CHECK(img.has_value());
        if (!img) { std::printf("  %s %d: %s\n", file.c_str(), id, err.c_str()); continue; }
        std::ifstream in(ref + "/" + std::to_string(id) + ".idx", std::ios::binary);
        std::vector<uint8_t> want((std::istreambuf_iterator<char>(in)), {});
        const bool same = img->width == w && img->height == h && img->format == fmt && img->pixels == want;
        CHECK(same);
        std::printf("  %-16s %5d %4dx%-3d fmt 0x%04x  %s\n", file.c_str(), id, img->width, img->height, img->format,
                    same ? "identical to reference" : "MISMATCH");
        ++checked;
    }
    CHECK(checked == 9);

    // corrupted inputs must fail cleanly or decode, never read out of bounds
    std::mt19937 rng(1234);
    int rejected = 0, decoded = 0;
    for (int round = 0; round < 3000; ++round) {
        std::vector<uint8_t> d = samples[size_t(round) % samples.size()];
        const int mode = round % 3;
        if (mode == 0) d.resize(rng() % d.size());                                     // truncate
        else for (int k = 0; k < 1 + int(rng() % 16); ++k) d[rng() % d.size()] ^= uint8_t(1u << (rng() % 8));   // flip bits
        if (mode == 2 && d.size() > 8) { d[6] = uint8_t(rng()); d[7] = uint8_t(rng()); }  // random format word
        std::string err;
        (mohawk::DecodeBitmap(d, &err) ? decoded : rejected)++;
    }
    std::printf("  fuzz: 3000 corrupted inputs, %d rejected, %d decoded, no crash\n", rejected, decoded);

    std::printf(fails ? "%d FAILURES\n" : "TBMP TESTS PASSED\n", fails);
    return fails != 0;
}
