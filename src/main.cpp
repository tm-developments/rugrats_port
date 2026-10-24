// main.cpp - Rugrats Adventure Game: unofficial native 64-bit port of the 1998 Mohawk LB
// (Living Books v3) game. Runs the game's own page scripts (lb/) over its .MHK archives.
//
//   rugrats_port.exe [--fullscreen | --borderless | --windowed] [--data DIR] [--userdir DIR]
//   rugrats_port.exe --prepare [OUTDIR]     installer step: build movies\ and sounds\, exit
//
// Data: the folder of the exe when it holds PROGRAM\ (else --data). Movies and sounds are
// pre-extracted from the archives by prepare.cpp, into the data folder (installer) or, on
// a first run, into %LOCALAPPDATA%\Rugrats Adventure Game\Cache.
// Saves, settings.ini and the log: %APPDATA%\Rugrats Adventure Game (or UserData\ beside
// the game when a file named "portable" is there, or --userdir).
// Display: exclusive full screen at the desktop resolution by default, the 640x480 game
// scaled to fit (4:3, pillarboxed). F11 / Options > Display > Zoom toggle a
// window, which has the original menu bar (Alt shows / hides it).
// Development builds (-DRUGRATS_DEV, see devenv.h) add test switches such as
// --headless-frames N, --at FRAME:ACTION and trace environment variables.
// === BYPASS SDL MAIN HIJACKING ===
#define SDL_MAIN_HANDLED
#include "devenv.h"
#include <SDL.h>

#include <algorithm>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <optional>
#include <vector>
#include <string>

#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <io.h>
#endif
#include "assets.h"
#include "busy_cursor.h"
#include "lb/lb_code.h"
#include "lb/lb_engine.h"
#include "audio.h"
#include "mohawk.h"
#include "video_player.h"
#include "video_sprites.h"
#include "script_interpreter.h"
#include "video_tbmp.h"
#include "prepare.h"

namespace {

constexpr int kCanvasW = 640, kCanvasH = 480;       // the original 4:3 screen (logical render size)
constexpr int kWideW = 860, kWideH = 484;           // --widescreen: 16:9, the room shown beside the frame

struct Options {
    std::string data = ".", movies = "movies", section = "English.Play", screenshot;
    size_t page = 0;
    int node = 0;                    // 0 = take OUTLINE [Globals] xnNode
    double scroll = -1;              // camera left edge; < 0 = derive from xnView
    int scale = 2;
    // Display: exclusive full screen at the desktop's resolution by default; --borderless
    // fills the screen with a borderless window instead; --windowed starts in a window.
    enum class Display { Default, Fullscreen, Borderless, Window } display = Display::Default;
    std::string sounds;              // the converted sounds (default <data>\\sounds, see prepare.cpp)
    std::string prepareOut;          // --prepare [folder]: build movies\\ and sounds\\ there, then exit
    bool prepare = false;
    std::string userDir;             // --userdir: where saves and settings go (default: the user's AppData)
    std::string renderer;            // --renderer d3d11|d3d9|opengl|software (default: settings.ini, else d3d11)
    bool portable = false;           // --portable: save in the game folder itself (asks for admin rights if needed)
    bool elevatedRelaunch = false;   // --elevated: this run is the admin relaunch made for --portable
    bool toolbarButtons = false, quitOverlay = false, objectOverlay = false;
    int clickX = -1, clickY = -1;    // headless: inject one click before the screenshot
    long clickFrame = -1;
    int hoverX = -1, hoverY = -1;    // headless: pointer position
    long escFrame = -1;              // headless: press Esc at this frame
    std::vector<std::string> at;     // headless: "frame:action[:args]" (click/move/down/up X,Y; esc; shot PATH)
    int explore = 0;                 // headless: click every scripted object in every view, N actions            // headless: frame of that click (default: 2 before the end)
    long headlessFrames = 0;
    bool skipIntro = false;          // start straight in the game (old behavior)
    bool widescreen = false;         // 16:9 canvas showing more of the room (default: 4:3 like the original)
    bool native = false;             // the hand-built front end instead of the script engine
    bool startAtSignIn = false;      // skip only the intro movies
    std::string typeName;            // headless test: name typed on the sign-in screen
};

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); return nullptr; }
            return argv[++i];
        };
        const char* v = nullptr;
        if (a == "--data" && (v = next("--data"))) o.data = v;
        else if (a == "--movies" && (v = next("--movies"))) o.movies = v;
        else if (a == "--fullscreen") o.display = Options::Display::Fullscreen;
        else if (a == "--borderless") o.display = Options::Display::Borderless;
        else if (a == "--windowed" || a == "--window") o.display = Options::Display::Window;
        else if (a == "--userdir" && (v = next("--userdir"))) o.userDir = v;
        else if (a == "--portable") o.portable = true;
        else if (a == "--renderer" && (v = next("--renderer"))) o.renderer = v;
        else if (a == "--d3d11" || a == "--dx11") o.renderer = "d3d11";
        else if (a == "--d3d9" || a == "--dx9") o.renderer = "d3d9";
        else if (a == "--opengl" || a == "--ogl") o.renderer = "opengl";
        else if (a == "--software") o.renderer = "software";
        else if (a == "--elevated") o.elevatedRelaunch = true;
        else if (a == "--sounds" && (v = next("--sounds"))) o.sounds = v;
        else if (a == "--prepare") { o.prepare = true; if (i + 1 < argc && argv[i + 1][0] != '-') o.prepareOut = argv[++i]; }
        else if (!kDevBuild) continue;                // release: test options and unknown ones are ignored
        else if (a == "--section" && (v = next("--section"))) o.section = v;
        else if (a == "--page" && (v = next("--page"))) o.page = size_t(std::stoul(v));
        else if (a == "--node" && (v = next("--node"))) o.node = std::stoi(v);
        else if (a == "--scroll" && (v = next("--scroll"))) o.scroll = std::stod(v);
        else if (a == "--scale" && (v = next("--scale"))) o.scale = std::max(1, std::stoi(v));
        else if (a == "--headless-frames" && (v = next("--headless-frames"))) o.headlessFrames = std::stol(v);
        else if (a == "--screenshot" && (v = next("--screenshot"))) o.screenshot = v;
        else if (a == "--toolbar-buttons") o.toolbarButtons = true;
        else if (a == "--quit-overlay") o.quitOverlay = true;
        else if (a == "--object-overlay") o.objectOverlay = true;
        else if (a == "--click" && (v = next("--click"))) std::sscanf(v, "%d,%d", &o.clickX, &o.clickY);
        else if (a == "--click-frame" && (v = next("--click-frame"))) o.clickFrame = std::stol(v);
        else if (a == "--hover" && (v = next("--hover"))) std::sscanf(v, "%d,%d", &o.hoverX, &o.hoverY);
        else if (a == "--esc-frame" && (v = next("--esc-frame"))) o.escFrame = std::stol(v);
        else if (a == "--at" && (v = next("--at"))) o.at.push_back(v);
        else if (a == "--explore" && (v = next("--explore"))) o.explore = std::stoi(v);
        else if (a == "--skip-intro") o.skipIntro = true;
        else if (a == "--widescreen") o.widescreen = true;
        else if (a == "--signin") o.startAtSignIn = true;
        else if (a == "--type" && (v = next("--type"))) o.typeName = v;
        else if (a == "--native") o.native = true;
        else { std::fprintf(stderr, "unknown or incomplete option %s\n", a.c_str()); return false; }
    }
    return true;
}

struct TextureDeleter { void operator()(SDL_Texture* t) const { if (t) SDL_DestroyTexture(t); } };
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

// What is on screen: the page archive, the scene background (from the page itself or,
// on the start page, from the current navigation node), and the page's movie.
struct PageState {
    mohawk::Archive* archive = nullptr;
    mohawk::Archive* node = nullptr;          // N01..N20 proxy shown on the start page
    mohawk::Palette palette{};                // palette of whatever the background came from
    bool hasPalette = false;
    TexturePtr background;
    int bgWidth = 0, bgHeight = 0;
    port::VideoPlayer movie;
    bool hasMovie = false;
    SDL_Rect movieRect{0, 0, 0, 0};           // BITL movie object (type 0x43), 4:3 coords; w = 0: centred
    mohawk::ScriptRuntime scripts;            // objects of whatever the scene came from
    bool hasScripts = false;
    const mohawk::ScriptObject* selected = nullptr;
};

// ---- front end ---------------------------------------------------------------------
// Start-up as OUTLINE orders it: [Intro] Page1-3 (RRINTRO1-3: backdrop + movie), then
// [Control] Page1.1 (SIGNIN: name entry), then [English.Play]. These screens are the
// original 640x480 art, shown 4:3 at x = 110 with black pillarbox bars.
enum class Mode { Intro, SignIn, Play, Engine };

struct FrontScreen {
    mohawk::Archive* archive = nullptr;
    mohawk::Palette palette{};
    bool hasPalette = false;
    TexturePtr background;
    int bgW = 0, bgH = 0;
    port::VideoPlayer movie;
    bool hasMovie = false;
    SDL_Rect movieRect{0, 0, 640, 480};          // BITL movie object (type 0x43), 4:3 coords
    bool movieShown = false;                     // a frame has been uploaded (kept for the fade-out)
    double clock = 0;                            // seconds on this screen
    mohawk::ScriptRuntime objects;               // BITL of the page
};

// SIGNIN.MHK, from its BITL: key objects ka..kz/kdel (click rects; id1 = the key-cap ANI),
// GoBtn, LineEditor (where the name is typed), ListDisplay (saved players), Alphabet
// (ANI whose SHP# holds the name font: A-Z, a-z, blank), FlashCursor (blank / '_'),
// Blink (Reptar's eye). The page's own logic is bytecode that isn't decoded yet, so the
// behavior here is a native re-implementation using the game's art and positions.
//
// Animations are the ANIs' own SPR# tracks (mohawk::SpriteTracks): each key cap and Go
// play their press animation when used; Reptar blinks every few seconds; SignInPrompt is
// Angelica's spoken instructions (tWAV 1005-1007) while her name is typed out as an
// example. Page sounds are the type 0x41 objects (TypeSnd, DeleteSnd, GoBtnSnd, ...).
struct SignInUI {
    std::vector<std::pair<mohawk::SpriteDef, std::string>> keys;    // key cap + key name
    std::map<std::string, SDL_Rect> rects;                         // object name -> rect
    std::optional<mohawk::SpriteDef> go;
    std::map<std::string, std::unique_ptr<mohawk::SpriteTracks>> press;   // key name / "Go" -> press anim
    std::unique_ptr<mohawk::SpriteTracks> blink, prompt;
    double blinkWait = 1.5;                      // seconds until Reptar's next blink
    std::map<std::string, uint16_t> sounds;      // "TypeSnd" -> tWAV id
    std::map<uint16_t, int> promptVoices;        // tWAV id -> playing voice
    bool promptPending = false;                  // Angelica waits for the fade-in to finish
    std::vector<uint16_t> font;                  // Alphabet SHP# bitmaps: [0..25] A-Z, [26..51] a-z, [52] blank
    uint16_t cursorBitmap = 0;                   // FlashCursor shape 2 ('_')
    std::vector<std::string> players;
    std::string name;
};

class Game {
public:
    explicit Game(Options o) : opt_(std::move(o)) {}
    ~Game() {
        page_.reset();                                           // textures before the renderer
        front_.reset(); signIn_.reset();
        toolbarButtons_.clear(); quitButtons_.clear(); toolbarBar_.reset();
        spriteCache_.reset();
        lb_.reset();
        for (auto& kv : lbMovieTex_) SDL_DestroyTexture(kv.second.tex);
        if (lbTex_) SDL_DestroyTexture(lbTex_);
        audio_.reset();
        if (movieTex_) SDL_DestroyTexture(movieTex_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    bool Init() {
        std::string err;
        // === SAFETY GUARD: Stop execution instantly if asset mapping files are missing ===
        if (!assets_.Open(opt_.data, opt_.movies, &err)) { 
            std::fprintf(stderr, "[ENGINE FATAL ERROR] assets: %s\n", err.c_str()); 
            std::fprintf(stderr, "Please double-check that your 'PROGRAM' directory contains the OUTLINE file!\n");
            return false; 
        }
        
        const port::Outline& ol = assets_.GetOutline();
        gameW_ = ol.Int("xRes", 640);                            // the original 4:3 frame
        gameH_ = ol.Int("yRes", 480);
        if (opt_.widescreen) { width_ = kWideW; height_ = kWideH; }
        hudX_ = (width_ - gameW_) / 2;                           // 0 (4:3), 110 in the widescreen canvas
        auto title = ol.bookInfo.count("title") ? ol.bookInfo.at("title") : std::string("Mohawk");

        if (opt_.headlessFrames > 0) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
        if (opt_.headlessFrames > 0) SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        // Full screen must not be "always on top": SDL normally marks a focused full-screen
        // window HWND_TOPMOST, so after Alt+Tab the game stayed above the window switched to.
        // Exclusive full screen also minimizes when it loses focus (see PumpEvents).
        SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");         // the exe's icon on the window
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "auto");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false; }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");   // keep the 8-bit art crisp
        ChooseDisplayMode();
        // the window opens hidden and goes full screen (or not) before it is first shown
        Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
        int scale = opt_.scale;
        {   // a window as large as the desktop allows, in whole multiples of the 640x480 screen
            SDL_Rect usable;
            if (opt_.headlessFrames == 0 && SDL_GetDisplayUsableBounds(0, &usable) == 0)
                while (scale > 1 && (width_ * scale > usable.w - 40 || height_ * scale > usable.h - 80)) --scale;
        }
        window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   width_ * scale, height_ * scale, wflags);
        if (!window_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
        // Direct3D 11 (Windows' flip-model presentation: full-screen smoothness without the old
        // Direct3D 9 exclusive mode, so Alt+Tab and dialogs behave); other drivers if it's missing
        // --renderer / --d3d9 / --opengl / --software, or settings.ini [Display] renderer=, pick another
        if (opt_.headlessFrames == 0) SDL_SetHint(SDL_HINT_RENDER_DRIVER, RenderDriverName().c_str());
        const bool softwareChosen = opt_.headlessFrames > 0 || RenderDriverName() == "software";
        renderer_ = SDL_CreateRenderer(window_, -1, softwareChosen ? SDL_RENDERER_SOFTWARE
                                                   : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_ && opt_.headlessFrames == 0) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "");
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (SDL_RendererInfo ri; renderer_ && SDL_GetRendererInfo(renderer_, &ri) == 0) std::printf("renderer: %s\n", ri.name);
        if (!renderer_) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
        SDL_RenderSetLogicalSize(renderer_, width_, height_);    // aspect kept (letterboxed) at any window / fullscreen size
        audio_ = std::make_unique<port::Audio>();
        audio_->SetTranslatedFolder(opt_.sounds + "/");   // the archives' own sounds (see SoundPath)
        LoadPlayerSettings();
        LoadVolume();
        BuildMenuBar();
        if (opt_.headlessFrames == 0) {
            if (startFullscreen_) SetFullscreen(true);
            SDL_ShowWindow(window_);
            SDL_RaiseWindow(window_);
            ApplyWindowIcons();
        }
        if (audio_->Init()) std::printf("audio: ready (sounds from %s)\n", opt_.sounds.c_str());

        std::printf("%s: %zu proxies, %zu page sections\n", title.c_str(), ol.proxies.size(), ol.pages.size());
        worldWrap_ = ol.bookInfo.count("fWorldWrap") && ol.bookInfo.at("fWorldWrap") == "true";
        nodeIndex_ = opt_.node > 0 ? opt_.node : GlobalInt("xnNode", 1);
        spriteCache_ = std::make_unique<mohawk::SpriteTextureCache>(renderer_);
        showToolbarButtons_ = opt_.toolbarButtons;
        showQuit_ = opt_.quitOverlay;
        showObjects_ = opt_.objectOverlay;
        LoadToolbar();
        LoadQuitOverlay();
        LoadCursor();
        if (!opt_.native) return StartEngine();
        // Start-up sequence: intro movies, then name entry, then the game (EnterPlay).
        const auto& pages = assets_.GetOutline().pages;
        if (!opt_.skipIntro && !opt_.startAtSignIn && pages.count("Intro") && !pages.at("Intro").empty())
            return LoadIntro(0) || EnterSignIn();
        if (!opt_.skipIntro && pages.count("Control") && !pages.at("Control").empty()) return EnterSignIn();
        return EnterPlay();
    }

    // Losing focus (another window, Alt+Tab, minimizing) freezes the whole game until the
    // window is active again.
    bool frozen_ = false;
    void SetFrozen(bool f) {
        if (f == frozen_ || opt_.headlessFrames > 0) return;
        frozen_ = f;
        if (audio_) audio_->PauseOutput(f, 2);
        altAlone_ = false;
    }

    int Run() {
        constexpr double kStep = 1.0 / 60.0;                    // engine logic tick
        const double freq = double(SDL_GetPerformanceFrequency());
        Uint64 last = SDL_GetPerformanceCounter();
        double acc = 0;
        for (long frame = 0; running_; ++frame) {
            PumpEvents();
            if (frozen_ && opt_.headlessFrames <= 0) {
                // the window is in the background: everything waits (game clock, movies, sound)
                if (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS && !(SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) { SetFrozen(false); continue; }
                // Exclusive full screen is a Direct3D exclusive device: drawing to it while it is
                // in the background makes the renderer reset the device, which takes the screen
                // back and pops the game over the window Alt+Tab switched to. So a backgrounded
                // or minimized exclusive window draws nothing until it is active again.
                const Uint32 wf = SDL_GetWindowFlags(window_);
                const bool exclusiveFs = (wf & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN;
                if (!exclusiveFs && !(wf & SDL_WINDOW_MINIMIZED)) Render();
                SDL_WaitEventTimeout(nullptr, 250);
                last = SDL_GetPerformanceCounter(); acc = 0;            // no catching up afterwards
                continue;
            }
            if (opt_.headlessFrames > 0) {                      // deterministic: one step per frame
                if (opt_.hoverX >= 0) { mouseX_ = opt_.hoverX; mouseY_ = opt_.hoverY; mouseIn_ = true; }
                Update(kStep);
                if (frame == 1 && !opt_.typeName.empty() && mode_ == Mode::SignIn)
                    for (char c : opt_.typeName) SignInChar(c);
                if (mode_ == Mode::Engine && !opt_.typeName.empty() && lbSection_ == "Control" && ++lbTypeWait_ == 30)
                    for (char c : opt_.typeName) EngineChar(c);
                const long clickAt = opt_.clickFrame >= 0 ? opt_.clickFrame : opt_.headlessFrames - 2;
                if (frame == clickAt && opt_.clickX >= 0 && !Transitioning()) {
                    if (mode_ == Mode::Engine) { EngineMouseMove(opt_.clickX, opt_.clickY); EngineClick(opt_.clickX, opt_.clickY, true); EngineClick(opt_.clickX, opt_.clickY, false); }
                    else Click(opt_.clickX, opt_.clickY);
                }
                if (mode_ == Mode::Engine && opt_.hoverX >= 0) EngineMouseMove(opt_.hoverX, opt_.hoverY);
                for (const std::string& s : opt_.at) {
                    const size_t c1 = s.find(':');
                    if (c1 == std::string::npos || std::stol(s.substr(0, c1)) != frame) continue;
                    const size_t c2 = s.find(':', c1 + 1);
                    const std::string act = s.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                    const std::string arg = c2 == std::string::npos ? std::string() : s.substr(c2 + 1);
                    int ax = 0, ay = 0;
                    std::sscanf(arg.c_str(), "%d,%d", &ax, &ay);
                    if (act == "shot") { Render(); Screenshot(arg); }
                    else if (act == "esc") { SDL_Event ev{}; ev.type = SDL_KEYDOWN; ev.key.keysym.sym = SDLK_ESCAPE; EngineEvent(ev); }
                    else if (act == "sdlkey") {                 // sdlkey:KEYCODE,MOD - through the window's key path
                        int kc = 0, km = 0; std::sscanf(arg.c_str(), "%d,%d", &kc, &km);
                        SDL_Event ev{}; ev.type = SDL_KEYDOWN; ev.key.keysym.sym = kc; ev.key.keysym.mod = Uint16(km);
                        std::printf("SDLKEY f%ld key %d mod %d\n", frame, kc, km);
                        if (!MenuEvent(ev) && mode_ == Mode::Engine && lb_) EngineEvent(ev);
                    }
                    else if (act == "move") { mouseX_ = ax; mouseY_ = ay; mouseIn_ = true; EngineMouseMove(ax, ay); }
                    else if (act == "down") { EngineMouseMove(ax, ay); EngineClick(ax, ay, true); }
                    else if (act == "up") { EngineMouseMove(ax, ay); EngineClick(ax, ay, false); }
                    else if (act == "click") { mouseX_ = ax; mouseY_ = ay; mouseIn_ = true; EngineMouseMove(ax, ay); EngineClick(ax, ay, true); EngineClick(ax, ay, false); }
                    else if (act == "dump" && lb_) lb_->DumpState();
                    else if (act == "sdldown" || act == "sdlup") {   // sdldown:x,y,button - through the window's event path
                        int bx = 0, by = 0, bb = 1; std::sscanf(arg.c_str(), "%d,%d,%d", &bx, &by, &bb);
                        SDL_Event ev{}; ev.type = act == "sdldown" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                        ev.button.button = uint8_t(bb); ev.button.x = bx; ev.button.y = by;
                        mouseX_ = bx; mouseY_ = by; mouseIn_ = true;
                        if (mode_ == Mode::Engine && lb_) EngineEvent(ev);
                        std::printf("SDLCLICK f%ld %s %d,%d b%d fade %.2f held %d\n", frame, act.c_str(), bx, by, bb, fade_, heldButton_);
                    }
                    else if (act == "var" && lb_) std::printf("VAR f%ld %s=%s\n", frame, arg.c_str(), lb_->Str(lb_->Var(arg)).c_str());
                    else if (act == "key" && lb_) lb_->Key(ax, ay, 0);      // key:VK,MODS
                    else if (act == "setvar" && lb_) {          // setvar:name,text - a string variable
                        const size_t comma = arg.find(',');
                        lb_->SetVar(arg.substr(0, comma), lb::Value(arg.substr(comma + 1)));
                    }
                    else if (act == "event" && lb_) {           // event:Item,number - send a script event
                        const size_t comma = arg.find(',');
                        if (lb::Item* it = lb_->ItemByName(arg.substr(0, comma))) it->RunScript(uint16_t(std::atoi(arg.c_str() + comma + 1)));
                    }
                }
                if (opt_.explore > 0 && mode_ == Mode::Engine && lbSection_ == "English.Play") ExploreStep(frame);
                if (DevEnv("SWEEP") && lb_ && frame >= 3000 && frame < 3300) {   // test: slow sideways sweep, log the visible cursor
                    const int x = 100 + (frame - 3000) * 140 / 100 % 420;
                    mouseX_ = x; mouseY_ = 300; mouseIn_ = true; EngineMouseMove(x, 300);
                    std::string vis;
                    for (lb::Item* c : lb_->AllItems()) if (c && !c->Dead() && lb_->IsDragged(c) && c->Visible() && c->Loaded()) vis += c->Name() + " ";
                    std::printf("SWEEP f%ld x%d %s zone %s\n", frame, x, vis.c_str(), lb_->Str(lb_->Var("pvCurZone")).c_str());
                }
                if (const char* pl = DevEnv("POSLOG"); pl && lb_) {      // test: an item's world position when it changes
                    static std::map<std::string, std::string> last;
                    for (lb::Item* it : lb_->AllItems()) {
                        if (it->Dead() || it->Name() != pl) continue;
                        char b[128]; std::snprintf(b, sizeof b, "x%d y%d vis%d L%d shifted%d(%d)", it->GetRect().left, it->GetRect().top, it->Visible(), it->Loaded(), it->babyShifted_, it->babyShift_.x);
                        if (last[pl] != b) { std::printf("POSLOG f%ld %s %s view %d xnXShiftLil=%s\n", frame, pl, b, lb_->view_.x, lb_->Str(lb_->Var("xnXShiftLil")).c_str()); last[pl] = b; }
                    }
                }
                if (DevEnv("TOMDRIFT") && lb_ && frame % 5 == 0) {   // test: Tommy's idle animations out of step with TommyA
                    static std::string last;
                    lb::Item* a = lb_->ItemByName("TommyA");
                    std::string now;
                    if (a && !a->Dead()) {
                        for (const char* n : {"TomR1", "TomR2", "TomR3", "TomR4"})
                            if (lb::Item* r = lb_->ItemByName(n)) {
                                const int d = r->GetRect().left - a->GetRect().left;
                                if (d < -12 || d > 12) { char b[48]; std::snprintf(b, sizeof b, "%s%+d ", n, d); now += b; }
                            }
                    }
                    if (now != last) { std::printf("TOMDRIFT f%ld node %s view %d: %s\n", frame, lb_->Str(lb_->Var("xnNode")).c_str(), lb_->view_.x, now.empty() ? "ok" : now.c_str()); last = now; }
                }
                if (DevEnv("TOMLOG") && lb_ && frame % 10 == 0) {     // test: where Tommy is drawn
                    static std::string last;
                    std::string now;
                    for (lb::Item* it : lb_->AllItems()) {
                        if (it->Dead() || !it->Loaded() || !it->Visible()) continue;
                        const std::string& n = it->Name();
                        if (n.rfind("Tom", 0) != 0 || n == "TomControl" || n.rfind("TomMarker", 0) == 0) continue;
                        const lb::Rect r = it->FrameRect();
                        char b[96]; std::snprintf(b, sizeof b, "%s@%d,%d ", n.c_str(), r.left - lb_->view_.x, r.top - lb_->view_.y);
                        now += b;
                    }
                    for (const char* n : {"TomR1", "TomR2", "TomR3", "TomR4"})
                        if (lb::Item* it = lb_->ItemByName(n)) { char b[64]; std::snprintf(b, sizeof b, "[%s %s x%d] ", n, it->Loaded() ? "L" : "u", it->GetRect().left - lb_->view_.x); now += b; }
                    now += "shift=" + lb_->Str(lb_->Var("xnXShiftTom")) + "," + lb_->Str(lb_->Var("xnYShiftTom"));
                    if (now != last) { std::printf("TOMLOG f%ld view %d: %s\n", frame, lb_->view_.x, now.c_str()); last = now; }
                }
                if (DevEnv("FISHBOT") && lb_ && lb_->ItemByName("PTail")) {   // test: solve the fishing picture by trying the piles
                    static int step = 0, part = 0; static long next = 0;
                    static const char* P[3] = {"Tail", "Body", "Head"}; static const char* L[3] = {"T", "B", "H"};
                    static const int px[3] = {417, 555, 527}, py[3] = {224, 170, 327};
                    auto V = [&](const std::string& n) { return lb_->HasVar(n) ? lb_->Str(lb_->Var(n)) : std::string("?"); };
                    if (frame >= next && frame > 2400) {
                        if (step == 0) {
                            part = -1;
                            for (int k = 0; k < 3; ++k) if (V(std::string("fvCur") + P[k]) != V(std::string("fvRight") + P[k])) { part = k; break; }
                            if (part < 0 || V("fbSndOn") == "1") { next = frame + 30; }
                            else {
                                const std::string top = V(std::string(L[part]) + V(std::string("fnFrame") + P[part]));
                                const bool want = top == V(std::string("fvRight") + P[part]);
                                std::printf("FISHBOT f%ld %s top %s right %s -> %s\n", frame, P[part], top.c_str(), V(std::string("fvRight") + P[part]).c_str(), want ? "place" : "next");
                                mouseX_ = px[part]; mouseY_ = py[part]; mouseIn_ = true; EngineMouseMove(px[part], py[part]); EngineClick(px[part], py[part], true);
                                step = want ? 1 : 10; next = frame + 4;
                            }
                        } else if (step == 10) { EngineClick(px[part], py[part], false); step = 0; next = frame + 20; }
                        else if (step < 5) { const int x = px[part] - step * 50, y = 220; mouseX_ = x; mouseY_ = y; EngineMouseMove(x, y); ++step; next = frame + 6; }
                        else { EngineClick(220, 220, false); step = 0; next = frame + 40; }
                    }
                }
                if (DevEnv("STAIRBOT") && lb_ && lbSection_ == "English.Play") {   // test: keep tossing cookies up the stairs
                    static long next = 0, t0 = -1; static int n = 0, dx = 0, dy = 0, hx = 0, hy = 0;
                    static std::mt19937 rng(std::atoi(DevEnv("STAIRBOT")));
                    lb::Item* th = lb_->ItemByName("Thrower");
                    const bool ready = th && th->Loaded() && th->Visible() && th->Enabled() && lb_->cursorShown_ &&
                                       lb_->Str(lb_->Var("hbTossing")) != "1" && frame > 600;
                    if (t0 < 0 && ready && frame >= next) {
                        t0 = frame; hx = th->GetRect().left + 75; hy = th->GetRect().top + 60;
                        int step = 10;                                          // the lowest step without a cookie
                        for (int k = 1; k <= 10; ++k) { lb::Item* c = lb_->ItemByName("Cookie" + std::to_string(k)); if (c && !c->Visible()) { step = k; break; } }
                        const int col = std::uniform_int_distribution<int>(0, 8)(rng);   // landing column (hnDifx)
                        dx = (77 + 20 * col + 10 + 75) - hx;                    // Thrower.left = 77 + 20*col (+10 into the slot)
                        dy = (266 + 8 * (step - 1) + 4) - hy;                  // hnDify = (y - Grid.top) div 8 + 1
                        mouseX_ = hx; mouseY_ = hy; EngineMouseMove(hx, hy); EngineClick(hx, hy, true);
                    } else if (t0 >= 0 && frame == t0 + 8) { EngineMouseMove(hx + dx / 2, hy + dy / 2);
                    } else if (t0 >= 0 && frame == t0 + 16) { EngineMouseMove(hx + dx, hy + dy);
                    } else if (t0 >= 0 && frame == t0 + 24) {
                        EngineClick(hx + dx, hy + dy, false);
                        std::printf("STAIRBOT f%ld throw %d dify=%s difx=%s count=%s\n", frame, ++n, lb_->Str(lb_->Var("hnDify")).c_str(),
                                    lb_->Str(lb_->Var("hnDifx")).c_str(), lb_->Str(lb_->Var("hnCookieCount")).c_str());
                        t0 = -1; next = frame + 40 + std::uniform_int_distribution<int>(0, 120)(rng);
                    }
                }
                if (DevEnv("COOKIEBOT") && lb_ && frame % 4 == 0) {      // test: keep Lil under a falling cookie
                    lb::Item* best = nullptr;
                    for (const char* n : {"Choc1", "Choc2", "Coco1", "Coco2"})
                        if (lb::Item* c = lb_->ItemByName(n); c && c->Visible() && c->Loaded() && c->GetRect().top > 40 && (!best || c->GetRect().top > best->GetRect().top)) best = c;
                    const bool holding = lb_->Str(lb_->Var("pvCookieInHand")) != "NoCookie" && !lb_->Str(lb_->Var("pvCookieInHand")).empty();
                    const int dest = lb_->Str(lb_->Var("pvCookieInHand")).rfind("Coco", 0) == 0 ? (DevEnv("COOKIEBOT2") ? 600 : 70) : 600;   // coconut: the bin; chocolate: Tommy
                    if (holding) { mouseX_ = dest; mouseY_ = 300; mouseIn_ = true; EngineMouseMove(dest, 300); }
                    else if (best) { const int x = (best->GetRect().left + best->GetRect().right) / 2; mouseX_ = x; mouseY_ = 300; mouseIn_ = true; EngineMouseMove(x, 300); }
                    static std::string lastHand;
                    const std::string hand = lb_->Str(lb_->Var("pvCookieInHand"));
                    if (hand != lastHand) { std::printf("COOKIEBOT f%ld hand %s clean %s\n", frame, hand.c_str(), lb_->Str(lb_->Var("pnCleanCookieNum")).c_str()); lastHand = hand; }
                    std::string vis;
                    for (const char* n : {"Default", "Empty", "Hot"}) if (lb::Item* c = lb_->ItemByName(n); c && c->Visible() && c->Loaded()) vis += std::string(n) + " ";
                    static std::string lastVis;
                    if (vis != lastVis) { std::printf("COOKIEBOT f%ld cursor [%s] x%d\n", frame, vis.c_str(), mouseX_); lastVis = vis; }
                }
                if (mode_ == Mode::Engine && frame == opt_.escFrame) {
                    SDL_Event ev{}; ev.type = SDL_KEYDOWN; ev.key.keysym.sym = SDLK_ESCAPE; EngineEvent(ev);
                }
                if (frame + 1 >= opt_.headlessFrames) running_ = false;
            } else {
                const Uint64 now = SDL_GetPerformanceCounter();
                acc = std::min(acc + double(now - last) / freq, 0.25);   // no spiral after a stall
                last = now;
                while (acc >= kStep) { Update(kStep); acc -= kStep; }
                tickFrac_ = acc;                                       // for drawing between ticks
            }
            Render();
        }
        if (!opt_.screenshot.empty()) Screenshot(opt_.screenshot);
        if (lb_ && DevEnv("LBDUMP")) lb_->DumpState();
        // Quitting unloads the page, as in the original player: the page's own scripts save
        // the player's progress on that (MAIN: 0xFFF7 -> event 207 writes PLAYFILE).
        if (mode_ == Mode::Engine && lb_) lb_->ClosePage();
        SaveProgress();
        return 0;
    }

private:
    // playMovie = false: a resumed player doesn't sit through the page's opening movie again.
    bool LoadPage(size_t index, bool playMovie = true) {
        const auto& pages = assets_.GetOutline().pages;
        auto sec = pages.find(opt_.section);
        if (sec == pages.end() || sec->second.empty()) { std::fprintf(stderr, "no section [%s]\n", opt_.section.c_str()); return false; }
        index %= sec->second.size();
        if (page_ && page_->archive) assets_.Remove(page_->archive);
        page_ = std::make_unique<PageState>();
        std::string err;
        page_->archive = assets_.LoadPage(opt_.section, index, &err);
        if (!page_->archive) { std::fprintf(stderr, "page: %s\n", err.c_str()); return false; }
        pageIndex_ = index;

        std::printf("[%s] %s -> %s\n", opt_.section.c_str(), sec->second[index].key.c_str(), page_->archive->Path().c_str());
        for (const auto& [tag, n] : page_->archive->TypeCounts()) std::printf("   %s x%zu\n", mohawk::TagName(tag).c_str(), n);

        if (opt_.section == "English.Play" && index == 0) LoadNode(nodeIndex_);   // start page shows a node
        else ShowBackground(*page_->archive);
        StopMovieAudio();
        auto movies = page_->archive->OfType(mohawk::Tag("tMOV"));
        if (!movies.empty()) {
            mohawk::ScriptRuntime pageObjects;
            if (pageObjects.Load(*page_->archive))
                for (const auto& o : pageObjects.Objects().objects)
                    if (o.type == 0x43 && o.rect.right > o.rect.left)
                        page_->movieRect = SDL_Rect{o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
            const bool startPage = opt_.section == "English.Play" && index == 0;
            if (startPage || !playMovie) {
                // MAIN's CryNIS is an in-game event (zoom / state sounds), not the opening.
                std::printf("   movie %s not started\n", movies.front()->name.c_str());
            } else if (auto m = assets_.Movie(*page_->archive, movies.front()->id, &err)) {
                page_->hasMovie = page_->movie.Open(*m);
                if (page_->hasMovie) {
                    page_->movie.Play();
                    StartMovieAudio(*m);
                    std::printf("   movie \"%s\" %dx%d, %.3f s -> %s\n", m->name.c_str(), m->width, m->height,
                                double(m->duration) / m->timeScale, m->file.c_str());
                }
            } else {
                std::printf("   movie %u: %s (skipped)\n", movies.front()->id, err.c_str());
                page_->hasMovie = false;
            }
        }
        SaveProgress();
        return true;
    }

    int GlobalInt(const std::string& name, int fallback) const {
        std::string v;
        if (auto pv = vars_.find(name); pv != vars_.end()) v = pv->second;          // player's state
        else if (auto it = assets_.GetOutline().globals.find(name); it != assets_.GetOutline().globals.end()) v = it->second;
        else return fallback;
        try { return std::stoi(v); } catch (...) { return fallback; }
    }

    // ---- player saves ----------------------------------------------------------------------
    // Each player is a file in the style of the game's own PROGRAM/GLOBALS:
    //   [Player]  playerName="Tommy";
    //   [Globals] the game variables, starting from PROGRAM/GLOBALS and updated as the player
    //             moves (xnNode, xnView, xvProxyToLoad, ...)
    //   [Port]    where this port left off: section, page, camera scroll, and whether the
    //             opening movie has been seen (it isn't replayed on resume).
    // Saved on every page / node change and on exit; read when that name signs in again.
    using Ini = std::map<std::string, std::map<std::string, std::string>>;
    static Ini ReadIni(const std::string& path) {
        Ini ini;
        std::ifstream in(path);
        std::string section, line;
        auto trim = [](std::string s) {
            const char* ws = " \t\r\n";
            s.erase(0, s.find_first_not_of(ws));
            s.erase(s.find_last_not_of(ws) + 1);
            return s;
        };
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
            if (!val.empty() && val.back() == ';') val = trim(val.substr(0, val.size() - 1));
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
            ini[section][key] = val;
        }
        return ini;
    }
    std::string SavePath(const std::string& name) const {
        std::string f;
        for (char ch : name) f += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
        return opt_.userDir + "/saves/" + f + ".ini";
    }
    // Loads the player's variables (GLOBALS defaults, then their save). True if a save existed.
    bool LoadPlayer(const std::string& name) {
        vars_ = ReadIni(opt_.data + "/PROGRAM/GLOBALS")["Globals"];
        port_.clear();
        std::error_code ec;
        if (!std::filesystem::exists(SavePath(name), ec)) return false;
        Ini save = ReadIni(SavePath(name));
        for (const auto& [k, v] : save["Globals"]) vars_[k] = v;
        port_ = save["Port"];
        return true;
    }
    void SaveProgress() {
        if (player_.empty() || mode_ != Mode::Play || !page_) return;
        char proxy[16];
        std::snprintf(proxy, sizeof proxy, "N%02dProxy", nodeIndex_);
        vars_["xnNode"] = std::to_string(nodeIndex_);
        vars_["xvProxyToLoad"] = proxy;
        if (page_->hasScripts && page_->bgWidth > 0) {
            const std::string view = page_->scripts.ActiveView(scrollX_ + hudX_, page_->bgWidth);
            if (view.rfind("View", 0) == 0) vars_["xnView"] = view.substr(4);
        }
        port_["section"] = opt_.section;
        port_["page"] = std::to_string(pageIndex_);
        port_["scroll"] = std::to_string(int(scrollX_));
        std::error_code ec;
        std::filesystem::create_directories(opt_.userDir + "/saves", ec);
        std::ofstream out(SavePath(player_));
        out << "[Player]\nplayerName=\"" << player_ << "\";\n\n[Globals]\n";
        auto quoted = [](const std::string& v) {             // numbers / true / false stay bare
            const bool bare = v == "true" || v == "false" ||
                (!v.empty() && v.find_first_not_of("-0123456789") == std::string::npos);
            return bare ? v : "\"" + v + "\"";
        };
        for (const auto& [k, v] : vars_) out << k << "=" << quoted(v) << ";\n";
        out << "\n[Port]\n";
        for (const auto& [k, v] : port_) out << k << "=" << quoted(v) << ";\n";
    }

    // Navigation nodes are proxies N01Proxy..N20Proxy ([Globals] xnNode / xvProxyToLoad).
    void LoadNode(int n) {
        char name[16];
        std::snprintf(name, sizeof name, "N%02dProxy", n);
        if (!assets_.GetOutline().proxies.count(name)) { std::printf("   no %s\n", name); return; }
        if (page_->node) assets_.Remove(page_->node);
        std::string err;
        page_->node = assets_.LoadProxy(name, &err);
        if (!page_->node) { std::printf("   %s: %s\n", name, err.c_str()); return; }
        nodeIndex_ = n;
        std::printf("   node %s -> %s\n", name, page_->node->Path().c_str());
        ShowBackground(*page_->node);
        SaveProgress();
    }

    // tBMP -> palette indices -> RGBA surface -> texture
    void ShowBackground(const mohawk::Archive& a) {
        page_->background.reset();
        page_->bgWidth = page_->bgHeight = 0;
        if (spriteCache_) spriteCache_->Clear();                 // sprites follow the active palette
        if (auto pal = assets_.PagePalette(a)) { page_->palette = *pal; page_->hasPalette = true; }
        const mohawk::Resource* r = assets_.BackgroundBitmap(a);
        if (!r || !page_->hasPalette) { std::printf("   no background bitmap\n"); return; }
        std::string err;
        auto img = assets_.Bitmap(a, r->id, &err);
        if (!img) { std::printf("   tBMP %u: %s\n", r->id, err.c_str()); return; }
        SDL_Surface* surf = mohawk::ToSurface(*img, page_->palette);
        if (!surf) { std::printf("   surface: %s\n", SDL_GetError()); return; }
        page_->background.reset(SDL_CreateTextureFromSurface(renderer_, surf));
        SDL_FreeSurface(surf);
        if (!page_->background) { std::printf("   texture: %s\n", SDL_GetError()); return; }
        page_->bgWidth = img->width;
        page_->bgHeight = img->height;
        // Object table of the same archive: view origins, groups, hotspots.
        std::string serr;
        page_->hasScripts = page_->scripts.Load(a, &serr);
        page_->selected = nullptr;
        if (page_->hasScripts) {
            page_->scripts.SetHandler([this](const mohawk::ScriptObject& o, mohawk::ScriptEvent) { OnObjectEvent(o); });
            const auto& t = page_->scripts.Objects();
            std::printf("   BITL: %zu objects, %zu views, %zu grouped; BCOD %s\n", t.objects.size(), t.viewOrigin.size(),
                        t.viewOf.size(), page_->scripts.Bytecode() ? "present" : "none");
        }
        // Initial view: [Globals] xnView names "View<n>"; its origin comes from BITL
        // (N01: View3 = 1371). The 640-wide view is centred in the 860-wide camera.
        // Without a View object, fall back to the n-th 640-px slice.
        const std::string view = "View" + std::to_string(GlobalInt("xnView", 1));
        const auto& origins = page_->scripts.Objects().viewOrigin;
        double viewLeft = double((GlobalInt("xnView", 1) - 1) % std::max(1, img->width / gameW_)) * gameW_;
        if (page_->hasScripts && origins.count(view)) viewLeft = origins.at(view);
        scrollX_ = opt_.scroll >= 0 ? opt_.scroll : viewLeft - hudX_;
        std::printf("   background tBMP %u \"%s\" %dx%d (format 0x%04x)\n", r->id, r->name.c_str(),
                    img->width, img->height, img->format);
    }

    // ---- sprites ----------------------------------------------------------------------
    // ToolProxy (TOOLBAR.MHK): the bar is the ANI whose bounds span the screen width
    // (3504: 0,400-640,484); its drawing script places the 640x84 bar (tBMP 3501) at
    // (0,400). Shown statically on its first frame. The buttons are the other ANIs with
    // bounds; which of them the game shows is decided by page scripts, so they are a
    // debug layer (T).
    void LoadToolbar() {
        std::string err;
        toolbarArchive_ = assets_.LoadProxy("ToolProxy", &err);
        if (!toolbarArchive_) { std::printf("toolbar: %s\n", err.c_str()); return; }
        for (const mohawk::Resource* r : toolbarArchive_->OfType(mohawk::Tag("ANI "))) {
            auto def = mohawk::LoadSprite(*toolbarArchive_, r->id, &err);
            if (!def) { std::printf("toolbar ANI %u: %s\n", r->id, err.c_str()); continue; }
            const auto& b = def->ani.bounds;
            const uint16_t script = mohawk::FirstDrawingScript(*def);
            if (b.right <= b.left || !script) continue;
            mohawk::SpriteInstance inst(toolbarArchive_, *def, script, false);
            inst.SetFrame(0);
            if (b.left == 0 && b.right - b.left >= gameW_) {
                if (!toolbarBar_) { toolbarBar_ = std::make_unique<mohawk::SpriteInstance>(inst); toolbarTop_ = b.top; }
            } else {
                toolbarButtons_.push_back(inst);
            }
        }
        std::string serr;
        hasToolbarObjects_ = toolbarObjects_.Load(*toolbarArchive_, &serr);       // click boxes (BITL)
        // What the original shows (reference playthrough): the "Stuff we gots so far" panel
        // with the three episode markers, Cry (greyed out until it can be used), Replay and
        // Quit. Pal1/Pal2/PalBase/Warp are developer buttons and ReturnButton belongs to the
        // sub-games, so they stay hidden.
        if (hasToolbarObjects_) {
            for (const auto& o : toolbarObjects_.Objects().objects) {
                const SDL_Rect r{o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
                // E1/E2/E3Marker are the episode prizes, shown once earned; a new game shows
                // the panel's empty swirls.
                if (o.type == 0x02 && (o.name == "MarkerBack" || o.name == "CryDisabled"))
                    toolPatches_.push_back(ToolPatch{o.name, o.resourceId, r});
                else if (o.type == 0x40 && (o.name == "ReplayButton" || o.name == "QuitButton")) {
                    if (auto d = mohawk::LoadSprite(*toolbarArchive_, o.resourceId)) {
                        ToolButton b{o.name, r, *d, std::make_unique<mohawk::SpriteTracks>(toolbarArchive_, *d)};
                        toolButtons_.push_back(std::move(b));
                    }
                } else if (o.type == 0x41) toolSounds_[o.name] = o.resourceId;
            }
            std::stable_sort(toolPatches_.begin(), toolPatches_.end(),        // panel under the markers
                             [](const ToolPatch& a, const ToolPatch& b) { return a.name == "MarkerBack" && b.name != "MarkerBack"; });
        }
        std::printf("toolbar: bar %s, %zu button sprites, %zu BITL objects\n", toolbarBar_ ? "found" : "missing",
                    toolbarButtons_.size(), hasToolbarObjects_ ? toolbarObjects_.Objects().objects.size() : size_t(0));
    }

    // QuitProxy (RRQUIT.MHK): a full-screen dimmer (ANI with empty bounds, shape 1 =
    // "Quitbg", whose index-0 pixels are transparent) plus the Yes/No buttons running
    // their animated scripts in a loop.
    void LoadQuitOverlay() {
        std::string err;
        const auto& proxies = assets_.GetOutline().proxies;
        
        // Safe Check: Does the proxy definition even exist in the outline?
        if (!proxies.count("QuitProxy")) {
            std::printf("quit overlay warning: QuitProxy not found in OUTLINE. Bypassing overlay initialization.\n");
            return;
        }

        quitArchive_ = assets_.Archive(proxies.at("QuitProxy").path, &err);
        
        // === CRITICAL SAFETY GUARD: Stop execution instantly if file is missing ===
        if (!quitArchive_) { 
            std::printf("quit overlay warning: %s (Check that RRQUIT.MHK is inside your PROGRAM folder!). Bypassing overlay initialization.\n", err.c_str()); 
            return; 
        }
        
        for (const mohawk::Resource* r : quitArchive_->OfType(mohawk::Tag("ANI "))) {
            auto def = mohawk::LoadSprite(*quitArchive_, r->id, &err);
            if (!def) continue;
            const auto& b = def->ani.bounds;
            if (b.right <= b.left) { if (!quitBackdrop_) quitBackdrop_ = def; continue; }
            if (const uint16_t script = mohawk::FirstDrawingScript(*def))
                quitButtons_.emplace_back(quitArchive_, *def, script, true);
        }
        if (quitObjects_.Load(*quitArchive_))
            for (const auto& o : quitObjects_.Objects().objects) {
                if (o.type == 0x41) quitSounds_[o.name] = o.resourceId;
                else if (o.name == "QuitYes" || o.name == "QuitNo")
                    quitRects_[o.name] = SDL_Rect{o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
            }
        std::printf("quit overlay: backdrop %s, %zu animated buttons\n", quitBackdrop_ ? "found" : "missing", quitButtons_.size());
    }

    // ---- page objects --------------------------------------------------------------------
    bool Wraps() const { return page_ && worldWrap_ && page_->bgWidth > width_; }
    // Canvas x where world x 0 of the scene sits when it doesn't scroll (narrow, centred).
    int SceneLeft() const { return Wraps() || !page_ || page_->bgWidth >= width_ ? 0 : (width_ - page_->bgWidth) / 2; }

    // Left click: toolbar strip -> toolbar BITL controls (4:3 coords, minus hudX_);
    // scene -> world x on the wrapping panorama -> hotspot of the view on screen.
    // Object bytecode is not executed yet.
    void HandleClick(int x, int y) {
        if (y >= toolbarTop_) { HandleToolbarClick(x - hudX_, y); return; }   // y >= 400
        if (!page_ || !page_->hasScripts || page_->bgWidth <= 0 || y >= page_->bgHeight) return;
        int wx;
        if (Wraps()) wx = ((int(scrollX_) + x) % page_->bgWidth + page_->bgWidth) % page_->bgWidth;
        else { wx = int(scrollX_) + x - SceneLeft(); if (wx < 0 || wx >= page_->bgWidth) return; }
        const std::string view = page_->scripts.ActiveView(scrollX_ + hudX_, page_->bgWidth);
        if (!page_->scripts.Click(wx, y, view, page_->bgWidth))
            std::printf("click (%d,%d) world x %d, %s: no hotspot\n", x, y, wx, view.c_str());
    }

    // Toolbar click boxes come from TOOLBAR.MHK's BITL (ToolBar 0,400-640,484, CryButton,
    // QuitButton, ...), in 4:3 coordinates. Smallest control containing the point wins.
    void HandleToolbarClick(int gx, int gy) {
        if (!hasToolbarObjects_ || !toolbarOpen_) return;
        auto in = [&](const SDL_Rect& r) { return gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h; };
        for (ToolButton& b : toolButtons_) {
            if (!in(b.rect)) continue;
            if (b.press) b.press->Start();
            if (b.name == "QuitButton") { ToolSound(*toolbarArchive_, toolSounds_, "QuitPress"); OpenQuit(); }
            else {
                ToolSound(*toolbarArchive_, toolSounds_, "ToolPress");
                std::printf("toolbar: %s (replay menu not implemented yet)\n", b.name.c_str());
            }
            return;
        }
        for (const ToolPatch& tp : toolPatches_)
            if (tp.name == "CryDisabled" && in(tp.rect)) { ToolSound(*toolbarArchive_, toolSounds_, "ToolBeep"); return; }
        const mohawk::ScriptObject* hit = nullptr;
        int best = 1 << 30;
        for (const auto& o : toolbarObjects_.Objects().objects) {
            const auto& r = o.rect;
            const int area = (r.right - r.left) * (r.bottom - r.top);
            if (o.type == 0x40 && gx >= r.left && gx < r.right && gy >= r.top && gy < r.bottom && area < best) {
                best = area; hit = &o;
            }
        }
        if (hit) std::printf("toolbar click (%d,%d) -> %s (object %u, %zu bytes of code; bytecode not executed)\n",
                             gx, gy, hit->name.c_str(), hit->objectId, hit->code.size());
        else std::printf("toolbar click (%d,%d): outside the toolbar\n", gx, gy);
    }

    void ToolSound(const mohawk::Archive& a, const std::map<std::string, uint16_t>& s, const char* name) {
        auto it = s.find(name);
        if (it != s.end()) PlaySound(a, it->second);
    }

    // Quit: the RRQUIT screen over the game. Yes quits (after its sound), No goes back,
    // anywhere else beeps.
    void OpenQuit() {
        if (!quitArchive_) { running_ = false; return; }
        showQuit_ = true;
        ToolSound(*quitArchive_, quitSounds_, "QuitSnd");
    }
    void QuitClick(int gx, int gy) {
        auto in = [&](const char* n) {
            auto it = quitRects_.find(n);
            if (it == quitRects_.end()) return false;
            const SDL_Rect& r = it->second;
            return gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h;
        };
        if (quitDelay_ > 0) return;
        if (in("QuitYes")) { ToolSound(*quitArchive_, quitSounds_, "QuitYesSnd"); SaveProgress(); quitDelay_ = 0.8; }
        else if (in("QuitNo")) { ToolSound(*quitArchive_, quitSounds_, "QuitNoSnd"); showQuit_ = false; }
        else ToolSound(*quitArchive_, quitSounds_, "QuitBeep");
    }

    // The game's own pointer (CURSOR.MHK "Default" arrow, "Hot" over things that can be
    // clicked), drawn in the canvas so it scales with the window. Its art is laid out
    // around a hotspot at (100,100) in the cursor's BITL coordinates.
    void LoadCursor() {
        std::string err;
        cursorArchive_ = assets_.LoadProxy("CursorProxy", &err);
        if (!cursorArchive_) { std::printf("cursor: %s (system cursor)\n", err.c_str()); return; }
        mohawk::ScriptRuntime objs;
        if (!objs.Load(*cursorArchive_)) return;
        for (const auto& o : objs.Objects().objects)
            if (o.type == 0x02 && (o.name == "Default" || o.name == "Hot"))
                cursors_[o.name] = CursorArt{o.resourceId, o.rect.left - 100, o.rect.top - 100};
        if (cursors_.count("Default") && opt_.headlessFrames <= 0) SDL_ShowCursor(SDL_DISABLE);
    }
    bool OverClickable() const {
        if (mode_ == Mode::SignIn && signIn_) {
            const int gx = mouseX_ - hudX_, gy = mouseY_;
            for (const auto& [name, r] : signIn_->rects)
                if ((name == "GoBtn" || name == "ListDisplay" || (name.size() >= 2 && name[0] == 'k')) &&
                    gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h) return true;
            return false;
        }
        if (mode_ != Mode::Play || cutscene_) return false;
        const int gx = mouseX_ - hudX_, gy = mouseY_;
        auto in = [&](const SDL_Rect& r) { return gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h; };
        if (showQuit_) return (quitRects_.count("QuitYes") && in(quitRects_.at("QuitYes"))) ||
                              (quitRects_.count("QuitNo") && in(quitRects_.at("QuitNo")));
        if (toolbarOpen_) for (const ToolButton& b : toolButtons_) if (in(b.rect)) return true;
        return false;
    }
    void DrawCursor() {
        if (!mouseIn_ || !cursorArchive_ || cursors_.empty() || cutscene_) return;
        const mohawk::Palette* pal = mode_ == Mode::Play ? (page_ && page_->hasPalette ? &page_->palette : nullptr)
                                                         : (front_ && front_->hasPalette ? &front_->palette : nullptr);
        if (!pal) return;
        auto it = cursors_.find(OverClickable() && cursors_.count("Hot") ? "Hot" : "Default");
        if (it == cursors_.end()) return;
        int w = 0, h = 0;
        if (SDL_Texture* t = spriteCache_->Get(*cursorArchive_, it->second.bitmap, *pal, &w, &h)) {
            const SDL_Rect d{mouseX_ + it->second.dx, mouseY_ + it->second.dy, w, h};
            SDL_RenderCopy(renderer_, t, nullptr, &d);
        }
    }

    void OnObjectEvent(const mohawk::ScriptObject& o) {
        page_->selected = &o;
        auto r = page_->scripts.Objects().WorldRect(o);
        std::printf("click -> %s (object %u, type 0x%02x, %s, world %d,%d %dx%d): %zu bytes of code, header 0x%04x"
                    " (bytecode not executed)\n", o.name.c_str(), o.objectId, o.type,
                    page_->scripts.Objects().viewOf.at(o.objectId).c_str(), r ? r->x : 0, r ? r->y : 0, r ? r->w : 0,
                    r ? r->h : 0, o.code.size(), o.CodeHeader());
    }

    // Outlines of placed objects in world space: they move with the panorama.
    void DrawObjects() {
        if (!page_ || !page_->hasScripts || page_->bgWidth <= 0) return;
        const auto& t = page_->scripts.Objects();
        for (const auto& o : t.objects) {
            auto r = t.WorldRect(o);
            if (!r || o.name.rfind("View", 0) == 0) continue;
            if (&o == page_->selected) SDL_SetRenderDrawColor(renderer_, 255, 40, 40, 255);
            else if (o.type == mohawk::kObjHotspot) SDL_SetRenderDrawColor(renderer_, 40, 255, 80, 255);
            else SDL_SetRenderDrawColor(renderer_, 255, 220, 0, 255);
            for (int k = -1; k <= 1; ++k) {                              // wrap: copies left/right
                if (!Wraps() && k) continue;
                SDL_Rect sr{r->x - int(scrollX_) + SceneLeft() + k * page_->bgWidth, r->y, r->w, r->h};
                if (sr.x + sr.w < 0 || sr.x >= width_) continue;
                SDL_RenderDrawRect(renderer_, &sr);
            }
        }
    }

    // 4:3 screen-space layers, shifted by hudX_. Full-width black backing behind the
    // centred 640x84 toolbar; its ANI clip (640x480) is extended to the canvas bottom so
    // all 84 rows show.
    void DrawSprites() {
        if (!page_ || !page_->hasPalette) return;
        const mohawk::Palette& pal = page_->palette;
        // The strip under the scene is black; the toolbar appears there only while the
        // pointer is over it (no slide, it just shows), as in the original.
        const SDL_Rect backing{0, toolbarTop_, width_, height_ - toolbarTop_};
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer_, &backing);
        if (toolbarOpen_ && toolbarArchive_) {
            for (const ToolPatch& tp : toolPatches_) {
                int w = 0, h = 0;
                if (SDL_Texture* t = spriteCache_->Get(*toolbarArchive_, tp.bitmap, pal, &w, &h)) {
                    const SDL_Rect d{hudX_ + tp.rect.x, tp.rect.y, w, h};
                    SDL_RenderCopy(renderer_, t, nullptr, &d);
                }
            }
            for (const ToolButton& b : toolButtons_) {
                if (b.press && b.press->Running()) b.press->Draw(renderer_, *spriteCache_, pal, hudX_, 0);
                else mohawk::DrawStaticShape(renderer_, *spriteCache_, *toolbarArchive_, b.def, 2, pal, hudX_, 0);
            }
        }
        
        if (showQuit_ && quitArchive_) {
            // === PASTE START: Dim the widescreen side bands to match the 4:3 center dimmer ===
            SDL_BlendMode prevBlend;
            SDL_GetRenderDrawBlendMode(renderer_, &prevBlend);
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 128);
            const SDL_Rect bands[2] = {
                {0, 0, hudX_, height_},                    // left side band (x 0-110)
                {hudX_ + gameW_, 0, hudX_, height_},       // right side band (x 750-860)
            };
            SDL_RenderFillRects(renderer_, bands, 2);
            SDL_SetRenderDrawBlendMode(renderer_, prevBlend);
            // === PASTE END ===

            if (quitBackdrop_) mohawk::DrawStaticShape(renderer_, *spriteCache_, *quitArchive_, *quitBackdrop_, 1, pal, hudX_, 0);
            for (const auto& b : quitButtons_) b.Draw(renderer_, *spriteCache_, pal, hudX_, 0);
        }
    }


    // ==== Living Books engine host ===========================================================
    // Every page (intro, sign-in, the game) runs through lb::Engine from its own BITL/BCOD.
    // The host loads pages named by OUTLINE, applies page fades ("fade" pages in OUTLINE:
    // the control screens), and supplies sound, movies, input and drawing.
    static std::string SectionForMode(int mode) {
        switch (mode) {
        case 1: return "Intro"; case 2: return "Control"; case 3: return "Credits"; case 4: return "Preview";
        case 5: return "English.Read"; case 6: return "English.Play"; case 7: return "Demo";
        default: return "";
        }
    }
    static int ModeForSection(const std::string& s) {
        for (int m = 1; m <= 7; ++m) if (SectionForMode(m) == s) return m;
        return 6;
    }
    static void ParsePageKey(const std::string& key, int* page, int* sub) {
        *page = 0; *sub = 0;
        std::sscanf(key.c_str(), "Page%d.%d", page, sub);
    }
    int FindPage(const std::string& section, int page, int sub) const {
        const auto& pages = assets_.GetOutline().pages;
        auto it = pages.find(section);
        if (it == pages.end()) return -1;
        for (size_t i = 0; i < it->second.size(); ++i) {
            int p, s;
            ParsePageKey(it->second[i].key, &p, &s);
            if (p == page && s == sub) return int(i);
        }
        return -1;
    }

    bool StartEngine() {
        lb::Host h;
        h.openArchive = [this](const std::string& path) { std::string err; mohawk::Archive* a = assets_.Archive(path, &err); if (!a) std::printf("archive %s: %s\n", path.c_str(), err.c_str()); return a; };
        h.programFile = [this](const std::string& name) {
            namespace fs = std::filesystem;
            std::error_code ec;
            // The game writes PLAYFILE (players' progress) and GLOBALS: they live in the user's
            // folder (AppData), seeded from the installed copies, so a normal (non-admin) user
            // never writes into Program Files - and never gets a separate VirtualStore copy.
            {
                std::string lower = name;
                for (char& ch : lower) ch = char(std::tolower((unsigned char)ch));
                if ((lower == "playfile" || lower == "globals") && opt_.userDir != opt_.data) {
                    std::string upper = lower;
                    for (char& ch : upper) ch = char(std::toupper((unsigned char)ch));
                    const fs::path mine = fs::path(opt_.userDir) / upper;
                    if (!fs::exists(mine, ec)) {
                        fs::create_directories(opt_.userDir, ec);
                        for (auto& e : fs::directory_iterator(fs::path(opt_.data) / "PROGRAM", ec)) {
                            std::string n = e.path().filename().string();
                            for (char& ch : n) ch = char(std::tolower((unsigned char)ch));
                            if (n == lower) { fs::copy_file(e.path(), mine, ec); break; }
                        }
                    }
                    return mine.string();
                }
            }
            const fs::path dir = fs::path(opt_.data) / "PROGRAM";
            for (auto& e : fs::directory_iterator(dir, ec)) {
                std::string n = e.path().filename().string(), a = name;
                for (char& ch : n) ch = char(std::tolower((unsigned char)ch));
                for (char& ch : a) ch = char(std::tolower((unsigned char)ch));
                if (n == a) return e.path().string();
            }
            return (dir / name).string();
        };
        h.playSound = [this](const mohawk::Archive& a, uint16_t id) { return EngineSound(SoundPath(a, id)); };
        h.playWav = [this](const std::string& wav) { return EngineSound(wav); };
        h.stopSound = [this](int v) { if (audio_) audio_->Stop(v); lbVoices_.erase(v); lbPausedLeft_.erase(v); };
        // a paused voice keeps the rest of its length and counts as playing until it resumes
        h.pauseSound = [this](int v, bool p) {
            auto it = lbVoices_.find(v);
            if (it == lbVoices_.end()) return;
            if (p && !lbPausedLeft_.count(v)) { lbPausedLeft_[v] = it->second > lbTime_ ? it->second - lbTime_ : 0; }
            else if (!p && lbPausedLeft_.count(v)) { it->second = lbTime_ + lbPausedLeft_[v]; lbPausedLeft_.erase(v); }
            else return;
            if (audio_ && v > 0) audio_->Pause(v, p);
            if (opt_.headlessFrames > 0) std::printf("   sound voice %d %s\n", v, p ? "paused" : "resumed");
        };
        h.soundPlaying = [this](int v) { auto it = lbVoices_.find(v); return it != lbVoices_.end() && (lbPausedLeft_.count(v) || lbTime_ < it->second); };
        // (headless test runs go faster than real time: their pictures run on the game clock)
        h.soundTime = [this](int v) { return opt_.headlessFrames <= 0 && v > 0 && audio_ && audio_->Ready() ? audio_->Position(v) : -2.0; };   // -2: no sound clock, -1: the voice has ended
        h.movie = [this](const mohawk::Archive& a, uint16_t id) { std::string err; auto m = assets_.Movie(a, id, &err); if (!m) std::printf("   movie %u: %s\n", id, err.c_str()); return m; };
        h.notify = [this](const lb::NotifyEvent& n) { EngineNotify(n); };
        h.log = [this](const std::string& s) { if (opt_.headlessFrames > 0 || lbLogAll_ || DevEnv("LBLOG")) std::printf("   [lb] %s\n", s.c_str()); };
        lb_ = std::make_unique<lb::Engine>(h);
        lb_->SetOutline(&assets_.GetOutline());
        for (const auto& [k, v] : assets_.GetOutline().globals) {
            // OUTLINE values come unquoted: numbers and true/false stay bare, the rest are strings
            std::string lv = v;
            for (char& ch : lv) ch = char(std::tolower((unsigned char)ch));
            const bool bare = !v.empty() && (lv == "true" || lv == "false" || v.find_first_not_of("-0123456789") == std::string::npos);
            if (bare) { lb::Code tmp(lb_.get()); tmp.Run(nullptr, tmp.Compile(k + " = " + v)); }
            else lb_->Var(k) = lb::Value(v);
        }
        lb_->Var("English") = lb::Value(1);
        mode_ = Mode::Engine;
        if (opt_.headlessFrames <= 0) SDL_ShowCursor(SDL_DISABLE);
        const auto& pages = assets_.GetOutline().pages;
        if (opt_.skipIntro) return OpenEnginePage("English.Play", opt_.page);
        if (opt_.startAtSignIn || !pages.count("Intro")) return OpenEnginePage("Control", 0);
        return OpenEnginePage("Intro", 0);
    }

    std::string SoundPath(const mohawk::Archive& a, uint16_t id) const {
        std::string stem = a.Path();
        const size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
        for (char& ch : stem) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        return opt_.sounds + "/" + stem + "_" + std::to_string(id) + ".wav";
    }
    // Sounds run on the game clock: a voice counts as playing for the length of its file,
    // so scripts that wait for a line to finish behave the same with or without a device.
    int EngineSound(const std::string& path) {
        if (!audio_ || !port::Audio::Exists(path)) {
            if (opt_.headlessFrames > 0) std::printf("   sound %s (missing)\n", path.c_str());
            return 0;
        }
        const double len = audio_->Duration(path);
        int v = audio_->Play(path);
        if (!v) v = lbFakeVoice_--;                               // no device: still time it
        lbVoices_[v] = lbTime_ + uint32_t(len * 1000.0);
        if (opt_.headlessFrames > 0) std::printf("   sound %s %.2fs\n", path.substr(path.find_last_of("/\\") + 1).c_str(), len);
        return v;
    }

    bool OpenEnginePage(const std::string& section, size_t index) {
        const auto& pages = assets_.GetOutline().pages;
        auto it = pages.find(section);
        if (it == pages.end() || index >= it->second.size()) { std::printf("[engine] no page %s #%zu\n", section.c_str(), index); return false; }
        const auto& pg = it->second[index];
        std::string err;
        mohawk::Archive* a = assets_.Archive(pg.path, &err);
        if (!a) { std::printf("[engine] %s: %s\n", pg.path.c_str(), err.c_str()); return false; }
        if (audio_) audio_->StopAll();
        lbVoices_.clear(); lbPausedLeft_.clear();
        for (auto& kv : lbMovieTex_) SDL_DestroyTexture(kv.second.tex);
        lbMovieTex_.clear();
        int page, sub;
        ParsePageKey(pg.key, &page, &sub);
        std::printf("[%s] %s -> %s%s\n", section.c_str(), pg.key.c_str(), a->Path().c_str(), pg.fade ? " (fade)" : "");
        lbSection_ = section;
        lbIndex_ = index;
        lbFade_ = pg.fade;
        lbTypeWait_ = 0;
        angelicaStarted_ = false;
        lbMouseX_ = lbMouseY_ = -1;                      // hand the new page's cursor the pointer position
        return lb_->OpenPage(a, 1000, ModeForSection(section), page, sub);
    }

    // opening a page after a transition: create/init, then (destFade) the slow fade-in
    std::function<void()> OpenAction(const std::string& section, int index, bool destFade) {
        return [this, section, index, destFade] {
            if (!OpenEnginePage(section, size_t(index))) return;
            // a game page's sounds wait for the fade-in (see Update)
            if (lb_) { lb_->holdVoices_ = destFade && section != "Control"; lb_->UpdateSetup(); }
            if (lb_) lb_->holdVoices_ = false;
            if (destFade) FadeIn(kFadeOut);
        };
    }
    std::string transTarget_;                        // section the current transition leads to

    // The startup intro's skip (click, Space, Enter, Esc) goes straight to the sign-in screen
    // (each logo page's script: goto(English, 2) = [Control] Page1), also while one logo is
    // fading out into the next or the next is fading in, when the page itself takes no input:
    // the fade then carries on into the sign-in screen instead of the next logo.
    void IntroSkipDuringFade() {
        if (Transitioning()) {
            if (transTarget_ == "Intro") { transNext_ = OpenAction("Control", 0, true); transTarget_ = "Control"; }
            return;
        }
        if (fade_ > 0) EngineGoTo("Control", 0);
    }
    bool IntroSkipInput(const SDL_Event& e) const {
        if (e.type == SDL_MOUSEBUTTONDOWN) return e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT;
        if (e.type != SDL_KEYDOWN || e.key.repeat) return false;
        const SDL_Keycode k = e.key.keysym.sym;
        if (e.key.keysym.mod & KMOD_SHIFT) return false;                       // (Alt+Enter skips too: Stop Movie)
        return k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER;
    }

    void EngineGoTo(const std::string& section, int index) {
        if (index < 0) { std::printf("[engine] goto: no such page in %s\n", section.c_str()); return; }
        const auto& pages = assets_.GetOutline().pages;
        const bool allowFade = lb_ ? lb_->TakePageFade() : true;
        // ... and every new page comes up slowly from black, unless the fades were turned off
        // (Tommy's jump into the cabinet game cuts straight in): the fishing-picture game fades
        // in after Grandpa's story, as in the original
        const bool destFade = allowFade;
        (void)pages;
        // the page's create/init run before it is first drawn (the sign-in page hides its debug
        // "save now" button in create: reloaded without a fade it must not flash up)
        auto open = OpenAction(section, index, destFade);
        // The player fades a page out on every page change unless the page asked it not to
        // (setpagefade(0), as before the jump into a game from the house): leaving a game for the
        // house fades slowly to black under the running man. The fade is a palette fade, so it
        // doesn't show over a movie: a cutscene that ends on a page change cuts straight across.
        const bool outFade = allowFade && (lbFade_ || !(lb_ && lb_->MovieOnScreen()));
        if (opt_.headlessFrames > 0) std::printf("[engine] goto %s #%d: pagefade %d source fade %d movie %d dest fade %d\n", section.c_str(), index, allowFade, lbFade_, lb_ ? lb_->MovieOnScreen() : 0, destFade);
        if (outFade || destFade) {
            // fade-out: the page freezes on its current frame and its sound stops
            if (audio_) audio_->StopAll();
            lbVoices_.clear(); lbPausedLeft_.clear();
            // leaving from a movie's picture (the palette fade can't darken it) cuts straight
            // to black, and the new page comes up slowly from there. The startup intro fades
            // slowly out only when it ends - after the title, or when it is skipped - and the
            // logos themselves go straight from one to the next.
            const bool introOut = lbSection_ == "Intro" && section != "Intro";
            TransitionTo(open, (outFade && !(lbSection_ == "Intro" && section == "Intro")) || introOut ? -1 : 1e-6);
            if (Transitioning()) transTarget_ = section;
        } else {
            lbPending_ = open;                                    // switch after this update
        }
    }

    void EngineNotify(const lb::NotifyEvent& n) {
        const auto& pages = assets_.GetOutline().pages;
        switch (n.kind) {
        case lb::NotifyEvent::kChangeMode: {
            const std::string sec = n.mode ? SectionForMode(n.mode) : lbSection_;
            int idx = FindPage(sec, n.page, n.subpage);
            if (idx < 0) idx = FindPage(sec, n.page, n.subpage ? 0 : 1);
            // "Continue the Adventure" goes to the player's bookmark; a player whose progress
            // was never saved has none: start the adventure (page 1) instead of going nowhere.
            if (idx < 0 && sec == "English.Play") { std::printf("[engine] no bookmark page %d: starting at page 1\n", n.page); idx = FindPage(sec, 1, 1); if (idx < 0) idx = 0; }
            EngineGoTo(sec, idx);
            break;
        }
        case lb::NotifyEvent::kGotoPage: {
            if (n.param == 0xfffe || n.param == 0xffff) {
                const int next = int(lbIndex_) + (n.param == 0xfffe ? 1 : -1);
                if (pages.count(lbSection_) && next >= 0 && next < int(pages.at(lbSection_).size())) EngineGoTo(lbSection_, next);
                else EngineGoTo("Control", 0);
            } else if (n.param == 0) {
                EngineGoTo(lbSection_, int(lbIndex_));     // goto(0): the current page again (sign-in after deleting a player)
            } else {
                int idx = FindPage(lbSection_, n.param, 1);
                if (idx < 0) idx = FindPage(lbSection_, n.param, 0);
                EngineGoTo(lbSection_, idx);
            }
            break;
        }
        case lb::NotifyEvent::kGotoControls: EngineGoTo("Control", 0); break;
        case lb::NotifyEvent::kQuit: running_ = false; break;
        case lb::NotifyEvent::kGotoQuit: std::printf("[engine] gotoquit ignored\n"); break;
        default: break;
        }
    }

    void UpdateEngine(double dt) {
        lbTime_ += uint32_t(dt * 1000.0 + 0.5);
        // the engine ignores the mouse during fades; when it runs again (or a new page opened),
        // its cursor picks up where the pointer is now - where the running man was
        if (mouseIn_ && (mouseX_ != lbMouseX_ || mouseY_ != lbMouseY_)) EngineMouseMove(mouseX_, mouseY_);
        lb_->Update(lbTime_);
        SignInAngelica();
        Autosave(dt);
        for (auto it = lbVoices_.begin(); it != lbVoices_.end();) it = !lbPausedLeft_.count(it->first) && lbTime_ >= it->second + 2000 ? lbVoices_.erase(it) : std::next(it);
        if (lbPending_) { auto f = std::move(lbPending_); lbPending_ = nullptr; f(); }
    }

    // Autosave: the game saves the player's progress (MainControl event 207 writes PLAYFILE)
    // when its page unloads - on quitting, or when the window is closed. Every 45 s of quiet
    // play the same save runs, so a crash or power cut loses little.
    double autosaveT_ = 0;
    void Autosave(double dt) {
        if (lbSection_ != "English.Play") { autosaveT_ = 0; return; }
        if ((autosaveT_ += dt) < 45.0) return;
        if (Busy() || !lb_->cursorShown_ || !lb_->Movies().empty() || lb_->Focus()) return;   // wait for a quiet moment
        autosaveT_ = 0;
        if (lb_->Var("debug").ToInt() || lb_->Var("xbSaveState").IsZero() || lb_->Var("playerFileHdl").type != lb::VT::LBX) return;
        if (lb::Item* mc = lb_->ItemByName("MainControl"); mc && !mc->Dead()) {
            mc->RunScript(207);
            std::printf("[engine] autosave\n");
        }
    }

    // Sign-in: Angelica's name instructions (SignInPromptControl -> SignInPrompt). The page's
    // own script plays them after 5-12 s of idling while no name is chosen or typed, and
    // again as a reminder; that is left as it is. The one change: with no saved players she
    // starts as soon as the sign-in screen is up. (Reptar's blink is the page's own: once,
    // after ~25 s of idling.)
    bool angelicaStarted_ = false;
    void SignInAngelica() {
        if (lbSection_ != "Control") { angelicaStarted_ = false; return; }
        lb::Item* ctl = lb_->ItemByName("SignInPromptControl");
        if (!ctl || ctl->Dead() || !lb_->HasVar("t_listlen")) return;
        const bool havePlayers = lb_->Var("t_listlen").ToInt() > 0;
        if (havePlayers) {
            angelicaStarted_ = true;                      // no early start; her idle reminder still applies
        } else if (!angelicaStarted_ && ctl->timingMode_ == lb::kAutoUserIdle && !ctl->IsPlaying()) {
            // her prompt itself, now (its own script then stops the control's idle timer);
            // the control's animation is just the 5 s wait before it
            lb::Item* prompt = lb_->ItemByName("SignInPrompt");
            if (prompt && !prompt->Dead() && lb_->Str(lb_->Var("curWord")).empty()) {
                angelicaStarted_ = true;
                ctl->CancelTimer();
                prompt->TogglePlaying(false, true);
            }
        }
    }

    // ---- headless explorer: clicks every object with a mouse script, in every view it reaches,
    // then turns / walks on. Engine log lines (unimplemented calls, script errors, missing
    // media) land in the output between the EXPLORE lines.
    struct ExploreView { std::vector<std::string> todo; size_t nav = 0, again = 0; bool built = false; };
    std::map<std::string, ExploreView> exViews_;
    int exIdle_ = 0, exDone_ = 0, exMovieFrames_ = 0;
    std::string exLast_;
    void ExploreStep(long frame) {
        if (!lb_->Movies().empty()) {                           // cut scenes: skip after a moment
            if (++exMovieFrames_ == 90) { EngineSkip(); std::printf("EXPLORE f%ld skip movie\n", frame); }
            exIdle_ = 0;
            return;
        }
        exMovieFrames_ = 0;
        if (!lb_->cursorShown_) { exIdle_ = 0; return; }
        if (++exIdle_ < 45) return;
        exIdle_ = 0;
        if (exDone_ >= opt_.explore) { if (exDone_++ == opt_.explore) std::printf("EXPLORE finished\n"); return; }
        const lb::Point view = lb_->view_;
        const int ww = lb_->world_.Width() > 0 ? lb_->world_.Width() : 0;
        auto onScreen = [&](lb::Item* it, lb::Point& at) -> bool {
            lb::Rect r = it->FrameRect();
            if (r.Width() <= 0 || r.Height() <= 0) r = it->GetRect();
            for (int k = -1; k <= 1; ++k) {
                const int sh = lb_->wrap_ ? k * ww : 0;
                if (!lb_->wrap_ && k) continue;
                const int l = std::max(r.left + sh, view.x), t = std::max(r.top, view.y);
                const int rr = std::min(r.right + sh, view.x + 640), b = std::min(r.bottom, view.y + 400);
                if (rr - l < 4 || b - t < 4) continue;
                // a point inside where this item is the one hit
                for (int gy = 1; gy < 8; ++gy) for (int gx = 1; gx < 8; ++gx) {
                    lb::Point p{l + (rr - l) * gx / 8, t + (b - t) * gy / 8};
                    if (lb_->ItemAt(p) == it) { at = lb::Point{p.x - view.x, p.y - view.y}; return true; }
                }
            }
            return false;
        };
        auto clickable = [&](lb::Item* it) {
            if (it->Dead() || !it->Loaded() || !it->Visible() || !it->Enabled()) return false;
            return it->HasScript(lb::kEvMouseDown) || it->HasScript(lb::kEvMouseUp) || it->HasScript(lb::kEvMouseUpIn);
        };
        auto isNav = [](const std::string& n) { return n == "LeftZone" || n == "RightZone" || n.rfind("Fwd", 0) == 0; };
        auto isMenu = [](const std::string& n) { return n.find("Quit") != std::string::npos || n.find("Replay") != std::string::npos || n.find("Save") != std::string::npos; };
        // a "screen" = room view + the set of clickable things on it (inventory, close-ups, ...)
        std::vector<std::string> names;
        for (lb::Item* it : lb_->Created()) {
            lb::Point at;
            if (clickable(it) && !isNav(it->Name()) && !isMenu(it->Name()) && onScreen(it, at)) names.push_back(it->Name());
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        std::string sig;
        for (auto& n : names) sig += n + ",";
        const std::string key = lb_->Str(lb_->Var("xvCurNode")) + "/" + lb_->Str(lb_->Var("xvCurView"));
        ExploreView& v = exViews_[key + "|" + sig];
        if (!v.built) {
            v.built = true;
            v.todo = names;
            // small things first: full-screen pictures (the open diaper) usually close the screen
            std::stable_sort(v.todo.begin(), v.todo.end(), [&](const std::string& a, const std::string& b) {
                lb::Item* x = lb_->ItemByName(a); lb::Item* y = lb_->ItemByName(b);
                auto area = [](lb::Item* i) { if (!i) return 0; lb::Rect r = i->FrameRect(); return r.Width() * r.Height(); };
                return area(x) < area(y);
            });
            // holding a tool (CurScrew): its matching zone (ScrewZone) and other zones first
            const std::string tool = lb_->Str(lb_->Var("xvTool"));
            if (tool.rfind("Cur", 0) == 0) {
                const std::string base = tool.substr(3);
                v.todo.erase(std::remove(v.todo.begin(), v.todo.end(), tool), v.todo.end());
                std::stable_sort(v.todo.begin(), v.todo.end(), [&](const std::string& a, const std::string& b) {
                    auto rank = [&](const std::string& n) { return n.find(base) != std::string::npos ? 0 : n.find("Zone") != std::string::npos ? 1 : 2; };
                    return rank(a) < rank(b);
                });
            }
            std::printf("EXPLORE f%ld screen %s: %s\n", frame, key.c_str(), sig.c_str());
        }
        auto click = [&](lb::Item* it, const char* what) {
            lb::Point at;
            if (!onScreen(it, at)) return false;
            std::printf("EXPLORE f%ld %s %s %s at %d,%d\n", frame, key.c_str(), what, it->Name().c_str(), at.x, at.y);
            lb_->MouseMove(at.x, at.y); lb_->MouseDown(at.x, at.y); lb_->MouseUp(at.x, at.y);
            ++exDone_;
            return true;
        };
        while (!v.todo.empty()) {
            const std::string n = v.todo.front();
            v.todo.erase(v.todo.begin());
            lb::Item* it = lb_->ItemByName(n);
            if (it && clickable(it) && click(it, "click")) return;
        }
        // alternate: click something here again (new random lines, tools on things), or move on
        if (!names.empty() && (v.again++ % 2) == 0) {
            if (lb::Item* it = lb_->ItemByName(names[(v.again / 2) % names.size()])) if (click(it, "again")) return;
        }
        // navigation: forward zones first, then turn right
        std::vector<lb::Item*> nav;
        for (lb::Item* it : lb_->Created()) {
            lb::Point at;
            if (clickable(it) && it->Name().rfind("Fwd", 0) == 0 && onScreen(it, at)) nav.push_back(it);
        }
        if (lb::Item* r = lb_->ItemByName("RightZone")) nav.push_back(r);
        for (size_t k = 0; k < nav.size(); ++k) {
            lb::Item* it = nav[(v.nav + k) % nav.size()];
            if (click(it, "navigate")) { v.nav = (v.nav + k + 1) % nav.size(); return; }
        }
        // nothing new here: click the clickables again (closes overlays such as the diaper)
        if (!names.empty()) {
            if (lb::Item* it = lb_->ItemByName(names[v.nav++ % names.size()])) if (click(it, "again")) return;
        }
        std::printf("EXPLORE f%ld %s: stuck\n", frame, key.c_str());
    }

    // canvas x -> original 640x480 view coordinates
    // The original screen is 640x480: the widescreen side bands only show more of the room,
    // so the pointer there counts as the nearest edge of the 640 frame (e.g. clicking the
    // edge strips around the diaper close-up closes it, as clicking its edges does).
    static int FrameX(int x) { return std::clamp(x, 0, 639); }
    bool OutsideFrame(int x, int y) const { return x < 0 || y < 0 || x >= width_ || y >= height_; }
    bool systemPointer_ = false;
    static int FrameY(int y) { return std::clamp(y, 0, 479); }
    void EngineMouseMove(int x, int y) { lbMouseX_ = x; lbMouseY_ = y; lb_->MouseMove(FrameX(x - hudX_), FrameY(y)); }
    int lbMouseX_ = -1, lbMouseY_ = -1;              // last pointer position the engine was given
    void EngineClick(int x, int y, bool down) {
        if (lbSection_ == "Intro") { if (down) EngineSkip(); return; }   // a click skips the intro (like Space)
        // a cut scene (movie playing, game cursor hidden): a click skips it, as in the original
        if (down && !lb_->cursorShown_ && !lb_->Movies().empty()) { EngineSkip(); return; }
        if (down) lb_->MouseDown(FrameX(x - hudX_), FrameY(y)); else lb_->MouseUp(FrameX(x - hudX_), FrameY(y));
        EngineSettle();
    }
    // Input is handled the moment it arrives, between the 60 Hz engine ticks, and the window
    // draws at the display's rate: a click that hides a baby's home picture would leave the
    // spot empty on screen until the next tick starts the reaction (a flicker on fast
    // displays). A zero-time engine update right after the input starts what came due.
    void EngineSettle() {
        if (lb_ && mode_ == Mode::Engine && !Transitioning() && fade_ <= 0) lb_->Update(lbTime_);
    }
    // The menu notifies the original sends: 101 = skip (Space/Esc on movies), 103 = quit.
    void EngineSkip() { if (!CreditsLocked()) lb_->NotifyAll(101, 0xffff); }
    // The end credits that follow the final ending play through: no click, Space or Esc stops
    // them (watched again from the Replay screen they can still be skipped).
    bool CreditsLocked() const { return lb_ && lb_->CreditsRolling(); }
    void EngineChar(char c) { lb_->Key(0, 0, (unsigned char)c); }
    static int VirtualKey(SDL_Keycode k) {
        if (k >= SDLK_a && k <= SDLK_z) return int(k - SDLK_a) + 'A';
        if (k >= SDLK_0 && k <= SDLK_9) return int(k - SDLK_0) + '0';
        switch (k) {
        case SDLK_SPACE: return 0x20; case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0d; case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB: return 0x09; case SDLK_ESCAPE: return 0x1b; case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28; case SDLK_DELETE: return 0x2e; case SDLK_INSERT: return 0x2d;
        case SDLK_HOME: return 0x24; case SDLK_END: return 0x23; case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22;
        case SDLK_PERIOD: return 0xbe;
        default: return 0;
        }
    }
    // ---- the original player's menu bar and shortcuts (the original RUGRATS.EXE: MENU 128, ACCELERATORS 128)
    //   File:    Stop Movie (101)  Go To Replay (102)  About (109)  Exit (103)
    //   Options: Display > Disable Mode Changes (202), Disable Warnings (203), Zoom (204)
    //            Volume  > Mute (205), Increase (206), Decrease (207)
    // Hidden while playing; Alt (pressed and released on its own) shows or hides it, as the
    // manual's shortcut table says. Shortcuts: Alt+Enter (Stop Movie, as in the manual), Ctrl+R,
    // Ctrl+V, Ctrl+Q, Ctrl+M, Ctrl+Up, Ctrl+Down (plus the manual's Alt+R and the ReadMe's
    // Ctrl+Enter). The port's own full screen / window switch is F11.
    int Accelerator(const SDL_Keysym& ks) const {
        const bool ctrl = ks.mod & KMOD_CTRL, alt = ks.mod & KMOD_ALT;
        const SDL_Keycode k = ks.sym;
        if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (ctrl || alt) && !(ks.mod & KMOD_SHIFT)) return 101;   // Alt+Enter / Ctrl+Enter
        if (k == SDLK_r && (ctrl || alt)) return 102;
        if (k == SDLK_v && ctrl) return 109;
        if (k == SDLK_q && ctrl) return 103;
        if (k == SDLK_m && ctrl) return 205;
        if (k == SDLK_UP && ctrl) return 206;
        if (k == SDLK_DOWN && ctrl) return 207;
        return 0;
    }
    bool MenuEnabled(int id) const {
        switch (id) {
        case 101: case 102: return mode_ == Mode::Engine && lb_ && lb_->MenuItemEnabled(id);
        case 103: return mode_ == Mode::Engine && lb_;
        case 202: case 203: return false;            // the old display driver's options: nothing to change here
        case 206: return muted_ || volume_ < kVolumeSteps;
        case 207: return muted_ || volume_ > 0;
        default: return true;
        }
    }
    void MenuCommand(int id) {
        if (!MenuEnabled(id)) return;
        switch (id) {
        case 101: case 102: case 103:
            if (Transitioning()) return;
            lb_->NotifyAll(uint16_t(id), 0xffff);
            break;
        case 109: {
            const std::string text =
                "Rugrats Adventure Game\n"
                "Version 1.0 (x64)\n"
                "Copyright 1998, Broderbund Software\n"
                "Ported by T.M. DEVELOPMENTS (2026). Developed with Claude by Anthropic, PBC.\n\n"
                "Unofficial port by T.M. DEVELOPMENTS. Not endorsed by or affiliated with "
                "HMH Education Company, Encore, Inc., Nickelodeon or Skydance Corporation. "
                "It contains no game content; it plays the files copied from your own original CD-ROM.\n\n"
                "Port code: GNU GPL v3 or later. Uses SDL2 (zlib License) and FFmpeg (LGPL/GPL). "
                "See LICENSE.TXT and THIRD-PARTY-NOTICES.TXT in the game folder.";
            if (audio_) audio_->PauseOutput(true);       // the game waits behind the box; so does its sound
            // exclusive full screen can't show a window over it: a borderless window meanwhile
            const bool exclusive = IsFullscreen() && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != SDL_WINDOW_FULLSCREEN_DESKTOP;
            if (exclusive) SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "About Rugrats Adventure Game", text.c_str(), window_);
            if (exclusive) SetFullscreen(true);
            if (audio_) audio_->PauseOutput(false);
            break;
        }
        case 204: SetFullscreen(!IsFullscreen()); break;   // Zoom: the game filling the screen
        case 205: muted_ = !muted_; ApplyVolume(); break;
        case 206: if (!muted_) volume_ = std::min(volume_ + 1, kVolumeSteps); muted_ = false; ApplyVolume(); break;
        case 207: if (!muted_) volume_ = std::max(volume_ - 1, 0); muted_ = false; ApplyVolume(); break;
        default: break;
        }
        SyncMenuBar();
    }
    // ---- display: exclusive full screen (default), borderless full screen, or a window ------
    // The 640x480 screen is scaled to fit, keeping its 4:3 shape (black bars at the sides on a
    // wide display). F11 (and Options > Display > Zoom) switch between full
    // screen and a window; the window has the menu bar (Alt shows / hides it), full screen
    // never shows it. Start-up mode: --fullscreen / --borderless / --windowed, else
    // settings.ini [Display] mode=fullscreen|borderless|windowed, else exclusive full screen.
    bool startFullscreen_ = true;
    bool borderless_ = false;                        // full screen as a borderless desktop-sized window
    void ChooseDisplayMode() {
        using D = Options::Display;
        D d = opt_.display;
        if (d == D::Default) {
            const std::string m = Lower(ReadIni(SettingsPath())["Display"]["mode"]);
            d = m == "borderless" ? D::Borderless : (m == "windowed" || m == "window") ? D::Window : D::Fullscreen;
        }
        borderless_ = d == D::Borderless;
        startFullscreen_ = d != D::Window && opt_.headlessFrames == 0;
    }
    bool IsFullscreen() const { return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN); }
    void SetFullscreen(bool on) {
        if (!window_ || opt_.headlessFrames > 0) return;
        if (!on) {
            // Back to a window. The icon is set before the switch and again after it (Windows
            // shows the title bar as soon as the style changes; the icon follows a moment later).
            ApplyWindowIcons();
            SDL_SetWindowFullscreen(window_, 0);
            SyncMenuBar();
            ApplyWindowIcons();
            return;
        }
        ShowMenuBar(false);
        if (!borderless_) {
            // exclusive: the display stays at its own (desktop) resolution; the game is scaled
            SDL_DisplayMode dm;
            const int di = std::max(0, SDL_GetWindowDisplayIndex(window_));
            if (SDL_GetDesktopDisplayMode(di, &dm) == 0) SDL_SetWindowDisplayMode(window_, &dm);
            if (SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN) == 0) { SyncMenuBar(); return; }
        }
        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SyncMenuBar();
    }
    // the SDL render driver for --renderer / settings.ini [Display] renderer= (default Direct3D 11)
    std::string RenderDriverName() const {
        std::string r = Lower(opt_.renderer.empty() ? ReadIni(SettingsPath())["Display"]["renderer"] : opt_.renderer);
        if (r == "d3d9" || r == "dx9" || r == "direct3d9" || r == "direct3d") return "direct3d";
        if (r == "opengl" || r == "ogl" || r == "gl") return "opengl";
        if (r == "software" || r == "sw") return "software";
        return "direct3d11";
    }
    static std::string Lower(std::string t) { for (char& c : t) c = char(std::tolower((unsigned char)c)); return t; }

    // Volume is the computer's in the original (the wave device), so it isn't per player:
    // kept in saves/settings.ini.
    static constexpr int kVolumeSteps = 10;
    int volume_ = kVolumeSteps;
    bool muted_ = false;
    std::string SettingsPath() const { return opt_.userDir + "/settings.ini"; }
    void LoadVolume() {
        Ini ini = ReadIni(SettingsPath());
        try { if (ini["Volume"].count("level")) volume_ = std::clamp(std::stoi(ini["Volume"]["level"]), 0, kVolumeSteps); } catch (...) {}
        muted_ = ini["Volume"]["mute"] == "1";
        ApplyVolume(false);
    }
    void ApplyVolume(bool save = true) {
        if (audio_) audio_->SetVolume(float(volume_) / kVolumeSteps, muted_);
        if (!save || opt_.headlessFrames > 0) return;
        std::error_code ec;
        std::filesystem::create_directories(opt_.userDir + "/saves", ec);
        Ini old = ReadIni(SettingsPath());
        std::ofstream out(SettingsPath());
        out << "[Volume]\nlevel=" << volume_ << ";\nmute=" << (muted_ ? 1 : 0) << ";\n";
        // [Display] mode=fullscreen|borderless|windowed: how the game starts (kept as written)
        out << "[Display]\nmode=" << (old["Display"].count("mode") ? old["Display"]["mode"] : std::string("fullscreen")) << ";\n";
        if (old["Display"].count("renderer")) out << "renderer=" << old["Display"]["renderer"] << ";\n";
    }
    bool altAlone_ = false;
    bool menuOnNextWindow_ = true;     // the menu bar shows when the game becomes a window (not after F11)
    // Alt on its own toggles the menu bar; menu clicks arrive as WM_COMMAND. True: event used.
    bool MenuEvent(const SDL_Event& e) {
        // F11: between full screen and a window (the window has the menu bar; after F11 it
        // stays hidden until Alt). Alt+Enter is the original player's Stop Movie (manual).
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F11 && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT))) {
            altAlone_ = false;
            if (IsFullscreen()) menuOnNextWindow_ = false;   // F11 to a window: no menu bar until Alt
            SetFullscreen(!IsFullscreen());
            return true;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) altAlone_ = e.key.keysym.sym == SDLK_LALT || e.key.keysym.sym == SDLK_RALT;
        else if (e.type == SDL_MOUSEBUTTONDOWN || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) altAlone_ = false;
        else if (e.type == SDL_KEYUP && altAlone_ && (e.key.keysym.sym == SDLK_LALT || e.key.keysym.sym == SDLK_RALT)) {
            altAlone_ = false;
            if (!IsFullscreen()) ShowMenuBar(!menuShown_);   // full screen never shows the menu bar
            return true;
        }
#ifdef _WIN32
        if (e.type == SDL_SYSWMEVENT && e.syswm.msg && e.syswm.msg->msg.win.msg == WM_COMMAND &&
            HIWORD(e.syswm.msg->msg.win.wParam) == 0) {
            MenuCommand(LOWORD(e.syswm.msg->msg.win.wParam));
            return true;
        }
#endif
        return false;
    }
    bool menuShown_ = false;
#ifdef _WIN32
    HWND hwnd_ = nullptr;
    HMENU menuBar_ = nullptr;
    std::map<int, int> menuState_;                 // what the menu shows now: bit 0 enabled, bit 1 checked
    // While Windows runs a modal loop (a menu open, the window being moved or sized) the game
    // doesn't run: the sound waits too, or a voice would run out ahead of the game clock and
    // the game, still timing it, would leave a gap before playing it again.
    static inline WNDPROC prevProc_ = nullptr;
    static inline port::Audio* modalAudio_ = nullptr;
    static LRESULT CALLBACK ModalProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        // Alt+Enter (Stop Movie) and Alt+R (Go To Replay) are the game's own shortcuts, not menu
        // mnemonics: Windows would beep for them while the menu bar is up
        if (msg == WM_SYSCHAR && (w == '\r' || w == 'r' || w == 'R')) return 0;
        if (modalAudio_) {
            if (msg == WM_ENTERMENULOOP || msg == WM_ENTERSIZEMOVE) modalAudio_->PauseOutput(true);
            else if (msg == WM_EXITMENULOOP || msg == WM_EXITSIZEMOVE) modalAudio_->PauseOutput(false);
        }
        return CallWindowProcW(prevProc_, h, msg, w, l);
    }
    // The exe's icon (resource 1) on the window: title bar (small), taskbar and Alt+Tab (big),
    // at the sizes this monitor's DPI wants. Set on the window itself, so it doesn't depend on
    // how the window class was registered.
    // The exe's icon on the window (title bar, taskbar, Alt+Tab), through SDL's own window-icon
    // call: the icon resource is drawn into a 32-bit surface and handed to SDL_SetWindowIcon.
    // Re-applied whenever the window gets its frame back (it starts full screen, without one).
    SDL_Surface* iconSurface_ = nullptr;
    void SetWindowIcons() {
        const int sz = std::max(32, GetSystemMetrics(SM_CXICON));
        HICON ic = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR));
        if (!ic) { std::printf("window icon: resource not loaded (error %lu)\n", GetLastError()); return; }
        ICONINFO ii{};
        if (GetIconInfo(ic, &ii) && ii.hbmColor) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;     // top-down
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            iconSurface_ = SDL_CreateRGBSurfaceWithFormat(0, sz, sz, 32, SDL_PIXELFORMAT_ARGB8888);
            HDC dc = GetDC(nullptr);
            const int rows = iconSurface_ ? GetDIBits(dc, ii.hbmColor, 0, UINT(sz), iconSurface_->pixels, &bi, DIB_RGB_COLORS) : 0;
            ReleaseDC(nullptr, dc);
            if (rows != sz && iconSurface_) { SDL_FreeSurface(iconSurface_); iconSurface_ = nullptr; }
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        DestroyIcon(ic);
        std::printf("window icon: %s (%dx%d)\n", iconSurface_ ? "ready" : "not converted", sz, sz);
        ApplyWindowIcons();
    }
    void ApplyWindowIcons() {
        if (!iconSurface_ || !window_) return;
        SDL_SetWindowIcon(window_, iconSurface_);
        if (hwnd_ && DevEnv("ICONLOG")) std::printf("icons applied: small %p style %08lx\n", (void*)SendMessageW(hwnd_, WM_GETICON, ICON_SMALL, 0), GetWindowLongW(hwnd_, GWL_STYLE));
    }
    void BuildMenuBar() {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (opt_.headlessFrames > 0 || !SDL_GetWindowWMInfo(window_, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
        hwnd_ = info.info.win.window;
        SetWindowIcons();
        modalAudio_ = audio_.get();
        prevProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ModalProc)));
        HMENU file = CreatePopupMenu(), options = CreatePopupMenu(), display = CreatePopupMenu(), volume = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, 101, L"&Stop Movie");
        AppendMenuW(file, MF_STRING, 102, L"Go To &Replay");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, 109, L"&About Rugrats Adventure Game...");
        AppendMenuW(file, MF_STRING, 103, L"E&xit");
        AppendMenuW(display, MF_STRING, 202, L"Disable &Mode Changes");
        AppendMenuW(display, MF_STRING, 203, L"Disable &Warnings");
        AppendMenuW(display, MF_STRING, 204, L"&Zoom");
        AppendMenuW(volume, MF_STRING, 205, L"&Mute");
        AppendMenuW(volume, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(volume, MF_STRING, 206, L"&Increase");
        AppendMenuW(volume, MF_STRING, 207, L"&Decrease");
        AppendMenuW(options, MF_POPUP, UINT_PTR(display), L"&Display");
        AppendMenuW(options, MF_POPUP, UINT_PTR(volume), L"&Volume");
        menuBar_ = CreateMenu();
        AppendMenuW(menuBar_, MF_POPUP, UINT_PTR(file), L"&File");
        AppendMenuW(menuBar_, MF_POPUP, UINT_PTR(options), L"&Options");
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
        // like the original: the menu bar sits on a window, and is hidden at full screen
        // (Alt still brings it up there)
        menuFullscreen_ = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        ShowMenuBar(!menuFullscreen_);
    }
    bool menuFullscreen_ = false;
    void ShowMenuBar(bool show) {
        if (!menuBar_ || show == menuShown_) return;
        menuShown_ = show;
        SyncMenuBar();
        const bool fs = SDL_GetWindowFlags(window_) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED);
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        SetMenu(hwnd_, show ? menuBar_ : nullptr);
        DrawMenuBar(hwnd_);
        if (!fs) SDL_SetWindowSize(window_, w, h);   // a window keeps its picture size; the frame grows
    }
    // greys and checks the items as the game and the settings stand (every frame; cheap)
    void SyncMenuBar() {
        if (!menuBar_) return;
        const bool fs = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (fs != menuFullscreen_) {
            menuFullscreen_ = fs;
            ShowMenuBar(!fs && menuOnNextWindow_);
            if (!fs) menuOnNextWindow_ = true;
        }   // Zoom / F11: hide at full screen, back on a window
        if (!menuShown_) return;
        for (int id : {101, 102, 103, 109, 202, 203, 204, 205, 206, 207}) {
            const bool checked = (id == 205 && muted_) || (id == 204 && fs);
            const int st = (MenuEnabled(id) ? 1 : 0) | (checked ? 2 : 0);
            auto f = menuState_.find(id);
            if (f != menuState_.end() && f->second == st) continue;
            menuState_[id] = st;
            EnableMenuItem(menuBar_, UINT(id), MF_BYCOMMAND | ((st & 1) ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(menuBar_, UINT(id), MF_BYCOMMAND | ((st & 2) ? MF_CHECKED : MF_UNCHECKED));
        }
    }
#else
    void BuildMenuBar() {}
    void ShowMenuBar(bool) {}
    void ApplyWindowIcons() {}
    void SyncMenuBar() {}
#endif

    void EngineEvent(const SDL_Event& e) {
        if (lbSection_ == "Intro" && (Transitioning() || fade_ > 0) && IntroSkipInput(e)) { IntroSkipDuringFade(); return; }
        if (Transitioning()) {
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == heldButton_) heldButton_ = 0;
            return;
        }
        switch (e.type) {
        case SDL_MOUSEMOTION: EngineMouseMove(e.motion.x, e.motion.y); break;
        // The original player takes the right button as a click too (its window passes either
        // button on and the player only checks that no other button is already down); the
        // middle button isn't passed on. Nothing is taken while a page fades in from black.
        case SDL_MOUSEBUTTONDOWN: {
            const uint8_t b = e.button.button;
            if (b != SDL_BUTTON_LEFT && b != SDL_BUTTON_RIGHT) break;
            if (heldButton_ && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(heldButton_))) heldButton_ = 0;   // its release went elsewhere
            if (heldButton_ || fade_ > 0) break;
            heldButton_ = b;
            EngineClick(e.button.x, e.button.y, true);
            break;
        }
        case SDL_MOUSEBUTTONUP:
            if (e.button.button != heldButton_ || !heldButton_) break;
            heldButton_ = 0;
            EngineClick(e.button.x, e.button.y, false);
            break;
        case SDL_TEXTINPUT: if (fade_ > 0) break; for (const char* c = e.text.text; *c; ++c) if (*c != ' ') EngineChar(*c); break;
        case SDL_KEYDOWN: {
            if (e.key.repeat) break;
            const SDL_Keycode k = e.key.keysym.sym;
            if (fade_ > 0) break;                        // no keys while a page fades in

            if (k == SDLK_ESCAPE) {
                // a movie or the intro: skip (the player's Esc). Otherwise Esc does what the
                // page's scripts give it: event 248 (the quit prompt in the game, back to the
                // picker in replay activities); pages without it (sign-in) ignore it.
                if (!lb_->Movies().empty() || lbSection_ == "Intro") { EngineSkip(); break; }
                // (like the toolbar's Quit button, which sends 248 to every item)
                for (lb::Item* it : lb_->AllItems())
                    if (!it->Dead() && it->HasScript(248)) it->RunScript(248);
                break;
            }
            if (int cmd = Accelerator(e.key.keysym)) { MenuCommand(cmd); break; }
            // key modifiers as the scripts give them (the Mac's, shifted down a byte): 1 command
            // (Ctrl on Windows, as the manual maps Cmd+Q to Ctrl+Q), 2 shift, 8 option (Alt).
            // The sign-in screen deletes the selected player with key(Backspace, 2): Shift+Backspace.
            int mods = 0;
            if (e.key.keysym.mod & KMOD_CTRL) mods |= 1;
            if (e.key.keysym.mod & KMOD_SHIFT) mods |= 2;
            if (e.key.keysym.mod & KMOD_ALT) mods |= 8;
            if (CreditsLocked() && (k == SDLK_SPACE || k == SDLK_ESCAPE || k == SDLK_RETURN)) break;
            if (int vk = VirtualKey(k)) { lb_->Key(vk, mods, k == SDLK_SPACE ? ' ' : 0); EngineSettle(); }
            break;
        }
        default: break;
        }
    }

    // ---- smooth turns: the game turns by moving the view in steps (40 px about every 1/30 s,
    // MAIN's Mover). The game keeps exactly those steps; on screen the room glides from one
    // step to the next at the display's refresh rate instead of jumping. Big jumps (a new
    // node, a zoom) are shown at once.
    double turnShown_ = 0, turnTarget_ = 0, turnFrom_ = 0, turnT0_ = 0, turnDur_ = 0.033, turnLastStep_ = -1;
    int turnLastView_ = INT32_MIN;
    double Clock() const {
        if (opt_.headlessFrames > 0) return lbTime_ / 1000.0;
        return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency());
    }
    int SmoothTurn() {
        const int v = lb_->view_.x;
        const double now = Clock();
        if (turnLastView_ == INT32_MIN || lbSection_ != "English.Play") {
            turnLastView_ = v; turnShown_ = turnTarget_ = turnFrom_ = v; return 0;
        }
        if (v != turnLastView_ && DevEnv("VIEWLOG")) {
            lb::Item* mv = lb_->ItemByName("Mover");
            lb::Item* tm = lb_->ItemByName("TubeMover");
            std::printf("VIEWLOG t=%u view %d -> %d mover %s tubemover %d\n", lb_->Now(), turnLastView_, v, mv && mv->timingMode_ ? "on" : "off", tm ? int(tm->timingMode_) : -1);
        }
        if (v != turnLastView_) {
            int d = v - turnLastView_;
            const int ww = lb_->wrap_ ? lb_->world_.Width() : 0;
            if (ww > 0) d = ((d % ww) + ww + ww / 2) % ww - ww / 2;
            turnLastView_ = v;
            // only the turn driver's own steps glide (MAIN's Mover, ticking while a turn runs);
            // anything else - a new node's view, the scripts lining a view up on arrival -
            // is shown at once, so the room doesn't slide under characters already in place
            // (the chute game's sideways moves and opening pan step the same way: TubeMover, TubeIntroPan)
            bool turning = false;
            for (const char* n : {"Mover", "TubeMover", "TubeIntroPan"})
                if (lb::Item* mover = lb_->ItemByName(n); mover && !mover->Dead() && mover->timingMode_ != 0) turning = true;
            if (std::abs(d) > 120 || !turning) {             // not a turn step: show it at once
                turnShown_ = turnTarget_ = turnFrom_ = v; turnLastStep_ = -1;
                return 0;
            }
            const double since = now - turnLastStep_;
            turnDur_ = (turnLastStep_ >= 0 && since > 0.01 && since < 0.15) ? since : 0.033;
            turnLastStep_ = now;
            // glide from the last step's place to this one's over one step's time: an even
            // movement every display frame (starting from the shown place repeated a frame)
            turnFrom_ = turnTarget_;
            turnTarget_ += d;
            turnT0_ = now;
        }
        const double k = std::clamp((now - turnT0_) / turnDur_, 0.0, 1.0);
        turnShown_ = turnFrom_ + (turnTarget_ - turnFrom_) * k;
        if (k >= 1.0) { turnShown_ = turnTarget_ = turnFrom_ = v; return 0; }
        return int(std::lround(turnShown_ - turnTarget_));
    }

    void RenderEngine() {
        const int W = width_, H = 480;
        lbBuf_.assign(size_t(W) * H, 0);
        lb::Canvas cv;
        cv.px = lbBuf_.data(); cv.w = W; cv.h = H;
        cv.originX = lb_->view_.x - hudX_;
        cv.originY = lb_->view_.y;
        cv.clip = SDL_Rect{0, 0, W, H};
        lb_->smoothDx_ = SmoothTurn();
        if (opt_.headlessFrames <= 0) lb_->SmoothPaletteFades(uint32_t(tickFrac_ * 1000.0));
        if (DevEnv("SMOOTHLOG") && lb_->smoothDx_) std::printf("SMOOTH t=%u dx %d view %d\n", lbTime_, lb_->smoothDx_, lb_->view_.x);
        lb_->Render(cv);
        // Tommy's diaper close-up is a 640-wide screen: keep the side areas clean (black) while
        // it is open, so the playpen doesn't show around it. It comes back when the close-up
        // closes (an item picked, or a click outside).
        if (lb::Item* inv = lb_->ItemByName("InvOpen"); inv && !inv->Dead() && inv->Loaded() && inv->Visible() && hudX_ > 0) {
            for (int y = 0; y < H; ++y) {
                uint8_t* row = lbBuf_.data() + size_t(y) * size_t(W);
                std::fill(row, row + std::min(hudX_, W), uint8_t(0));
                if (hudX_ + 640 < W) std::fill(row + hudX_ + 640, row + W, uint8_t(0));
            }
        }
        if (!lbTex_) lbTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        void* pixels = nullptr;
        int pitch = 0;
        if (lbTex_ && SDL_LockTexture(lbTex_, nullptr, &pixels, &pitch) == 0) {
            const mohawk::Palette& pal = lb_->CurrentPalette();
            uint32_t lut[256];
            for (int i = 0; i < 256; ++i) lut[i] = 0xff000000u | uint32_t(pal.rgb[i][0]) << 16 | uint32_t(pal.rgb[i][1]) << 8 | pal.rgb[i][2];
            lut[0] = 0xff000000u;                                 // nothing drawn = black
            for (int y = 0; y < H; ++y) {
                uint32_t* d = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + size_t(y) * size_t(pitch));
                const uint8_t* s = lbBuf_.data() + size_t(y) * size_t(W);
                for (int x = 0; x < W; ++x) d[x] = lut[s[x]];
            }
            SDL_UnlockTexture(lbTex_);
            SDL_Rect dst{0, 0, W, H};
            SDL_RenderCopy(renderer_, lbTex_, nullptr, &dst);
        }
        // movies on top, 1:1 at their item rect; a frame-wide movie gets black side bars
        std::set<port::VideoPlayer*> live;
        static const bool movDbg = DevEnv("MOVDBG") != nullptr;
        if (movDbg) {
            static size_t lastN = 0;
            const size_t n = lb_->Movies(true).size();
            if (n != lastN) std::printf("render: %zu movie(s) on screen\n", n);
            lastN = n;
        }
        for (const auto& m : lb_->Movies(true)) {
            const qtshim::DecodedFrame* f = m.player->Frame();
            if (movDbg) std::printf("render movie t=%.3f frame pts=%lld %s\n", m.player->Time() / m.player->Asset().timeScale, f ? (long long)f->pts600 : -1LL, f ? "" : "(no frame)");
            live.insert(m.player);
            if (!f) continue;
            MovieTex& t = lbMovieTex_[m.player];
            if (!t.tex || t.w != f->width || t.h != f->height) {
                if (t.tex) SDL_DestroyTexture(t.tex);
                t.tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
                t.w = f->width; t.h = f->height; t.pts = -1;
            }
            if (f->pts600 != t.pts) { SDL_UpdateTexture(t.tex, nullptr, f->bgra, f->stride); t.pts = f->pts600; }
            // movies play at window positions (the original overlays them on the screen)
            const SDL_Rect dst{hudX_ + m.rect.left, m.rect.top, f->width, f->height};
            if (f->width >= 640) {
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
                const SDL_Rect bars[2] = {{0, 0, dst.x, height_}, {dst.x + dst.w, 0, W - dst.x - dst.w, height_}};
                SDL_RenderFillRects(renderer_, bars, 2);
            }
            SDL_RenderCopy(renderer_, t.tex, nullptr, &dst);
        }
        for (auto it = lbMovieTex_.begin(); it != lbMovieTex_.end();) {
            if (!live.count(it->first)) { SDL_DestroyTexture(it->second.tex); it = lbMovieTex_.erase(it); } else ++it;
        }
    }

    // ---- front end ------------------------------------------------------------------------
    void Click(int x, int y) {
        if (cutscene_) { EndCutscene(); return; }                // a click skips a cutscene
        if (mode_ == Mode::Play && showQuit_) { QuitClick(x - hudX_, y); return; }
        if (mode_ == Mode::Play) HandleClick(x, y); else FrontClick(x - hudX_, y);
    }

    // ---- full-screen cutscenes (NIS movies) --------------------------------------------------
    // Played 1:1 at their BITL rect inside the 4:3 frame (NIS01: 640x400 at the top), black
    // everywhere else. Cutscenes after sign-in start and end with straight cuts, no fades.
    // Click / Space / Enter / Esc skips.
    bool PlayCutscene(const std::string& proxy, int offset = 0) {
        std::string err;
        mohawk::Archive* a = assets_.LoadProxy(proxy, &err);
        if (!a) { std::printf("cutscene %s: %s\n", proxy.c_str(), err.c_str()); return false; }
        const uint16_t id = uint16_t(assets_.GetOutline().proxies.at(proxy).baseId + offset);
        auto m = assets_.Movie(*a, id, &err);
        if (!m) { std::printf("cutscene %s tMOV %u: %s\n", proxy.c_str(), id, err.c_str()); return false; }
        auto v = std::make_unique<port::VideoPlayer>();
        if (!v->Open(*m)) return false;
        cutRect_ = SDL_Rect{0, 0, m->width, m->height};
        mohawk::ScriptRuntime objs;
        if (objs.Load(*a))
            for (const auto& o : objs.Objects().objects)
                if (o.type == 0x43 && o.resourceId == id && o.rect.right > o.rect.left)
                    cutRect_ = SDL_Rect{o.rect.left, o.rect.top, m->width, m->height};
        v->Play();
        StartMovieAudio(*m);
        cutscene_ = std::move(v);
        std::printf("cutscene \"%s\" (%s tMOV %u) %dx%d, %.1f s -> %s\n", m->name.c_str(), proxy.c_str(), id,
                    m->width, m->height, double(m->duration) / m->timeScale, m->file.c_str());
        return true;
    }
    void EndCutscene() {
        if (!cutscene_) return;
        StopMovieAudio();
        cutscene_.reset();
        std::printf("cutscene ended\n");
    }
    void DrawCutscene() {
        if (const qtshim::DecodedFrame* f = cutscene_->Frame()) {
            UploadMovieFrame(*f);
            const SDL_Rect d{hudX_ + cutRect_.x, cutRect_.y, f->width, f->height};
            SDL_RenderCopy(renderer_, movieTex_, nullptr, &d);
        }
    }

    // Common part of an intro / control screen: archive, palette, 640x480 backdrop, BITL.
    bool LoadFrontScreen(const std::string& section, size_t index) {
        const auto& pages = assets_.GetOutline().pages;
        if (!pages.count(section) || index >= pages.at(section).size()) return false;
        auto f = std::make_unique<FrontScreen>();
        std::string err;
        f->archive = assets_.Archive(pages.at(section)[index].path, &err);
        if (!f->archive) { std::printf("[%s] %s: %s\n", section.c_str(), pages.at(section)[index].key.c_str(), err.c_str()); return false; }
        std::printf("[%s] %s -> %s\n", section.c_str(), pages.at(section)[index].key.c_str(), f->archive->Path().c_str());
        if (auto pal = assets_.PagePalette(*f->archive)) { f->palette = *pal; f->hasPalette = true; }
        if (const mohawk::Resource* r = assets_.BackgroundBitmap(*f->archive)) {
            if (auto img = assets_.Bitmap(*f->archive, r->id, &err); img && f->hasPalette) {
                if (SDL_Surface* surf = mohawk::ToSurface(*img, f->palette)) {
                    f->background.reset(SDL_CreateTextureFromSurface(renderer_, surf));
                    f->bgW = img->width; f->bgH = img->height;
                    SDL_FreeSurface(surf);
                }
            }
        }
        f->objects.Load(*f->archive, &err);
        if (spriteCache_) spriteCache_->Clear();                 // new palette
        front_ = std::move(f);
        fade_ = 0;                                               // screens cut in; only fade-outs
        return true;
    }

    // [Intro] PageN: backdrop + the page's movie at its BITL movie rect (Movie2 is
    // 440x480 at x=100). Advances when the movie ends, or on click / Space / Enter.
    bool LoadIntro(size_t i) {
        StopMovieAudio();
        mode_ = Mode::Intro;
        introIndex_ = i;
        if (!LoadFrontScreen("Intro", i)) return false;
        for (const auto& o : front_->objects.Objects().objects)
            if (o.type == 0x43 && o.rect.right > o.rect.left)
                front_->movieRect = SDL_Rect{o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
        auto movies = front_->archive->OfType(mohawk::Tag("tMOV"));
        std::string err;
        if (!movies.empty()) {
            if (auto m = assets_.Movie(*front_->archive, movies.front()->id, &err)) {
                front_->hasMovie = front_->movie.Open(*m);
                if (front_->hasMovie) { front_->movie.Play(); StartMovieAudio(*m); }
                std::printf("   movie \"%s\" %dx%d at (%d,%d), %.2f s\n", m->name.c_str(), m->width, m->height,
                            front_->movieRect.x, front_->movieRect.y, double(m->duration) / m->timeScale);
            } else {
                std::printf("   movie %u: %s (showing the backdrop instead)\n", movies.front()->id, err.c_str());
            }
        }
        return true;
    }

    // The intro movies cut straight from one to the next (no fade in); after the last one
    // the picture fades slowly to black and the name-entry screen comes up.
    void NextIntro() {
        const auto& pages = assets_.GetOutline().pages;
        for (size_t i = introIndex_ + 1; pages.count("Intro") && i < pages.at("Intro").size(); ++i)
            if (LoadIntro(i)) return;
        LeaveIntro();
    }
    // ... and the name-entry screen fades up from black just as slowly.
    // The movie freezes on its current frame and its sound cuts off as the fade begins.
    void LeaveIntro() {
        if (Transitioning()) return;
        if (front_ && front_->hasMovie) front_->movie.Pause();
        StopMovieAudio();
        TransitionTo([this] {
            if (!EnterSignIn()) { EnterPlay(); return; }
            FadeIn(kFadeOut);
        });
    }
    void FadeIn(double seconds) { fade_ = 1.0; fadeInDur_ = seconds; }

    // ---- transitions and sound -------------------------------------------------------------
    static constexpr double kFadeOutDefault = 3.4;      // the original's slow page fade, out and in (reference video: ~7 half-second steps)
    // (test runs may set FADESEC to keep their scripted click timings)
    const double kFadeOut = DevEnv("FADESEC") ? std::atof(DevEnv("FADESEC")) : kFadeOutDefault;
    void TransitionTo(std::function<void()> next, double seconds = -1) {
        if (seconds < 0) seconds = kFadeOut;
        if (transNext_) return;                  // already leaving this screen
        transNext_ = std::move(next);
        transT_ = 0;
        transDur_ = seconds;
    }
    bool Transitioning() const { return bool(transNext_); }

    void StartMovieAudio(const port::MovieAsset& m) {
        StopMovieAudio();
        if (!m.audio.empty() && audio_) movieVoice_ = audio_->Play(m.audio);
        if (opt_.headlessFrames > 0) std::printf("   soundtrack %s\n", m.audio.empty() ? "(none)" : m.audio.c_str());
    }
    void StopMovieAudio() { if (movieVoice_ && audio_) audio_->Stop(movieVoice_); movieVoice_ = 0; }

    // tWAV <id> of <archive> -> <data>/sounds/<ARCHIVE>_<id>.wav
    int PlaySound(const mohawk::Archive& a, uint16_t id) {
        if (!audio_ || !id) return 0;
        std::string stem = a.Path();
        const size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        if (const size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
        for (char& ch : stem) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        const std::string path = opt_.sounds + "/" + stem + "_" + std::to_string(id) + ".wav";
        const int v = audio_->Play(path);
        if (opt_.headlessFrames > 0) std::printf("   sound %s_%u%s\n", stem.c_str(), id, port::Audio::Exists(path) ? "" : " (file missing)");
        return v;
    }

    // [Control] Page1.1 (SIGNIN.MHK): name entry.
    bool EnterSignIn() {
        if (!LoadFrontScreen("Control", 0)) return false;
        mode_ = Mode::SignIn;
        signIn_ = std::make_unique<SignInUI>();
        SignInUI& ui = *signIn_;
        const auto& t = front_->objects.Objects();
        std::string err;
        for (const auto& o : t.objects) {
            const SDL_Rect r{o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
            ui.rects[o.name] = r;
            const bool key = o.name.size() >= 2 && o.name[0] == 'k' && o.type == 0x40;
            if (key && r.w > 0 && r.x >= 0) {                       // kspc sits off-screen
                if (auto d = mohawk::LoadSprite(*front_->archive, o.resourceId)) {
                    ui.keys.emplace_back(*d, o.name.substr(1));
                    ui.press[o.name.substr(1)] = std::make_unique<mohawk::SpriteTracks>(front_->archive, *d);
                }
            } else if (o.type == 0x41) {                            // page sound: id1 = tWAV
                ui.sounds[o.name] = o.resourceId;
            } else if (o.name == "GoBtn") {
                ui.go = mohawk::LoadSprite(*front_->archive, o.resourceId);
                if (ui.go) ui.press["Go"] = std::make_unique<mohawk::SpriteTracks>(front_->archive, *ui.go);
            } else if (o.name == "Blink") {
                if (auto d = mohawk::LoadSprite(*front_->archive, o.resourceId))
                    ui.blink = std::make_unique<mohawk::SpriteTracks>(front_->archive, *d);
            } else if (o.name == "SignInPrompt") {
                if (auto d = mohawk::LoadSprite(*front_->archive, o.resourceId))
                    ui.prompt = std::make_unique<mohawk::SpriteTracks>(front_->archive, *d);
            } else if (o.name == "Alphabet") {
                if (auto d = mohawk::LoadSprite(*front_->archive, o.resourceId))
                    for (const auto& sh : d->shapes) ui.font.push_back(sh.bitmapId);
            } else if (o.name == "FlashCursor") {
                if (auto d = mohawk::LoadSprite(*front_->archive, o.resourceId); d && d->shapes.size() >= 2)
                    ui.cursorBitmap = d->shapes[1].bitmapId;
            }
        }
        std::ifstream in(PlayersFile());
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) ui.players.push_back(line);
        }
        std::printf("   sign-in: %zu keys, font %zu glyphs, %zu saved players, Go button %s, %zu sounds, prompt %s\n",
                    ui.keys.size(), ui.font.size(), ui.players.size(), ui.go ? "found" : "missing", ui.sounds.size(),
                    ui.prompt ? std::to_string(ui.prompt->TrackCount()).append(" tracks").c_str() : "missing");
        // Angelica's instructions: the prompt's master track starts and stops her lines.
        if (ui.prompt) {
            mohawk::Archive* arch = front_->archive;
            ui.prompt->SetSoundHook([this, arch](uint16_t id, bool start) {
                if (!signIn_) return;
                auto& voices = signIn_->promptVoices;
                if (start) voices[id] = PlaySound(*arch, id);
                else if (voices.count(id)) { if (audio_) audio_->Stop(voices[id]); voices.erase(id); }
            });
            // She explains name entry only when there are no saved players yet; starts once
            // the screen is fully up.
            ui.promptPending = ui.players.empty();
        }
        SDL_StartTextInput();
        return true;
    }

    std::string PlayersFile() const { return opt_.userDir + "/rugrats_players.txt"; }

    // Name font: A-Z -> Alphabet shapes 1-26, a-z -> 27-52, space -> 53 (blank).
    uint16_t GlyphFor(char c) const {
        const auto& f = signIn_->font;
        if (c >= 'A' && c <= 'Z' && f.size() > size_t(c - 'A')) return f[size_t(c - 'A')];
        if (c >= 'a' && c <= 'z' && f.size() > size_t(26 + c - 'a')) return f[size_t(26 + c - 'a')];
        if (c == ' ' && f.size() > 52) return f[52];
        return 0;
    }
    int TextWidth(const std::string& s) {
        int w = 0, gw = 0, gh = 0;
        for (char c : s) if (uint16_t g = GlyphFor(c); g && spriteCache_->Get(*front_->archive, g, front_->palette, &gw, &gh)) w += gw;
        return w;
    }

    void SignInSound(const char* name) {
        if (signIn_ && signIn_->sounds.count(name)) PlaySound(*front_->archive, signIn_->sounds[name]);
    }
    void SignInPress(const std::string& key) {
        auto it = signIn_->press.find(key);
        if (it != signIn_->press.end() && it->second) it->second->Start();
    }
    // The player taking over ends Angelica's demonstration (and her voice).
    void StopPrompt() {
        SignInUI& ui = *signIn_;
        ui.promptPending = false;
        if (!ui.prompt || !ui.prompt->Running()) return;
        ui.prompt->Stop();
        for (auto& [id, v] : ui.promptVoices) if (audio_) audio_->Stop(v);
        ui.promptVoices.clear();
    }

    void SignInChar(char c) {
        if (Transitioning()) return;
        std::string& n = signIn_->name;
        if (c == ' ' && (n.empty() || n.back() == ' ')) return;
        if (!GlyphFor(c)) return;
        StopPrompt();
        SignInPress(std::string(1, char(std::tolower(static_cast<unsigned char>(c)))));
        const SDL_Rect line = signIn_->rects.count("LineEditor") ? signIn_->rects["LineEditor"] : SDL_Rect{237, 38, 191, 45};
        if (TextWidth(n + c) > line.w - 8) { SignInSound("OverFlowSnd"); return; }
        n += c;
        SignInSound("TypeSnd");
    }
    void SignInDelete() {
        if (Transitioning()) return;
        StopPrompt();
        SignInPress("del");
        if (signIn_->name.empty()) return;
        signIn_->name.pop_back();
        SignInSound("DeleteSnd");
    }

    void SignInGo() {
        if (Transitioning()) return;
        std::string n = signIn_->name;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n.empty()) return;
        StopPrompt();
        SignInPress("Go");
        SignInSound("GoBtnSnd");
        auto& p = signIn_->players;
        if (std::find(p.begin(), p.end(), n) == p.end()) {
            std::ofstream(PlayersFile(), std::ios::app) << n << "\n";
        }
        std::printf("sign-in: player \"%s\"\n", n.c_str());
        player_ = n;
        TransitionTo([this] { EnterPlay(); });                    // slow fade, then the game
    }

    bool EnterPlay() {
        SDL_StopTextInput();
        front_.reset();
        signIn_.reset();
        if (spriteCache_) spriteCache_->Clear();
        mode_ = Mode::Play;
        fade_ = 0;
        // LoadPage creates page_ (null until then) and loads the page's movie; a movie
        // that hasn't been converted is reported and skipped, it doesn't stop the boot.
        if (player_.empty()) return LoadPage(opt_.page);
        const bool returning = LoadPlayer(player_);
        size_t page = opt_.page;
        if (returning) {
            if (port_.count("section") && assets_.GetOutline().pages.count(port_["section"])) opt_.section = port_["section"];
            try { page = std::stoul(port_["page"]); } catch (...) {}
        }
        nodeIndex_ = opt_.node > 0 ? opt_.node : GlobalInt("xnNode", nodeIndex_);
        const bool seen = port_["openingSeen"] == "1";
        port_["openingSeen"] = "1";
        std::printf("player \"%s\": %s\n", player_.c_str(), returning ? "resuming saved game" : "new game");
        std::optional<double> savedScroll;               // read now: loading the page saves over it
        if (returning && port_.count("scroll") && opt_.scroll < 0) {
            try { savedScroll = std::stod(port_["scroll"]); } catch (...) {}
        }
        if (!LoadPage(page, !seen)) return false;
        if (savedScroll) scrollX_ = *savedScroll;
        if (!seen) PlayCutscene("IntroProxy");           // EPISODE0 NIS 01: "We now return to Reptar..."
        SaveProgress();
        return true;
    }

    // Clicks in 4:3 coordinates (canvas x - 110).
    void FrontClick(int x, int y) {
        if (Transitioning()) return;
        if (mode_ == Mode::Intro) { LeaveIntro(); return; }   // one click skips the whole intro
        if (mode_ != Mode::SignIn || !signIn_) return;
        auto in = [&](const SDL_Rect& r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
        SignInUI& ui = *signIn_;
        // A click anywhere while Angelica is explaining cuts her off and clears the name line;
        // that click does nothing else.
        if (ui.promptPending || (ui.prompt && ui.prompt->Running())) {
            StopPrompt();
            ui.name.clear();
            return;
        }
        for (const auto& [def, key] : ui.keys) {
            if (!in(ui.rects["k" + key])) continue;
            if (key == "del") SignInDelete();
            else if (key.size() == 1) SignInChar(ui.name.empty() || ui.name.back() == ' ' ? char(std::toupper(key[0])) : key[0]);
            return;
        }
        if (ui.rects.count("GoBtn") && in(ui.rects["GoBtn"])) { SignInGo(); return; }
        // saved players, one per row in ListDisplay (row layout is this port's, not decoded)
        if (ui.rects.count("ListDisplay") && in(ui.rects["ListDisplay"])) {
            const size_t row = size_t((y - ui.rects["ListDisplay"].y - 4) / kListRowH);
            if (row < ui.players.size() && row < kListRows) { StopPrompt(); ui.name = ui.players[row]; SignInSound("SelectionSnd"); }
        }
    }

    // Keys on the intro / sign-in screens. Returns true when consumed.
    bool FrontKey(SDL_Keycode k) {
        if (Transitioning()) return k != SDLK_F11;
        if (mode_ == Mode::Intro) {
            if (k == SDLK_ESCAPE) { LeaveIntro(); return true; }                      // skip the intro
            if (k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER) { LeaveIntro(); return true; }
            return k != SDLK_F11;
        }
        if (mode_ == Mode::SignIn && signIn_) {
            if (k == SDLK_BACKSPACE) { SignInDelete(); return true; }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { SignInGo(); return true; }
            return k != SDLK_ESCAPE && k != SDLK_F11;
        }
        return false;
    }

    void UpdateFront(double dt) {
        if (!front_) return;
        front_->clock += dt;
        if (mode_ == Mode::Intro) {
            if (front_->hasMovie) {
                front_->movie.Update(dt);
                if (front_->movie.AtEnd() && !Transitioning()) NextIntro();
            } else if (front_->clock > 2.0 && !Transitioning()) {
                NextIntro();                                     // movie not converted: show the backdrop briefly
            }
        } else if (mode_ == Mode::SignIn && signIn_) {
            SignInUI& ui = *signIn_;
            for (auto& [k, p] : ui.press) if (p) p->Update(dt);
            if (ui.prompt && ui.promptPending && fade_ <= 0) { ui.promptPending = false; ui.prompt->Start(); }
            if (ui.prompt) ui.prompt->Update(dt);
            if (ui.blink) {
                if (ui.blink->Running()) ui.blink->Update(dt);
                else if ((ui.blinkWait -= dt) <= 0) {
                    ui.blink->Start();
                    ui.blinkWait = std::uniform_real_distribution<double>(2.5, 6.0)(rng_);
                }
            }
        }
    }

    void DrawGlyph(uint16_t bitmap, int x, int y, int* advance) {
        int w = 0, h = 0;
        SDL_Texture* t = spriteCache_->Get(*front_->archive, bitmap, front_->palette, &w, &h);
        if (t) { SDL_Rect d{x, y, w, h}; SDL_RenderCopy(renderer_, t, nullptr, &d); }
        if (advance) *advance = w;
    }

    void DrawText(const std::string& s, int x, int y) {
        for (char c : s) { int adv = 0; if (uint16_t g = GlyphFor(c)) DrawGlyph(g, x, y, &adv); x += adv; }
    }

    void RenderFront() {
        if (!front_) return;
        const int ox = hudX_;                                    // 4:3 frame at x = 110
        if (front_->background) {
            SDL_Rect d{ox, 0, front_->bgW, front_->bgH};
            SDL_RenderCopy(renderer_, front_->background.get(), nullptr, &d);
        }
        if (mode_ == Mode::Intro && front_->hasMovie) {
            const qtshim::DecodedFrame* f = front_->movie.AtEnd() ? nullptr : front_->movie.Frame();
            if (f) { UploadMovieFrame(*f); front_->movieShown = true; }
            if (front_->movieShown && movieTex_) {           // the last frame stays up for the fade-out
                SDL_Rect d{ox + front_->movieRect.x, front_->movieRect.y, texW_, texH_};
                SDL_RenderCopy(renderer_, movieTex_, nullptr, &d);
            }
        }
        if (mode_ == Mode::SignIn && signIn_) {
            SignInUI& ui = *signIn_;
            const mohawk::Palette& pal = front_->palette;
            auto drawCap = [&](const mohawk::SpriteDef& def, const std::string& key) {
                auto it = ui.press.find(key);
                if (it != ui.press.end() && it->second && it->second->Running())
                    it->second->Draw(renderer_, *spriteCache_, pal, ox, 0);
                else
                    mohawk::DrawStaticShape(renderer_, *spriteCache_, *front_->archive, def, 1, pal, ox, 0);
            };
            for (const auto& [def, key] : ui.keys) drawCap(def, key);
            if (ui.go) drawCap(*ui.go, "Go");
            if (ui.blink) ui.blink->Draw(renderer_, *spriteCache_, pal, ox, 0);
            const bool demo = ui.prompt && ui.prompt->Running();     // Angelica is typing her name
            if (demo) ui.prompt->Draw(renderer_, *spriteCache_, pal, ox, 0);
            const SDL_Rect line = ui.rects.count("LineEditor") ? ui.rects["LineEditor"] : SDL_Rect{237, 38, 191, 45};
            const int tx = ox + line.x + 4, ty = line.y + 2;
            DrawText(ui.name, tx, ty);
            if (!demo && ui.cursorBitmap && int(front_->clock * 2) % 2 == 0)     // blink at 1 Hz
                DrawGlyph(ui.cursorBitmap, tx + TextWidth(ui.name), ty, nullptr);
            if (ui.rects.count("ListDisplay")) {
                const SDL_Rect& L = ui.rects["ListDisplay"];
                for (size_t i = 0; i < ui.players.size() && i < kListRows; ++i)
                    DrawText(ui.players[i], ox + L.x + 4, L.y + 4 + int(i) * kListRowH);
            }
        }
    }

    // Pillarbox bars stay black; the fade covers the whole canvas.
    // PROGRAM\MOHAWK.W32, the player's settings file the installer puts beside OUTLINE.
    // [Audio.WaveRateTranslations] maps the Mac recording rates to PC rates (11127=11025,
    // 22254=22050): those sounds play at the PC rate, as in the original player.
    void LoadPlayerSettings() {
        namespace fs = std::filesystem;
        auto Lower = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; };
        std::error_code ec;
        std::string path;
        for (auto& e : fs::directory_iterator(fs::path(opt_.data) / "PROGRAM", ec))
            if (Lower(e.path().filename().string()) == "mohawk.w32") path = e.path().string();
        std::ifstream in(path);
        std::string line, section;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == ';') continue;
            if (line[0] == '[') { section = Lower(line); continue; }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (section == "[audio.waveratetranslations]") {
                const int from = std::atoi(line.substr(0, eq).c_str()), to = std::atoi(line.substr(eq + 1).c_str());
                if (from > 0 && to > 0 && audio_) { audio_->SetRateTranslation(from, to); std::printf("player settings: sounds at %d Hz play at %d Hz\n", from, to); }
            }
        }
    }

    // ---- the player's running-man busy cursor: shown while a page fades or loads (and when
    // a script asks with waitcursor), on top of everything, hotspot at the pointer.
    SDL_Texture* busyTex_[port::kBusyFrameCount] = {};
    uint32_t busyMs_ = 0;                            // time the busy cursor has been running
    bool Busy() const {
        // also while the game itself fades the palette with its cursor hidden (episode / floor changes)
        return mode_ == Mode::Engine && (Transitioning() || fade_ > 0 ||
               (lb_ && (lb_->waitCursor_ || (!lb_->cursorShown_ && lb_->PaletteFading()))));
    }
    void DrawBusyCursor() {
        if (!Busy() || !mouseIn_) return;
        const int k = int(busyMs_ / port::kBusyFrameMs) % port::kBusyFrameCount;
        if (!busyTex_[k]) {
            uint32_t px[16 * 16];
            const uint8_t* c = port::kBusyFrames[k];
            for (int y = 0; y < 16; ++y) {
                const int img = c[2 * y] << 8 | c[2 * y + 1], mask = c[32 + 2 * y] << 8 | c[33 + 2 * y];
                for (int x = 0; x < 16; ++x) {
                    const int bit = 0x8000 >> x;
                    px[y * 16 + x] = (img & bit) ? 0xff000000u : (mask & bit) ? 0xffffffffu : 0u;
                }
            }
            busyTex_[k] = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 16, 16);
            if (!busyTex_[k]) return;
            SDL_UpdateTexture(busyTex_[k], nullptr, px, 16 * 4);
            SDL_SetTextureBlendMode(busyTex_[k], SDL_BLENDMODE_BLEND);
        }
        const int hv = port::kBusyFrames[k][64] << 8 | port::kBusyFrames[k][65], hh = port::kBusyFrames[k][66] << 8 | port::kBusyFrames[k][67];
        const SDL_Rect d{mouseX_ - hh, mouseY_ - hv, 16, 16};
        SDL_RenderCopy(renderer_, busyTex_[k], nullptr, &d);
    }

    void DrawFade() {
        if (fade_ <= 0) return;
        // the fade's level at this very moment, not at the last 60 Hz tick: it changes on
        // every frame of the display
        double f = fade_;
        if (transNext_ && transDur_ > 0) f = std::min(1.0, (transT_ + tickFrac_) / transDur_);
        else if (fadeInDur_ > 0) f = std::max(0.0, fade_ - tickFrac_ / fadeInDur_);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, Uint8(std::lround(std::clamp(f, 0.0, 1.0) * 255)));
        SDL_RenderFillRect(renderer_, nullptr);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    }

    void UploadMovieFrame(const qtshim::DecodedFrame& f) {
        if (!movieTex_ || texW_ != f.width || texH_ != f.height) {
            if (movieTex_) SDL_DestroyTexture(movieTex_);
            movieTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, f.width, f.height);
            texW_ = f.width; texH_ = f.height; uploadedPts_ = -1;
        }
        if (f.pts600 != uploadedPts_ || lastUploadFrom_ != &f) {
            SDL_UpdateTexture(movieTex_, nullptr, f.bgra, f.stride);
            uploadedPts_ = f.pts600;
            lastUploadFrom_ = &f;
        }
    }

    void PumpEvents() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running_ = false;
            if (MenuEvent(e)) continue;
            // pointer position and presence, for every mode (the busy cursor draws at it)
            if (e.type == SDL_MOUSEMOTION) { mouseX_ = e.motion.x; mouseY_ = e.motion.y; mouseIn_ = true; }
            if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)) { mouseX_ = e.button.x; mouseY_ = e.button.y; mouseIn_ = true; }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_LEAVE) mouseIn_ = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_ENTER) mouseIn_ = true;
            if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_MINIMIZED)) SetFrozen(true);
            // Alt+Tab (or any other window) out of exclusive full screen: minimize to the taskbar,
            // which also gives the desktop its own display back; clicking the taskbar button
            // restores the game full screen. Borderless and windowed stay where they are.
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && window_ && opt_.headlessFrames <= 0
                && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN)
                SDL_MinimizeWindow(window_);
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) SetFrozen(false);
            if (frozen_) continue;                                       // no input reaches a frozen game
            if (mode_ == Mode::Engine) { EngineEvent(e); continue; }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && !Transitioning())
                Click(e.button.x, e.button.y);                          // logical coords (SDL scales)
            if (e.type == SDL_TEXTINPUT && mode_ == Mode::SignIn && signIn_)
                for (const char* c = e.text.text; *c; ++c) SignInChar(*c);
            if (e.type != SDL_KEYDOWN || e.key.repeat) continue;
            // Alt+Enter is Stop Movie: on the sign-in screen (no movie) it doesn't sign in
            if (mode_ == Mode::SignIn && (e.key.keysym.mod & KMOD_ALT) &&
                (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) continue;
            if (mode_ != Mode::Play && FrontKey(e.key.keysym.sym)) continue;
            if (mode_ != Mode::Play && e.key.keysym.sym != SDLK_ESCAPE && e.key.keysym.sym != SDLK_F11) continue;
            if (cutscene_ && e.key.keysym.sym != SDLK_F11) {
                const SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_SPACE || k == SDLK_RETURN || k == SDLK_KP_ENTER) EndCutscene();
                continue;
            }
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:                                     // Esc asks, like the Quit button
                if (showQuit_) { ToolSound(*quitArchive_, quitSounds_, "QuitNoSnd"); showQuit_ = false; }
                else OpenQuit();
                break;
            case SDLK_SPACE:
                if (page_->hasMovie) {
                    page_->movie.Toggle();
                    if (movieVoice_) audio_->Pause(movieVoice_, !page_->movie.Playing());
                }
                break;
            case SDLK_r: case SDLK_m:
                if (page_->hasMovie) {
                    page_->movie.Restart();
                    if (movieVoice_ && audio_->Playing(movieVoice_)) audio_->Seek(movieVoice_, 0);
                    else StartMovieAudio(page_->movie.Asset());
                }
                break;
            case SDLK_LEFTBRACKET: if (page_->node) LoadNode(nodeIndex_ > 1 ? nodeIndex_ - 1 : 20); break;
            case SDLK_RIGHTBRACKET: if (page_->node) LoadNode(nodeIndex_ < 20 ? nodeIndex_ + 1 : 1); break;
            case SDLK_p: showPalette_ = !showPalette_; break;
            case SDLK_q: showQuit_ = !showQuit_; break;
            case SDLK_o: showObjects_ = !showObjects_; break;
            case SDLK_n: LoadPage(pageIndex_ + 1); break;
            case SDLK_b: LoadPage(pageIndex_ + PageCount() - 1); break;
            default: break;
            }
        }
        SyncMenuBar();
    }

    size_t PageCount() const { return assets_.GetOutline().pages.at(opt_.section).size(); }

    void Update(double dt) {
        busyMs_ = Busy() ? busyMs_ + uint32_t(dt * 1000.0 + 0.5) : 0;   // the running man starts on frame 1
        if (DevEnv("BUSYLOG")) {
            static bool was = false;
            if (Busy() != was) { was = Busy(); std::printf("BUSY %d t=%u trans %d fade %.2f wait %d pal %d cursor %d\n", was, lbTime_, Transitioning(), fade_, lb_ ? lb_->waitCursor_ : 0, lb_ ? lb_->PaletteFading() : 0, lb_ ? lb_->cursorShown_ : 0); }
        }
        if (transNext_) {                                        // fading out to black
            transT_ += dt;
            fade_ = std::min(1.0, transT_ / transDur_);
            if (audio_) audio_->SetMaster(float(1.0 - fade_));
            if (transT_ >= transDur_) {
                auto next = std::move(transNext_);
                transNext_ = nullptr;
                transTarget_.clear();
                if (audio_) { audio_->StopAll(); audio_->SetMaster(1.0f); }
                movieVoice_ = 0;
                fade_ = 0;
                next();
                return;
            }
        } else {
            fade_ = std::max(0.0, fade_ - dt / fadeInDur_);
        }
        // a page that fades in is set up at once (its first picture complete) but only starts
        // playing when the fade is over: the quit page's goodbye scene no longer begins while
        // the screen is still coming up from black
        if (mode_ == Mode::Engine) {
            if (Transitioning()) return;
            if (fade_ > 0) {
                // A game page comes up from black as a still picture: what its scripts set up
                // at once is there (the babies in the chute), but the clock stands still until
                // it is fully up - the control room's static is frozen, Chuckie's peek from the
                // trash can and the voices (Grandpa's instructions) start after (reference
                // video). The menu pages stay still until then.
                if (!lb_) return;
                if (lbSection_ == "Control") { lb_->UpdateSetup(); return; }
                lb_->holdVoices_ = true;
                UpdateEngine(0);
                if (lb_) lb_->holdVoices_ = false;
                // a palette fade doesn't darken a movie: a cutscene starting the page shows at once
                if (lb_ && !lb_->Movies().empty()) fade_ = 0;
                return;
            }
            UpdateEngine(dt);
            return;
        }
        if (mode_ != Mode::Play) { UpdateFront(dt); return; }
        if (cutscene_) {
            cutscene_->Update(dt);
            if (cutscene_->AtEnd()) EndCutscene();                // straight cut into the game
            return;
        }
        if (!page_) return;
        if (quitDelay_ > 0 && (quitDelay_ -= dt) <= 0) running_ = false;
        const bool wantBar = !showQuit_ && ((mouseIn_ && mouseY_ >= toolbarTop_) || showToolbarButtons_);
        if (wantBar && !toolbarOpen_ && toolbarArchive_) ToolSound(*toolbarArchive_, toolSounds_, "ToolOpen");
        toolbarOpen_ = wantBar;
        for (ToolButton& b : toolButtons_) if (b.press) b.press->Update(dt);
        if (page_->hasMovie) page_->movie.Update(dt);
        if (showQuit_) for (auto& b : quitButtons_) b.Update(dt);
        if (page_->hasScripts) page_->scripts.Tick();
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        const double speed = 960.0;                            // pixels per second
        if (keys[SDL_SCANCODE_LEFT]) scrollX_ -= speed * dt;
        if (keys[SDL_SCANCODE_RIGHT]) scrollX_ += speed * dt;
        const double maxX = std::max(0, page_->bgWidth - width_);   // 860-wide camera
        if (Wraps()) {
            scrollX_ = std::fmod(scrollX_, double(page_->bgWidth));
            if (scrollX_ < 0) scrollX_ += page_->bgWidth;
        } else {
            scrollX_ = std::clamp(scrollX_, 0.0, maxX);
        }
    }

    void Render() {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (mode_ == Mode::Engine) {
            if (lb_) lb_->hideCursorItems_ = Busy();     // only the running man while fading / loading
            // outside the game's screen (the black bars in fullscreen) the system pointer shows
            if (opt_.headlessFrames <= 0) {
                const bool outside = mouseIn_ && OutsideFrame(mouseX_, mouseY_);
                if (outside != systemPointer_) { systemPointer_ = outside; SDL_ShowCursor(outside ? SDL_ENABLE : SDL_DISABLE); }
            }
            RenderEngine(); DrawFade(); DrawBusyCursor(); SDL_RenderPresent(renderer_); return;
        }
        if (mode_ != Mode::Play) { RenderFront(); DrawFade(); DrawCursor(); SDL_RenderPresent(renderer_); return; }
        if (cutscene_) { DrawCutscene(); SDL_RenderPresent(renderer_); return; }
        if (page_ && page_->background) DrawBackground();
        // A movie covers the scene while it runs; once it has finished, the scene shows.
        if (page_ && page_->hasMovie && !page_->movie.AtEnd()) DrawMovie();
        if (showObjects_) DrawObjects();
        DrawSprites();
        if (showPalette_ && page_ && page_->hasPalette) DrawPalette();
        DrawFade();
        DrawCursor();
        SDL_RenderPresent(renderer_);
    }

    // The scene is world-sized (2560 wide on nodes) and wraps; draw the 860-wide camera
    // window at scrollX_, in up to two pieces where it crosses the seam. Scenes narrower
    // than the camera (640-wide pages) are centred. Top-aligned: scenes and movies are
    // 640x400, leaving the bottom strip for the toolbar.
    void DrawBackground() {
        const int w = page_->bgWidth, h = std::min(page_->bgHeight, height_);
        if (!Wraps()) {
            const int x0 = std::clamp(int(scrollX_), 0, std::max(0, w - width_));
            const int cw = std::min(w, width_);
            SDL_Rect src{x0, 0, cw, h}, dst{SceneLeft(), 0, cw, h};
            SDL_RenderCopy(renderer_, page_->background.get(), &src, &dst);
            return;
        }
        const int x0 = int(scrollX_) % w;
        const int first = std::min(width_, w - x0);
        SDL_Rect src{x0, 0, first, h}, dst{0, 0, first, h};
        SDL_RenderCopy(renderer_, page_->background.get(), &src, &dst);
        if (first < width_) {
            SDL_Rect src2{0, 0, width_ - first, h}, dst2{first, 0, width_ - first, h};
            SDL_RenderCopy(renderer_, page_->background.get(), &src2, &dst2);
        }
    }

    // FMV is the base of the game: page movies play 1:1 inside the original 4:3 frame at
    // their BITL rect (640x400 above the toolbar), never stretched. Beside them the
    // widescreen bands are black, like the pillarboxed intro, so no mismatched scenery
    // shows next to the video.
    void DrawMovie() {
        const qtshim::DecodedFrame* f = page_->movie.Frame();
        if (!f) return;
        UploadMovieFrame(*f);
        const SDL_Rect& r = page_->movieRect;
        const SDL_Rect dst = r.w > 0 ? SDL_Rect{hudX_ + r.x, r.y, f->width, f->height}
                                     : SDL_Rect{hudX_ + (gameW_ - f->width) / 2, 0, f->width, f->height};
        const int bandH = std::max(toolbarTop_, dst.y + dst.h);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        const SDL_Rect frame{0, 0, width_, bandH};
        SDL_RenderFillRect(renderer_, &frame);
        SDL_RenderCopy(renderer_, movieTex_, nullptr, &dst);
    }

    void DrawPalette() {                                         // 16x16 swatches of the page tPAL
        const int cell = 12, x0 = width_ - 16 * cell - 8, y0 = 8;
        for (int i = 0; i < 256; ++i) {
            const auto& c = page_->palette.rgb[i];
            SDL_SetRenderDrawColor(renderer_, c[0], c[1], c[2], 255);
            SDL_Rect r{x0 + (i % 16) * cell, y0 + (i / 16) * cell, cell, cell};
            SDL_RenderFillRect(renderer_, &r);
        }
    }

    void Screenshot(const std::string& path) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        Render();
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (s && SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
            SDL_SaveBMP(s, path.c_str()) == 0)
            std::printf("screenshot %dx%d -> %s (movie t=%.0f/%d)\n", w, h, path.c_str(),
                        page_ && page_->hasMovie ? page_->movie.Time() : 0.0,
                        page_ && page_->hasMovie ? page_->movie.Asset().duration : 0);
        else
            std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
        if (s) SDL_FreeSurface(s);
    }

    Options opt_;
    port::AssetManager assets_;
    std::unique_ptr<PageState> page_;
    size_t pageIndex_ = 0;
    int nodeIndex_ = 1;
    double scrollX_ = 0;
    std::unique_ptr<mohawk::SpriteTextureCache> spriteCache_;
    mohawk::Archive* toolbarArchive_ = nullptr;
    mohawk::Archive* quitArchive_ = nullptr;
    std::unique_ptr<mohawk::SpriteInstance> toolbarBar_;
    std::vector<mohawk::SpriteInstance> toolbarButtons_, quitButtons_;
    std::optional<mohawk::SpriteDef> quitBackdrop_;
    bool showToolbarButtons_ = false, showQuit_ = false, showObjects_ = false;
    bool worldWrap_ = false;
    mohawk::ScriptRuntime toolbarObjects_;
    bool hasToolbarObjects_ = false;
    int toolbarTop_ = 400;
    struct ToolPatch { std::string name; uint16_t bitmap; SDL_Rect rect; };
    struct ToolButton { std::string name; SDL_Rect rect; mohawk::SpriteDef def; std::unique_ptr<mohawk::SpriteTracks> press; };
    std::vector<ToolPatch> toolPatches_;
    std::vector<ToolButton> toolButtons_;
    std::map<std::string, uint16_t> toolSounds_, quitSounds_;
    std::map<std::string, SDL_Rect> quitRects_;
    mohawk::ScriptRuntime quitObjects_;
    bool toolbarOpen_ = false;
    double quitDelay_ = 0;                          // Yes pressed: quit once its sound has played
    struct CursorArt { uint16_t bitmap; int dx, dy; };
    mohawk::Archive* cursorArchive_ = nullptr;
    std::map<std::string, CursorArt> cursors_;
    int mouseX_ = 0, mouseY_ = 0;
    bool mouseIn_ = false;                          // from the bar's ANI bounds (3504: y 400)
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* movieTex_ = nullptr;
    int texW_ = 0, texH_ = 0;
    int width_ = kCanvasW, height_ = kCanvasH;      // logical canvas (16:9)
    int gameW_ = 640, gameH_ = 480, hudX_ = 0;      // original 4:3 frame and its offset in the canvas
    int64_t uploadedPts_ = -1;
    const qtshim::DecodedFrame* lastUploadFrom_ = nullptr;
    Mode mode_ = Mode::Play;
    size_t introIndex_ = 0;
    std::unique_ptr<FrontScreen> front_;
    std::unique_ptr<SignInUI> signIn_;
    std::string player_;
    std::map<std::string, std::string> vars_;       // the player's game variables ([Globals])
    std::map<std::string, std::string> port_;       // the player's [Port] resume data
    double fade_ = 0;
    double tickFrac_ = 0;                          // real time since the last logic tick (s)
    uint8_t heldButton_ = 0;                       // the mouse button that started the current click
    std::function<void()> transNext_;               // where the running fade-out leads
    double transT_ = 0, transDur_ = 2.0;
    double fadeInDur_ = 0.4;                        // length of the current fade up from black
    std::unique_ptr<port::Audio> audio_;
    int movieVoice_ = 0;
    // engine host state
    std::unique_ptr<lb::Engine> lb_;
    std::string lbSection_;
    size_t lbIndex_ = 0;
    bool lbFade_ = false, lbLogAll_ = false;
    uint32_t lbTime_ = 0;
    int lbTypeWait_ = 0, lbFakeVoice_ = -1;
    std::map<int, uint32_t> lbVoices_;                 // voice -> game time it ends
    std::map<int, uint32_t> lbPausedLeft_;             // paused voice -> ms it still has to play
    std::function<void()> lbPending_;
    std::vector<uint8_t> lbBuf_;
    SDL_Texture* lbTex_ = nullptr;
    struct MovieTex { SDL_Texture* tex = nullptr; int w = 0, h = 0; int64_t pts = -1; };
    std::map<port::VideoPlayer*, MovieTex> lbMovieTex_;
    std::unique_ptr<port::VideoPlayer> cutscene_;   // full-screen NIS playing over the game
    SDL_Rect cutRect_{0, 0, 640, 400};
    std::mt19937 rng_{std::random_device{}()};
    static constexpr int kListRowH = 46;                 // saved-player rows in ListDisplay
    static constexpr size_t kListRows = 4;
    bool running_ = true, showPalette_ = false;
};

} // namespace

#ifdef _WIN32
// ---- crash reporting (Windows): the GUI build has no console, so the log goes to
// rugrats_log.txt in the user's folder (AppData\Rugrats Adventure Game), and a crash writes
// rugrats_crash.txt (where it happened) and rugrats_crash.dmp (a minidump) there too.
static wchar_t g_exeDir[MAX_PATH];
static std::wstring g_logDir;                       // the user folder (set once it is known)
static std::wstring ExePath(const wchar_t* name) { return (g_logDir.empty() ? std::wstring(g_exeDir) : g_logDir) + name; }
static void DescribeAddress(FILE* f, const void* addr) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod)
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    std::fprintf(f, "  %p  %ls+0x%llx\n", addr, base ? base + 1 : path,
                 static_cast<unsigned long long>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)));
}
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (FILE* f = _wfopen(ExePath(L"rugrats_crash.txt").c_str(), L"w")) {
        std::fprintf(f, "Rugrats port crash\nexception 0x%08lx at\n", ep->ExceptionRecord->ExceptionCode);
        DescribeAddress(f, ep->ExceptionRecord->ExceptionAddress);
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            std::fprintf(f, "%s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                         reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
        std::fprintf(f, "thread %lu stack:\n", GetCurrentThreadId());
        void* frames[48];
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) DescribeAddress(f, frames[i]);
        std::fclose(f);
    }
    HANDLE h = CreateFileW(ExePath(L"rugrats_crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(h);
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void InstallCrashReporting() {
    GetModuleFileNameW(nullptr, g_exeDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_exeDir, L'\\')) slash[1] = 0;
    SetUnhandledExceptionFilter(CrashFilter);
}
static void StartLog(const std::string& userDir) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, userDir.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, userDir.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w += L'\\';
    g_logDir = w;
    if (!GetConsoleWindow()) {                       // double-clicked GUI build: log to a file
        _wfreopen(ExePath(L"rugrats_log.txt").c_str(), L"w", stdout);
        _dup2(_fileno(stdout), _fileno(stderr));     // one file handle: lines don't overwrite each other
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);        // every line reaches the file, even on a crash
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}
#endif

// First run without the installer's preparation: build the movies and sounds once, with a
// small window showing how far along it is (about a minute on a modern PC).
static void PrepareWithProgressWindow(const std::string& data, const std::string& out) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_Window* w = SDL_CreateWindow("Rugrats Adventure Game - preparing movies and sounds (first run)",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 520, 90, 0);
    SDL_Renderer* r = w ? SDL_CreateRenderer(w, -1, 0) : nullptr;
    port::PrepareAssets(data, out, [&](int done, int total, const std::string&) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        if (r) {
            SDL_SetRenderDrawColor(r, 24, 24, 40, 255);
            SDL_RenderClear(r);
            SDL_Rect frame{20, 30, 480, 30}, bar{22, 32, total ? 476 * done / total : 0, 26};
            SDL_SetRenderDrawColor(r, 120, 120, 140, 255);
            SDL_RenderDrawRect(r, &frame);
            SDL_SetRenderDrawColor(r, 250, 170, 30, 255);
            SDL_RenderFillRect(r, &bar);
            SDL_RenderPresent(r);
        }
        return true;
    });
    if (r) SDL_DestroyRenderer(r);
    if (w) SDL_DestroyWindow(w);
}

// Can this run write saves into the game folder? (The exe's manifest is asInvoker, so Windows
// never redirects these writes into VirtualStore: a protected folder just fails the test.)
static bool GameFolderWritable(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path d = fs::u8path(dir), probe = d / ".rugrats_write_test";
    {
        std::FILE* f = nullptr;
#ifdef _WIN32
        f = _wfopen(probe.wstring().c_str(), L"wb");
#else
        f = std::fopen(probe.string().c_str(), "wb");
#endif
        if (!f) return false;
        std::fclose(f);
        fs::remove(probe, ec);
    }
    for (const char* sub : {"saves", "PROGRAM/PLAYFILE", "PROGRAM/GLOBALS", "settings.ini"}) {   // existing files must be writable too
        const fs::path p = d / sub;
        if (!fs::exists(p, ec)) continue;
        if (fs::is_directory(p, ec)) {
            const fs::path q = p / ".rugrats_write_test";
            std::FILE* f = nullptr;
#ifdef _WIN32
            f = _wfopen(q.wstring().c_str(), L"wb");
#else
            f = std::fopen(q.string().c_str(), "wb");
#endif
            if (!f) return false;
            std::fclose(f);
            fs::remove(q, ec);
        } else {
#ifdef _WIN32
            const DWORD attr = GetFileAttributesW(p.wstring().c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))   // copied from the CD
                SetFileAttributesW(p.wstring().c_str(), attr & ~DWORD(FILE_ATTRIBUTE_READONLY));
            std::FILE* f = _wfopen(p.wstring().c_str(), L"r+b");
#else
            std::FILE* f = std::fopen(p.string().c_str(), "r+b");
#endif
            if (!f) return false;
            std::fclose(f);
        }
    }
    return true;
}

#ifdef _WIN32
// Start this exe again with the same arguments, as administrator (UAC prompt). False when the
// user declines or it can't start; the caller then keeps the saves in AppData instead.
static bool RelaunchElevated() {
    int n = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &n);
    std::wstring params;
    for (int i = 1; args && i < n; ++i) {
        std::wstring a = args[i], q = L"\"";
        for (wchar_t c : a) { if (c == L'"') q += L"\\\""; else q += c; }
        if (!q.empty() && q.back() == L'\\') q += L'\\';
        params += q + L"\" ";
    }
    if (args) LocalFree(args);
    params += L"--elevated";
    wchar_t exe[MAX_PATH * 4];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)))) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}
#endif

int main(int argc, char** argv) {
    static_assert(sizeof(void*) == 8, "this port targets 64-bit builds");
#ifdef _WIN32
    InstallCrashReporting();
    if (DevEnv("RUGRATS_CRASHTEST")) { std::printf("crash test\n"); *static_cast<volatile int*>(nullptr) = 1; }
#endif
    
    SDL_SetMainReady();

    // --data must be the folder that CONTAINS MHK\ and PROGRAM\ (OUTLINE paths like
    // //RUGRATS/MHK/MAIN.MHK resolve against it). Default: the exe's own folder (the install
    // folder) when it holds PROGRAM\, else the current folder. Command-line options win.
    auto opt = std::make_unique<Options>();
    opt->data = ".";
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        char* base = SDL_GetBasePath();
        if (base) {
            const fs::path b = fs::u8path(base);
            if (fs::exists(b / "PROGRAM", ec) || fs::exists(b / "program", ec)) opt->data = b.u8string();
            SDL_free(base);
        }
    }
    opt->movies.clear();
    if (!ParseArgs(argc, argv, *opt)) return 2;
    if (opt->prepare) {                              // installer step: build movies\ and sounds\, then exit
        const bool ok = port::PrepareAssets(opt->data, opt->prepareOut, [](int done, int total, const std::string& a) {
            std::printf("[%3d/%3d] %s\n", done, total, a.c_str());
            return true;
        });
        return ok ? 0 : 1;
    }
    if (opt->movies.empty()) {
        // the converted movies and sounds: beside the game when the installer made them,
        // else (first run of a plain copy) built once into the user's local app data
        namespace fs = std::filesystem;
        std::string base = opt->data;
        if (!port::AssetsPrepared(base) && opt->headlessFrames == 0) {
            const char* local = std::getenv("LOCALAPPDATA");
            fs::path cache = local ? fs::u8path(local) / "Rugrats Adventure Game" / "Cache"
                                   : fs::u8path(opt->data) / "Cache";
            base = cache.u8string();
            if (!port::AssetsPrepared(base)) PrepareWithProgressWindow(opt->data, base);
        }
        opt->movies = (fs::u8path(base) / "movies").u8string();
        if (opt->sounds.empty()) opt->sounds = (fs::u8path(base) / "sounds").u8string();
    }
    if (opt->sounds.empty()) opt->sounds = (std::filesystem::u8path(opt->data) / "sounds").u8string();
    // Saves, settings and logs: the user's own folder, never the install folder (Program Files
    // is read-only without admin rights). --userdir chooses another; a file named "portable"
    // beside the game keeps them in UserData\ next to it instead; test runs use the data folder.
    if (opt->userDir.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path d = fs::u8path(opt->data);
        // Game-folder saves (--portable, or a file named "portable" beside the game): saves\,
        // settings.ini, the log and PROGRAM\PLAYFILE stay in the game folder, like the original.
        // In a protected folder (Program Files) that needs admin rights: ask once (UAC) and
        // restart elevated; if declined, fall back to the user's AppData folder.
        bool gameFolder = opt->portable || fs::exists(d / "portable", ec) || fs::exists(d / "portable.txt", ec);
        if (gameFolder && opt->headlessFrames == 0 && !GameFolderWritable(opt->data)) {
#ifdef _WIN32
            if (!opt->elevatedRelaunch && RelaunchElevated()) return 0;
#endif
            gameFolder = false;
        }
        if (opt->headlessFrames > 0 || gameFolder) opt->userDir = opt->data;
        else if (char* pref = SDL_GetPrefPath("", "Rugrats Adventure Game")) { opt->userDir = pref; SDL_free(pref); }
        else opt->userDir = opt->data;
    }
    while (opt->userDir.size() > 1 && (opt->userDir.back() == '/' || opt->userDir.back() == '\\')) opt->userDir.pop_back();
    {
        std::error_code ec;
        namespace fs = std::filesystem;
        const fs::path u = fs::u8path(opt->userDir), oldSaves = fs::u8path(opt->data) / "saves";
        // first run after an earlier build that kept saves beside the game: bring them along
        if (opt->userDir != opt->data && !fs::exists(u / "saves", ec) && fs::is_directory(oldSaves, ec)) {
            fs::create_directories(u / "saves", ec);
            for (auto& e : fs::directory_iterator(oldSaves, ec)) {
                const std::string n = e.path().filename().string();
                fs::copy_file(e.path(), (n == "settings.ini" ? u : u / "saves") / n, fs::copy_options::skip_existing, ec);
            }
            fs::copy_file(fs::u8path(opt->data) / "rugrats_players.txt", u / "rugrats_players.txt", fs::copy_options::skip_existing, ec);
        }
        fs::create_directories(u / "saves", ec);
    }
#ifdef _WIN32
    if (opt->headlessFrames == 0) StartLog(opt->userDir);
    else StartLog(opt->data);
#endif
    std::printf("data: %s\nuser folder: %s\n", opt->data.c_str(), opt->userDir.c_str());

    // Heap-allocate the Game engine instance to prevent early stack overflow exceptions
    auto game = std::make_unique<Game>(*opt);
    if (!game->Init()) {
        std::printf("Game initialization failed.\n");
        if (opt->headlessFrames == 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rugrats Adventure Game",
                "The game's files could not be opened.\n\nThe game needs the MHK and PROGRAM folders "
                "from the original Rugrats Adventure Game CD-ROM in its install folder. "
                "Please reinstall, pointing the installer at your original disc.", nullptr);
        return 1;
    }
    
    return game->Run();
}
