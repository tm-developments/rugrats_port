// lb_engine.h - Living Books v3 page engine (Mohawk Player 3.1c1, as used by the Rugrats
// Adventure Game), reimplemented natively.
//
// Model (from the original RUGRATS.EXE and the archives; the ScummVM Mohawk "livingbooks" engine documents
// most of the shared Living Books design and was used as the reference for it):
//   * A page is an archive + base id: BITL (items), BCOD (compiled script), tPAL, VRSN.
//   * Items: picture (2), hotspot (3), animation (0x40: ANI/SPR#/SHP#/SCRP), sound (0x41),
//     group (0x42), movie (0x43), palette (0x45), proxy (0x46: loads another page, named in
//     OUTLINE [Proxies]). Items live in one z-ordered list (front = topmost).
//   * Each item carries script entries: (event -> opcode on targets), conditional jumps and
//     expressions into the page's BCOD. Events: phases, mouse, rollover, started/done,
//     notify (sync) and custom events 128-255 sent by scripts ("run event N on target").
//   * Rugrats additions handled here: a wrapping 2560-wide world with a 640x480 view
//     (setworld / setvieworigin), drag-following items (the cursor), rollovers, parents
//     (items moved together), z-order changes, LBX data files (player saves), key events.
#pragma once
#include <SDL.h>
#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "assets.h"
#include "lb/lb_value.h"
#include "mohawk.h"
#include "video_player.h"
#include "video_tbmp.h"

namespace lb {

class Engine;
class Page;
class Code;

// ---- events, opcodes, item records ----------------------------------------------------
enum Event : uint16_t {
    kEvPhaseInit = 0, kEvPhaseIntro = 1, kEvMouseDown = 2, kEvStarted = 3, kEvDone = 4, kEvMouseUp = 5,
    kEvPhaseMain = 6, kEvNotified = 7, kEvDragStart = 8, kEvDragMove = 9, kEvDragEnd = 0xa,
    kEvRolloverBegin = 0xb, kEvRolloverMove = 0xc, kEvRolloverEnd = 0xd, kEvMouseUpIn = 0xe, kEvMouseUpOut = 0xf,
    kEvMouseTrackIn = 0x10, kEvMouseTrackMove = 0x11, kEvMouseTrackMoveIn = 0x12, kEvMouseTrackMoveOut = 0x13,
    kEvMouseTrackOut = 0x14, kEvFocusBegin = 0x15, kEvFocusEnd = 0x16, kEvInit = 0x17, kEvItemLoad = 0x1a,
    kEvListLoad = 0x1b, kEvPhaseCreate = 0xff
};
enum Phase : uint16_t { kPhaseInit = 0, kPhaseIntro = 1, kPhaseMain = 2, kPhaseNone = 0x7fff, kPhaseLoad = 0xfffe, kPhaseCreate = 0xffff };
enum Timing : uint16_t { kAutoNone = 0, kAutoIntro = 1, kAutoUserIdle = 2, kAutoMain = 3, kAutoSync = 4, kAutoInit = 5, kAutoCreate = 6, kAutoLoad = 7 };
enum ItemType : uint16_t { kItemPicture = 2, kItemHotspot = 3, kItemAnim = 0x40, kItemSound = 0x41, kItemGroup = 0x42,
                           kItemMovie = 0x43, kItemPalette = 0x45, kItemProxy = 0x46,
                           kItemVectorMotion = 0xfa0 };

struct ScriptEntry {
    uint16_t type = 0, event = 0, opcode = 0, param = 0;
    uint8_t condTag = 0;
    uint16_t state = 0;
    uint16_t argc = 0;
    std::vector<uint16_t> argParam, argTarget;
    uint16_t targetingType = 0;              // 0x3f3f expression, 0xffff name, 0 = ids
    std::vector<std::string> targets;
    uint16_t newUnknown = 0, newMode = 0, newPage = 0, newSubpage = 0;
    std::string newCursor;
    uint16_t matchFrom = 0, matchNotify = 0;
    uint32_t offset = 0;
    uint16_t jumpTarget = 0;
    uint16_t dataType = 0;
    std::vector<uint8_t> data;
    std::vector<std::string> conditions;
    std::vector<ScriptEntry> subs;
};

// ---- drawing -----------------------------------------------------------------------------
// An 8-bit canvas onto the world. Buffer x = world x - originX (wrapped when the world
// wraps); clip is in buffer coordinates.
struct Canvas {
    uint8_t* px = nullptr;
    int w = 0, h = 0;
    int originX = 0, originY = 0;
    SDL_Rect clip{0, 0, 0, 0};
};

struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

// ---- items -----------------------------------------------------------------------------
class Item {
public:
    Item(Engine* e, Page* p, Rect r, uint16_t type);
    virtual ~Item() = default;

    void ReadFrom(const uint8_t*& p, const uint8_t* end);           // BITL item body
    void ReadData(uint16_t type, const std::vector<uint8_t>& data, bool atLoad);
    virtual bool ReadOwnData(uint16_t type, const std::vector<uint8_t>& data) { return false; }

    virtual void DestroySelf();
    virtual void SetEnabled(bool enabled);
    virtual void SetGlobalEnabled(bool enabled);
    virtual bool Contains(Point pt);
    virtual void Update();
    virtual void Draw(Canvas&) {}
    virtual void HandleMouseDown(Point pos);
    virtual void HandleMouseUp(Point pos);
    virtual bool TogglePlaying(bool playing, bool restart = false);
    virtual void Done(bool onlyNotify);
    virtual void Init();
    virtual void Seek(uint16_t pos) {}
    virtual void SeekToTime(uint32_t ms) {}
    virtual void SetVisible(bool visible);
    virtual void SetGlobalVisible(bool visible);
    virtual void StartPhase(uint16_t phase);
    virtual void Stop();
    virtual void Notify(uint16_t data, uint16_t from);
    virtual void Load();
    virtual void Unload();
    virtual void MoveBy(int dx, int dy);
    virtual void MoveTo(int x, int y) { MoveBy(x - rect_.left, y - rect_.top); }
    virtual Rect FrameRect() const { return rect_; }    // getframerect: what is drawn now
    virtual int FrameNumber() const { return 0; }
    virtual int PositionMs() const { return 0; }        // getposition / playfrom
    virtual bool IsPlaying() const { return playing_; }
    virtual std::vector<Item*> Members() { return {}; }  // groups / proxies
    virtual void OnDestroyed() {}
    virtual void NativeEvent(uint16_t /*event*/) {}   // events a built-in item class answers itself
    void DetachPage() { page_ = nullptr; }  // the page is going away before the item

    void RunScript(uint16_t event, uint16_t data = 0, uint16_t from = 0);
    bool HasScript(uint16_t event) const { for (const ScriptEntry& s : scripts_) if (s.event == event) return true; return false; }
    int RunScriptEntry(ScriptEntry& e);
    void SetNextTime(uint32_t minMs, uint32_t maxMs);
    void CancelTimer() { nextTime_ = 0; }
    void SetNextTime(uint32_t minMs, uint32_t maxMs, uint32_t start);

    uint16_t Id() const { return itemId_; }
    const std::string& Name() const { return name_; }
    uint16_t Type() const { return type_; }
    uint16_t ResourceId() const { return resourceId_; }
    uint64_t Serial() const { return serial_; }
    const Rect& GetRect() const { return rect_; }
    Page* GetPage() const { return page_; }
    bool Loaded() const { return loaded_; }
    bool Visible() const { return visible_ && globalVisible_; }
    bool Enabled() const { return En(); }
    bool En() const;                         // enabled, not paused, not locked out by disable(all)
    int paused_ = 0;                         // paused by an item playing in control mode 2
    bool IsAmbient() const { return isAmbient_; }
    uint16_t SoundPriority() const { return soundMode_; }
    bool Dead() const { return dead_; }

    // fields scripts reach directly (setplayparams, rollover, setparent, ...)
    Rect rect_;
    Rect origRect_;
    struct Move { uint64_t stamp; int dx, dy; };
    std::vector<Move> moves_;               // this item's moves (a late-parented child catches up on them)
    uint64_t bornStamp_ = 0;                          // the BITL rect (dragbeginfrom points are relative to it)
    Point drawOffset_;                       // displacement from the BITL rect (anims draw at ANI bounds + this)
    uint16_t loopMode_ = 0, timingMode_ = kAutoNone, controlMode_ = 0, soundMode_ = 0;
    uint32_t delayMin_ = 0, delayMax_ = 0, periodMin_ = 0, periodMax_ = 0;
    Point relocPoint_;
    bool rollover_ = false, rolloverInside_ = false, doHitTest_ = true;
    bool rolloverBlocks_ = true;             // rollover(1, ..): items beneath get no rollover
    uint64_t parent_ = 0;                   // serial of the parent, or kScreenParent
    // the babies' placement shift (MAIN): applied in events 26/178/204, undone in 179
    Point babyShift_{};
    bool babyShifted_ = false;
    static constexpr uint64_t kScreenParent = ~uint64_t(0);
    std::map<std::string, Value> props_;     // setproperty / getproperty (keys lower-case)
    std::list<Item*>::iterator orderIt_;
    bool inOrder_ = false;
    bool zBack_ = false;                     // sent to the back by setzorder(-1): behind the room, not on screen

protected:
    friend class Engine;
    friend class Page;
    friend class Code;
    Engine* vm_;
    Page* page_;
    uint16_t type_;
    std::string name_;
    uint16_t resourceId_ = 0, itemId_ = 0;
    uint64_t serial_ = 0;
    bool loaded_ = false, visible_ = true, globalVisible_ = true, playing_ = false;
    bool enabled_ = false, globalEnabled_ = true, isAmbient_ = false, dead_ = false;
    uint32_t nextTime_ = 0, startTime_ = 0;
    uint16_t loops_ = 0, phase_;
    std::vector<ScriptEntry> scripts_;
    std::vector<uint32_t> loadCode_;         // 0x194 records: run once the page is built

    ScriptEntry ParseEntry(uint16_t type, const uint8_t*& p, int& size, bool sub);
    bool CheckCondition(const std::string& condition);
    void RunCommand(const std::string& command);
};

class PictureItem : public Item {
public:
    using Item::Item;
    bool ReadOwnData(uint16_t type, const std::vector<uint8_t>& data) override;
    bool Contains(Point pt) override;
    void Draw(Canvas& c) override;
    void Init() override;
    uint16_t drawMode_ = 0;
};

// One SPR# node: a SCRP op list run frame by frame.
struct AnimNode {
    struct Op { uint8_t op; std::vector<uint8_t> d; };
    std::vector<Op> ops;
    size_t cur = 0;
    uint16_t cel = 0;
    int x = 0, y = 0;
    uint32_t delay = 0;
    void Reset() { cur = 0; cel = 0; x = y = 0; delay = 0; }
};

class AnimItem : public Item {
public:
    using Item::Item;
    void Init() override;
    void SetEnabled(bool enabled) override;
    bool Contains(Point pt) override;
    void Update() override;
    void Draw(Canvas& c) override;
    bool TogglePlaying(bool playing, bool restart) override;
    bool Drawn() const { for (const AnimNode& n : nodes_) if (n.cel) return true; return false; }   // has a picture up
    void Halt();                             // stopped by a disable: back to frame 1, no done event
    void Done(bool onlyNotify) override;
    void Seek(uint16_t pos) override;
    void SeekToTime(uint32_t ms) override;
    void StartPhase(uint16_t phase) override;
    void Load() override;
    void Stop() override;
    Rect FrameRect() const override;
    int FrameNumber() const override { return int(frame_); }
    int PositionMs() const override { return int(frame_ * tempo_); }
    void OnDestroyed() override;
    void SilenceVoice();                     // a full-screen cutscene starts: its voice stops
    std::string DebugCels() const {
        std::string s = "cels";
        for (const AnimNode& n : nodes_) s += " " + std::to_string(n.cel) + "@" + std::to_string(n.x) + "," + std::to_string(n.y);
        return s + " shapes " + std::to_string(shapes_.size()) + (running_ ? " running" : "");
    }

private:
    enum NodeState { kDone, kRunning, kWaiting };
    NodeState Step(AnimNode& n, bool seeking);
    bool AnimUpdate();                      // true when the animation finished
    void AnimStart() { lastTime_ = 0; running_ = true; }
    void AnimStop();
    bool CelTransparentAt(const AnimNode& n, int x, int y) const;
    Rect bounds_, clip_;
    std::vector<uint16_t> shapes_;
    std::vector<Point> offsets_;
    std::vector<AnimNode> nodes_;
    uint32_t tempo_ = 1, lastTime_ = 0, frame_ = 0;
    bool running_ = false, animRunning_ = false, built_ = false;
    int sound_ = 0;                          // voice of the playing anim sound
    uint32_t soundWaitFrom_ = 0;             // the frames hold until that sound is heard (lip sync)
    uint16_t soundId_ = 0;
};

class SoundItem : public Item {
public:
    using Item::Item;
    void Update() override;
    bool TogglePlaying(bool playing, bool restart) override;
    void Stop() override;
    void OnDestroyed() override;
    void SetGlobalEnabled(bool enabled) override;
private:
    bool running_ = false;
    bool paused_ = false;
    bool heldStart_ = false;                 // asked to play while the page was fading in
    int voice_ = 0;
};

class GroupItem : public Item {
public:
    using Item::Item;
    bool ReadOwnData(uint16_t type, const std::vector<uint8_t>& data) override;
    void DestroySelf() override;
    void SetEnabled(bool enabled) override;
    void SetGlobalEnabled(bool enabled) override;
    bool Contains(Point) override { return false; }
    bool TogglePlaying(bool playing, bool restart) override;
    void Seek(uint16_t pos) override;
    void SetVisible(bool visible) override;
    void SetGlobalVisible(bool visible) override;
    void StartPhase(uint16_t phase) override;
    void Stop() override;
    void Load() override;
    void Unload() override;
    void MoveBy(int dx, int dy) override;
    std::vector<Item*> Members() override;
    std::vector<uint16_t> ids_;              // member item ids
    std::vector<uint64_t> serials_;          // members added by addtogroup
private:
    bool starting_ = false;
};

// The player's "VectorMotion" item (type 0xfa0, Type=VectorMotion): a point that moves under
// a velocity and an acceleration (pixels per second, per second squared) inside a bounds
// rect, carrying its children (setparent). The scripts drive it through globals:
//   event 252: take _vmBounds, _vmVx, _vmVy, _vmAx, _vmAy (for the item _vmName)
//   event 251: put the item's current _vmVx/_vmVy/_vmAx/_vmAy back
//   setplayparams(self, 0, 0, 2, "min,max"): run, with a "done" (event 4) every min..max ms
//   setplayparams(self): stop
// Leaving the bounds sends event 248 and holds it at the edge. The stair toss throws its cookie
// with one (upwards at -800 px/s/s until it reaches the target step); the alien chase flies
// Angelica with one, pulled toward a wandering point.
class VectorMotionItem : public Item {
public:
    using Item::Item;
    void Update() override;
    void NativeEvent(uint16_t event) override;
private:
    void Take();
    double x_ = 0, y_ = 0, vx_ = 0, vy_ = 0, ax_ = 0, ay_ = 0;
    Rect bounds_{};
    bool hasBounds_ = false, running_ = false, outside_ = false;
    uint32_t last_ = 0, nextTick_ = 0;
};

class MovieItem : public Item {
public:
    uint32_t syncWait_ = 0;                  // ms spent waiting for the soundtrack to start
    using Item::Item;
    void Update() override;
    bool TogglePlaying(bool playing, bool restart) override;
    void Stop() override;
    void OnDestroyed() override;
    port::VideoPlayer* Player() { return video_ ? video_.get() : nullptr; }
    int Voice() const { return voice_; }
    // a zoom between rooms that has finished: its last picture stays up (as QuickTime leaves
    // it) until the script moves it away or destroys it - no black frame before the next room
    bool Ended() const { return ended_; }
    // showchannel / hidechannel: QuickTime video tracks, 1-based; kept across plays
    void SetChannel(int channel, bool shown);
    void Load() override;
    void Unload() override;
private:
    void Close();
    void OpenVideo();
    std::map<int, bool> channels_;
    std::unique_ptr<port::VideoPlayer> video_;
    int voice_ = 0;
    uint32_t holdUntil_ = 0;                 // last picture held until then (floor changes)
    bool ended_ = false;
    static constexpr uint32_t kHoldLastMs = 400;
    uint32_t closeAt_ = 0;                   // a finished cutscene's last picture stays until then
};

class PaletteItem : public Item {
public:
    using Item::Item;
    bool ReadOwnData(uint16_t type, const std::vector<uint8_t>& data) override;
    bool TogglePlaying(bool playing, bool restart) override;
    void Update() override;
    bool Fading() const { return fading_; }
    double ApplyAt(uint32_t now);            // set the colours for that moment; returns how far (0..1)
private:
    uint16_t period_ = 0, step_ = 1, start_ = 0, count_ = 0;
    std::vector<std::array<uint8_t, 3>> colors_;
    std::vector<std::array<uint8_t, 3>> from_;
    uint32_t fadeStart_ = 0;
    bool fading_ = false;
};

class ProxyItem : public Item {
public:
    using Item::Item;
    ~ProxyItem() override;
    void Load() override;
    void Unload() override;
    std::vector<Item*> Members() override;
    Page* SubPage() { return sub_.get(); }
    void ReleaseSubPage() { sub_.reset(); }
private:
    std::unique_ptr<Page> sub_;
};

// ---- pages ---------------------------------------------------------------------------
class Page {
public:
    Page(Engine* e, mohawk::Archive* a, uint16_t baseId, Item* owner);
    ~Page();
    bool Open();                             // BITL + BCOD, items registered, init + load phase
    mohawk::Archive* Archive() const { return archive_; }
    uint16_t BaseId() const { return baseId_; }
    Code* GetCode() const { return code_.get(); }
    const std::vector<Item*>& Items() const { return items_; }
    Item* Owner() const { return owner_; }
    void Forget(Item* it);
    void Adopt(Item* it) { items_.push_back(it); }
private:
    Engine* vm_;
    mohawk::Archive* archive_;
    uint16_t baseId_;
    Item* owner_;
    std::unique_ptr<Code> code_;
    std::vector<Item*> items_;
};

// ---- host services -----------------------------------------------------------------------
struct NotifyEvent {
    enum Kind { kGotoPage, kGotoControls, kGotoQuit, kQuit, kIntroDone, kChangeMode, kShowCursor, kCursor } kind;
    int param = 0, mode = 0, page = 0, subpage = 0;
    std::string text;
};

struct Host {
    std::function<mohawk::Archive*(const std::string& outlinePath)> openArchive;
    std::function<std::string(const std::string& name)> programFile;     // PROGRAM/<name>, case-insensitive
    std::function<int(const mohawk::Archive&, uint16_t id)> playSound;   // tWAV -> voice
    std::function<int(const std::string& wav)> playWav;                  // movie soundtrack
    std::function<void(int voice)> stopSound;
    std::function<void(int voice, bool paused)> pauseSound;
    std::function<bool(int voice)> soundPlaying;
    std::function<double(int voice)> soundTime;                          // seconds played (0: not started yet, -1: ended, -2: no clock)
    std::function<std::optional<port::MovieAsset>(const mohawk::Archive&, uint16_t id)> movie;
    std::function<void(const NotifyEvent&)> notify;
    std::function<void(const std::string&)> log;
};

std::shared_ptr<LBXObject> CreateLBX(Engine* e, int type);

// ---- the engine --------------------------------------------------------------------------
class Engine {
public:
    explicit Engine(Host host);
    ~Engine();

    // Opens the main page (e.g. MAIN.MHK, base 1000) and starts its phases.
    bool OpenPage(mohawk::Archive* a, uint16_t baseId, int mode, int page, int subpage);
    void ClosePage();
    void DumpState();                        // debugging: items, phases, variables
    void Update(uint32_t nowMs);
    void UpdateSetup();               // a page fading in: setup phases only, nothing plays             // run timers, animations, queued events
    void Render(Canvas& c);                  // back to front, 8-bit
    struct MovieDraw { port::VideoPlayer* player; Rect rect; };
    std::vector<MovieDraw> Movies(bool withEnded = false) const;   // playing movies, front-most last (withEnded: and finished zooms still on screen)
    const mohawk::Palette& CurrentPalette() const { return palette_; }

    // input (view coordinates: 0..640 x 0..480 in the original frame)
    void MouseMove(int vx, int vy);
    void MouseDown(int vx, int vy);
    void MouseUp(int vx, int vy);
    void Key(int code, int mods, int ch);    // code: Windows virtual key, ch: character (0 if none)

    // ---- services for items and code ----
    uint32_t Now() const { return now_; }
    uint64_t moveClock_ = 0;                 // orders item creation and moves
    bool holdVoices_ = false;                // page fading in: animations hold their voices
    bool rolloverDirty_ = false;             // something was switched on: recheck rollovers
    void RefreshRollover();
    void RolloverPass(bool moved);
    void TrackFocus();
    void SmoothPaletteFades(uint32_t extraMs);   // palette fades drawn at the display's moment
    std::mt19937& Rng() { return rng_; }
    Value& Var(const std::string& name);     // case-insensitive, created on use
    bool HasVar(const std::string& name) const;
    void DeleteVar(const std::string& name);
    void SetVar(const std::string& name, const Value& v) { Var(name) = v; }
    Item* Resolve(const Value& v);           // item by ptr / name / id
    Item* ItemById(uint16_t id);
    Item* ItemByName(const std::string& name);
    Item* ItemBySerial(uint64_t serial);
    Value Ref(Item* it) const { return it ? Value::Item(it->Serial()) : Value(0); }
    std::string Str(const Value& v);         // string form, items as their names
    bool Equal(const Value& a, const Value& b);
    void Register(Item* it);
    void AddToOrder(Item* it);               // on top
    void RemoveFromOrder(Item* it);
    void SetZOrder(Item* it, int where);     // 0 = front, -1 = back
    void SetZBehind(Item* it, Item* ref);    // directly behind ref (ref stays in front)
    Item* ZPrev(Item* it);
    void Destroy(Item* it);                  // deferred
    Item* Clone(Item* src, uint16_t newId, const std::string& newName);
    void QueueDone(Item* it);
    // an animation finishing inside the update pass (see collectAnimDone_)
    bool DeferAnimDone(Item* it) { if (!collectAnimDone_) return false; animDone_.push_back(it); return true; }
    void EndRollover(Item* it);
    void RebeginRollovers();
    std::map<uint64_t, uint32_t> pendingRollEnd_;   // zone rollover ends waiting (gap crossing)
    void FlushRolloverEnds(bool all);              // pointer leaves the item (it goes away / stops tracking)
    void NotifyAll(uint16_t data, uint16_t from);
    void SetEnableForAll(bool enable, Item* except);
    int PlaySound(Item* source, uint16_t resourceId, const mohawk::Archive* a);
    void StopSound(int voice);
    void PauseSound(int voice, bool paused);
    bool SoundPlaying(int voice) const;
    double SoundTime(int voice) const;
    void LockSound(Item* owner, bool lock);
    std::optional<port::MovieAsset> MovieAsset(const mohawk::Archive& a, uint16_t id);
    int PlayWav(const std::string& wav) { return host_.playWav ? host_.playWav(wav) : 0; }
    const Bitmap* GetBitmap(const mohawk::Archive* preferred, uint16_t id);
    std::vector<uint8_t> GetResource(const mohawk::Archive* preferred, uint32_t tag, uint16_t id, const mohawk::Archive** found = nullptr);
    void SetPalette(int first, const std::vector<std::array<uint8_t, 3>>& colors);
    void SetPageFade(bool on) { pageFade_ = on; }
    // setpagefade(0) before a goto: the next page change cuts instead of fading
    bool TakePageFade() { const bool f = pageFade_; pageFade_ = true; return f; }
    bool pageFade_ = true;
    uint32_t movieEndAt_ = 0xffffffffu;               // game time a movie last finished
    // a movie is playing, or one just finished (its last picture is still up)
    bool MovieOnScreen() const { return !Movies(false).empty() || (movieEndAt_ != 0xffffffffu && Now() - movieEndAt_ <= 400); }
    std::unique_ptr<Page> LoadProxyPage(const std::string& name, Item* owner);
    void PostNotify(const NotifyEvent& n) { notifies_.push_back(n); }
    void Log(const std::string& s) { if (host_.log) host_.log(s); }
    std::string ProgramFile(const std::string& n) { return host_.programFile ? host_.programFile(n) : n; }
    Item* Focus() const { return focus_; }
    void SetFocus(Item* it) { focus_ = it; focusInside_ = it != nullptr; }
    bool focusInside_ = false;           // pointer inside the pressed item (track in/out)
    Point MouseWorld() const;
    uint16_t CurrentPhase() const { return phase_; }
    Page* MainPage() const { return page_.get(); }
    int PageNumber() const { return curPage_; }

    // world / view (setworld, setvieworigin)
    Rect world_{0, 0, 640, 480};
    bool wrap_ = false;
    Point view_;
    Point WrapPoint(Point p) const;
    void SetViewOrigin(Point p);
    void NotifyUnload(const std::vector<Item*>& pageItems);   // 0xFFF7 to a page about to unload
    // the player's menu items (101 skip, 102 replay, 103 quit) that scripts switch on/off
    void SetMenuItemEnabled(int id, bool on) { menuDisabled_[id] = !on; }
    bool MenuItemEnabled(int id) const { auto f = menuDisabled_.find(id); return f == menuDisabled_.end() || !f->second; }
    std::map<int, bool> menuDisabled_{{102, true}};   // Go To Replay: off until the game (MAIN) turns it on
    Point Near(const Item* it, Point w) const;
    void InsertInFront(const std::vector<Item*>& items, Item* ref);

    // drag (dragbeginfrom / setdragparams)
    void BeginDrag(Item* it, Point from);
    void EndDrag(Item* it);
    // setdragparams: with a reference point the item's top-left stays in the rect (the held
    // tools give the range themselves); with just a rect the whole item stays inside it (Lil in
    // the cookie game: her rect exactly fills the limit's height, so she only moves sideways)
    struct DragLimit { Rect r; bool whole; };
    std::map<uint64_t, DragLimit> dragLimits_;
    void ApplyDragLimit(Item* it);
    struct Drag { uint64_t serial; Point last, grab; };   // grab point (world, unclamped) and offset in the item
    std::vector<Drag> drags_;
    void DragFollow(Item* d, Drag& g);
    void FollowPointer(bool moved);
    bool IsDragged(const Item* it) const;

    // groups by name (addtogroup)
    GroupItem* GroupByName(const std::string& name, bool create);

    // keys (setkeyevent)
    struct KeyEvent { int key; int mods; uint64_t item; uint16_t event; };
    std::vector<KeyEvent> keyEvents_;

    // autoevent: an event run repeatedly on an item
    struct AutoEvent {
        uint64_t item = 0; uint16_t event = 0, loops = 1, timing = 0;
        uint32_t delayMin = 0, delayMax = 0;
        Point sync;                          // timing 4: (sender or 0 = any, notify data)
        uint16_t left = 0; uint32_t due = 0; // runs left in this round, next run (0 = idle)
    };
    std::vector<AutoEvent> autoEvents_;
    void AddAutoEvent(AutoEvent a);
    void AutoEventNotify(Item* it, uint16_t data, uint16_t from);
    void ArmAutoEvent(AutoEvent& a);

    // sync timers (sendsync, autoevent)
    struct Timer { uint32_t due; uint64_t item; uint16_t data; uint16_t event; bool notify; };
    std::vector<Timer> timers_;

    // disable(0) / enable(0): "all items" (the quit prompt switches everything off, then
    // switches its own buttons on). Items that existed at the lock and weren't switched on
    // again since stay inactive until enable(0).
    bool localDisabled_ = false;
    uint64_t lockSerial_ = 0;
    std::set<uint64_t> unlocked_;
    void LockAll(bool lock) {
        localDisabled_ = lock; lockSerial_ = nextSerial_; unlocked_.clear();
        if (lock) for (Item* it : std::vector<Item*>(created_)) if (!it->Dead() && LockedOut(it)) EndRollover(it);
    }
    bool LockedOut(const Item* it) const { return localDisabled_ && it->Serial() < lockSerial_ && !unlocked_.count(it->Serial()); }
    std::vector<Item*> AllItems() const { return created_; }
    bool PaletteFading() const;              // a fade to black is running (before a cut scene)
    std::vector<std::pair<const Item*, uint16_t>> runStack_;   // items' events being run, innermost last
    int smoothDx_ = 0;                       // host: room drawn this far from view_.x (smooth turns)
    bool OnScreenLayer(const Item* it) const;   // fixed on the screen (doesn't take part in smooth turns)
    bool cursorShown_ = true;
    std::string cursorHider_;               // the item whose script last hid the cursor
    bool waitCursor_ = false;                // waitcursor(): the player's busy cursor
    bool hideCursorItems_ = false;           // set by the host while the busy cursor shows
    void SetOutline(const port::Outline* o) { outline_ = o; }
    bool IntroDone() const { return introDone_; }
    // for the headless explorer / tests
    const std::vector<Item*>& Created() const { return created_; }
    Item* ItemAt(Point world) {                    // what a click would hit, ignoring the cursor
        Item* it = nullptr;
        for (Item* x : order_) if (!x->Dead() && !IsDragged(x) && x->Contains(Near(x, world))) { it = x; break; }
        return it;
    }

private:
    friend class Item;
    void RunPhases();
    void HandleNotify(const NotifyEvent& n);
    Item* HitTest(Point world);

    Host host_;
    std::unique_ptr<Page> page_;
    int curMode_ = 0, curPage_ = 0, curSubpage_ = 0;
    uint16_t phase_ = kPhaseInit;
    int phaseStep_ = 0;
    Point pendingView_{}; bool hasPendingView_ = false;
    const port::Outline* outline_ = nullptr;
    bool introDone_ = false;
    uint32_t now_ = 0;
    std::mt19937 rng_{std::random_device{}()};
    std::map<std::string, Value> vars_;
    std::map<uint64_t, std::unique_ptr<Item>> items_;
    std::vector<Item*> created_;             // creation order (updates, phases)
    std::list<Item*> order_;                 // front = topmost
    std::vector<Item*> pendingDestroy_;
    std::vector<Item*> pendingDone_;
    // animations that finish during one update pass: their done events run after the pass,
    // the most recently loaded first (when Tommy and a baby finish a hand-over on the same
    // tick, the baby's - which gives the item - goes before Tommy's, as in the original)
    bool collectAnimDone_ = false;
    std::vector<Item*> animDone_;
    std::vector<NotifyEvent> notifies_;
    uint64_t nextSerial_ = 1;
    Item* focus_ = nullptr;
    mohawk::Palette palette_{};
    std::map<std::pair<const mohawk::Archive*, uint16_t>, Bitmap> bitmaps_;
    std::vector<mohawk::Archive*> archives_; // open page archives, newest last
    // drag

    Point mouse_;                            // view coordinates
    // sound priority (LBItem::playSound rules)
    uint16_t lastSoundOwner_ = 0, lastSoundPriority_ = 0, soundLockOwner_ = 0, maxSoundPriority_ = 0;
    int lastSoundVoice_ = 0;
    std::map<std::string, GroupItem*> namedGroups_;
    std::vector<std::unique_ptr<Item>> graveyard_;
    std::vector<std::unique_ptr<Page>> pageGraveyard_;   // unloaded proxies' pages, freed next update
public:
    void RetirePage(std::unique_ptr<Page> p) { if (p) pageGraveyard_.push_back(std::move(p)); }
    bool Owns(const Item* it) const;
    bool CreditsRolling() const;              // the end credits after the final ending are playing          // still allocated (live, or dead in the graveyard)
private:
};

} // namespace lb
