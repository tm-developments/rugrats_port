// lb_items.cpp - Living Books items and their script entries.
#include "devenv.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "lb/lb_code.h"
#include "lb/lb_engine.h"

namespace lb {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
int16_t S16(const uint8_t* p) { return int16_t(BE16(p)); }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
std::string CStr(const uint8_t*& p, const uint8_t* end) {
    const uint8_t* s = p;
    while (p < end && *p) ++p;
    std::string r(reinterpret_cast<const char*>(s), size_t(p - s));
    if (p < end) ++p;
    return r;
}
std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }

// Blit an 8-bit bitmap into the canvas at world (wx, wy); index 0 is transparent.
void Blit(Canvas& c, const Bitmap& b, int wx, int wy, const Engine* vm, const SDL_Rect* worldClip = nullptr) {
    if (!b.w || !b.h) return;
    const int ww = vm->wrap_ ? vm->world_.Width() : 0;
    int bx0 = wx - c.originX;
    const int by = wy - c.originY;
    std::vector<int> xs{bx0};
    if (ww > 0) {
        // bring the copy nearest the canvas, then add neighbours that overlap it
        while (bx0 + b.w <= 0) bx0 += ww;
        while (bx0 >= c.w) bx0 -= ww;
        xs = {bx0};
        if (bx0 - ww + b.w > 0) xs.push_back(bx0 - ww);
        if (bx0 + ww < c.w) xs.push_back(bx0 + ww);
    }
    for (int bx : xs) {
        int x0 = std::max({bx, c.clip.x, 0}), x1 = std::min({bx + b.w, c.clip.x + c.clip.w, c.w});
        int y0 = std::max({by, c.clip.y, 0}), y1 = std::min({by + b.h, c.clip.y + c.clip.h, c.h});
        if (worldClip) {
            // worldClip in world coords, relative to this copy
            const int cx0 = worldClip->x - c.originX + (bx - (wx - c.originX));
            x0 = std::max(x0, cx0); x1 = std::min(x1, cx0 + worldClip->w);
            y0 = std::max(y0, worldClip->y - c.originY); y1 = std::min(y1, worldClip->y - c.originY + worldClip->h);
        }
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = b.px.data() + size_t(y - by) * size_t(b.w) + size_t(x0 - bx);
            uint8_t* d = c.px + size_t(y) * size_t(c.w) + size_t(x0);
            for (int x = x0; x < x1; ++x, ++s, ++d) if (*s) *d = *s;
        }
    }
}
bool OpaqueAt(const Bitmap* b, int x, int y) {
    return b && x >= 0 && y >= 0 && x < b->w && y < b->h && b->px[size_t(y) * size_t(b->w) + size_t(x)] != 0;
}
} // namespace

// ---- base item ---------------------------------------------------------------------------
Item::Item(Engine* e, Page* p, Rect r, uint16_t type) : rect_(r), origRect_(r), vm_(e), page_(p), type_(type), phase_(kPhaseLoad) { bornStamp_ = ++e->moveClock_; }

void Item::ReadFrom(const uint8_t*& p, const uint8_t* end) {
    resourceId_ = BE16(p);
    itemId_ = BE16(p + 2);
    const uint16_t size = BE16(p + 4);
    p += 6;
    name_ = CStr(p, end);
    const uint8_t* bodyEnd = std::min(end, p + size);
    while (p + 4 <= bodyEnd) {
        const uint16_t t = BE16(p), n = BE16(p + 2);
        p += 4;
        const uint8_t* q = std::min(bodyEnd, p + n);
        std::vector<uint8_t> data(p, q);
        p = q;
        ReadData(t, data, true);
    }
    p = bodyEnd;
}

void Item::ReadData(uint16_t t, const std::vector<uint8_t>& d, bool atLoad) {
    if (ReadOwnData(t, d)) return;
    switch (t) {
    case 0x66: case 0x67: {
        const uint8_t* p = d.data();
        int size = int(d.size());
        if (size >= 6) scripts_.push_back(ParseEntry(t, p, size, false));
        break;
    }
    case 0x68:                                                 // play info
        if (d.size() >= 20) {
            loopMode_ = BE16(&d[0]); delayMin_ = BE16(&d[2]); delayMax_ = BE16(&d[4]); timingMode_ = BE16(&d[6]);
            periodMin_ = BE16(&d[8]); periodMax_ = BE16(&d[10]); relocPoint_ = Point{S16(&d[12]), S16(&d[14])};
            controlMode_ = BE16(&d[16]); soundMode_ = BE16(&d[18]);
        }
        break;
    case 0x6e: if (d.size() >= 2) phase_ = BE16(&d[0]); break;           // load phase
    case 0x70: {                                                         // command text
        const uint8_t* p = d.data();
        std::string cmd = CStr(p, d.data() + d.size());
        RunCommand(cmd);
        break;
    }
    case 0x73: enabled_ = false; break;
    case 0x74: enabled_ = true; break;
    case 0x75: if (atLoad) visible_ = false; else SetVisible(false); break;
    case 0x76: if (atLoad) visible_ = true; else SetVisible(true); break;
    case 0x77: if (atLoad) globalEnabled_ = false; else SetGlobalEnabled(false); break;
    case 0x78: if (atLoad) globalEnabled_ = true; else SetGlobalEnabled(true); break;
    case 0x79: if (atLoad) globalVisible_ = false; else SetGlobalVisible(false); break;
    case 0x7a: if (atLoad) globalVisible_ = true; else SetGlobalVisible(true); break;
    case 0x7b: isAmbient_ = true; break;
    case 0x80: if (d.size() >= 2) doHitTest_ = BE16(&d[0]) != 0; break;
    case 0x194:                                                          // code to run on load
        if (d.size() >= 4) {
            if (atLoad) loadCode_.push_back(BE32(&d[0]));
            else if (page_ && page_->GetCode()) page_->GetCode()->Run(this, BE32(&d[0]));
        }
        break;
    default: break;                                                      // 0x6f 0x7c 0x7d 0x7e 0x7f: not needed
    }
}

ScriptEntry Item::ParseEntry(uint16_t type, const uint8_t*& p, int& size, bool sub) {
    ScriptEntry e;
    e.type = type;
    int expectedEnd = 0;
    if (sub) {
        expectedEnd = size - (BE16(p) + 2);
        p += 2;
        e.event = 0xffff;
    } else {
        e.event = BE16(p);
        p += 2;
    }
    e.opcode = BE16(p); e.param = BE16(p + 2);
    p += 4;
    size -= 6;
    e.condTag = uint8_t((e.event >> 8) & 0xff);
    if (!sub) e.event &= 0xff;
    else e.event = 0xffff;
    auto need = [&](int n) { return size >= n; };

    if (type == 0x66 && e.opcode == 0xfffe) {
        for (uint16_t i = 0; i < e.param && size > 0; ++i) {
            e.subs.push_back(ParseEntry(type, p, size, true));
            if (i + 1 < e.param && (size % 2) == 1) { ++p; --size; }
        }
    } else if (type == 0x66) {
        if (!need(2)) return e;
        e.argc = BE16(p); p += 2; size -= 2;
        const uint16_t tt = e.argc;
        if (tt == 0x3f3f || tt == 0xfffe || tt == 0xffff) {
            e.targetingType = tt;
            const uint16_t count = need(2) ? BE16(p) : 0;
            p += 2; size -= 2;
            const int align = size % 2;
            for (uint16_t i = 0; i < count && size > 0; ++i) {
                const uint8_t* s = p;
                std::string t = CStr(p, p + size);
                size -= int(p - s);
                e.targets.push_back(t);
            }
            e.argc = uint16_t(e.targets.size());
            if (size % 2 != align) { ++p; --size; }
        } else if (e.argc) {
            for (uint16_t i = 0; i < e.argc && need(4); ++i) {
                e.argParam.push_back(BE16(p)); e.argTarget.push_back(BE16(p + 2));
                p += 4; size -= 4;
            }
        }
    }
    if (type == 0x66 && e.opcode == 0xfffb && need(6)) { e.offset = BE32(p); e.jumpTarget = BE16(p + 4); p += 6; size -= 6; }
    if (type == 0x66 && e.opcode == 0xfffd && need(4)) { e.offset = BE32(p); p += 4; size -= 4; }
    if (type == 0x67 && e.opcode == 6) {
        if (e.param == 1 && need(8)) { e.newUnknown = BE16(p); e.newMode = BE16(p + 2); e.newPage = BE16(p + 4); e.newSubpage = BE16(p + 6); p += 8; size -= 8; }
        else if (e.param == 3) { const uint8_t* s = p; e.newCursor = CStr(p, p + size); size -= int(p - s); }
    }
    if (e.opcode == 0x1d && need(4)) { e.offset = BE32(p); p += 4; size -= 4; }
    if (e.opcode == 0xffff && need(4)) {
        e.dataType = BE16(p);
        uint16_t len = BE16(p + 2);
        p += 4; size -= 4;
        len = uint16_t(std::min<int>(len, size));
        e.data.assign(p, p + len);
        p += len; size -= len;
    }
    if (e.event == kEvNotified && !sub && need(4)) { e.matchFrom = BE16(p); e.matchNotify = BE16(p + 2); p += 4; size -= 4; }
    if (sub) {
        // subentries are padded to their declared length
        if (size > expectedEnd) { p += size - expectedEnd; size = expectedEnd; }
        return e;
    }
    if (e.condTag == 1 && size > 0) {
        const uint8_t* s = p;
        std::string cond = CStr(p, p + size);
        size -= int(p - s);
        if (cond.empty() && size > 0) { s = p; cond = CStr(p, p + size); size -= int(p - s); }
        e.conditions.push_back(cond);
    } else if (e.condTag == 2 && need(4)) {
        p += 4; size -= 4;
    }
    if (size > 0) { p += size; size = 0; }
    return e;
}

void Item::DestroySelf() {
    if (dead_) return;
    vm_->Destroy(this);
}

bool Item::En() const { return enabled_ && globalEnabled_ && paused_ == 0 && !vm_->LockedOut(this); }

void Item::SetEnabled(bool enabled) {
    if (enabled && vm_->localDisabled_) vm_->unlocked_.insert(serial_);
    // a switched-off item stops tracking the pointer at once (the toolbar folds away when
    // Quit or Cry switches it off, instead of staying up until the mouse moves)
    if (!enabled) vm_->EndRollover(this);
    if (DevEnv("LBDEBUG2") && name_ == DevEnv("LBDEBUG2")) { char b[96]; std::snprintf(b, sizeof b, "%s SetEnabled(%d)", name_.c_str(), enabled); vm_->Log(b); }
    if (enabled && !loaded_ && !playing_ && timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
    if (enabled && !enabled_ && rollover_) vm_->rolloverDirty_ = true;
    enabled_ = enabled;
}

void Item::SetGlobalEnabled(bool enabled) {
    // Local and global enable are separate flags; an item is active when both are on.
    const bool was = loaded_ && En();
    if (!enabled) vm_->EndRollover(this);
    if (enabled && !globalEnabled_ && rollover_) vm_->rolloverDirty_ = true;
    globalEnabled_ = enabled;
    if (!was && loaded_ && En() && !playing_ && timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
}

bool Item::Contains(Point pt) {
    if (!loaded_) return false;
    if (playing_ && loopMode_ == 0xffff) Stop();
    if (!playing_ && timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
    if (!(visible_ && globalVisible_)) return false;
    if (rect_.Contains(pt)) return true;
    if (vm_->wrap_ && vm_->world_.Width() > 0) {                // world wraps: test the neighbour copies
        const int ww = vm_->world_.Width();
        return rect_.Contains(Point{pt.x + ww, pt.y}) || rect_.Contains(Point{pt.x - ww, pt.y});
    }
    return false;
}

void Item::Update() {
    // A disabled item's automatic start waits, whatever its load phase: the babies' home
    // pictures (load phase none) are switched off while one of their idle animations plays,
    // so the next idle animation can't start on top of it (two Chuckies at once).
    if (!En()) return;
    if (phase_ != kPhaseNone && !loaded_) return;
    if (nextTime_ == 0 || nextTime_ > vm_->Now()) return;
    if (TogglePlaying(playing_, true)) nextTime_ = 0;
    else if (loops_ == 0 && timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
}

void Item::HandleMouseDown(Point) {
    if (!loaded_ || !En()) return;
    vm_->SetFocus(this);
    RunScript(kEvMouseDown);
    RunScript(kEvMouseTrackIn);
}

void Item::HandleMouseUp(Point pos) {
    vm_->SetFocus(nullptr);
    RunScript(kEvMouseUp);
    RunScript(Contains(pos) ? kEvMouseUpIn : kEvMouseUpOut);
}

bool Item::TogglePlaying(bool playing, bool restart) {
    if (playing) {
        vm_->QueueDone(this);
        return true;
    }
    if (((loaded_ && En()) || phase_ == kPhaseNone) && !playing_) {
        playing_ = TogglePlaying(true, restart);
        if (playing_) {
            nextTime_ = 0;
            startTime_ = vm_->Now();
            loops_ = (loopMode_ == 0xffff || loopMode_ == 0xfffe) ? 0xffff : loopMode_;
            if (controlMode_ >= 1) {
                vm_->LockSound(this, true);
                if (controlMode_ >= 2) vm_->SetEnableForAll(false, this);
            }
            RunScript(kEvStarted);
            Notify(0, itemId_);
        }
    }
    return playing_;
}

void Item::Done(bool onlyNotify) {
    if (onlyNotify) {
        if (relocPoint_.x || relocPoint_.y) MoveBy(relocPoint_.x, relocPoint_.y);
        if (loops_ && --loops_) SetNextTime(delayMin_, delayMax_, startTime_);
        else Done(false);
        return;
    }
    playing_ = false;
    loops_ = 0;
    startTime_ = 0;
    if (controlMode_ >= 1) {
        vm_->LockSound(this, false);
        if (controlMode_ >= 2) vm_->SetEnableForAll(true, this);
    }
    if (timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
    RunScript(kEvDone);
    Notify(0xffff, itemId_);
}

void Item::Init() { RunScript(kEvInit); }

void Item::SetVisible(bool v) { visible_ = v; }
void Item::SetGlobalVisible(bool v) { globalVisible_ = v; }

void Item::StartPhase(uint16_t phase) {
    if (phase_ == phase) {
        if (phase_ != kPhaseNone) SetEnabled(true);
        Load();
    }
    switch (phase) {
    case kPhaseLoad: RunScript(kEvListLoad); break;
    case kPhaseCreate:
        RunScript(kEvPhaseCreate);
        if (timingMode_ == kAutoCreate) SetNextTime(periodMin_, periodMax_);
        break;
    case kPhaseInit:
        RunScript(kEvPhaseInit);
        if (timingMode_ == kAutoInit) SetNextTime(periodMin_, periodMax_);
        break;
    case kPhaseIntro:
        RunScript(kEvPhaseIntro);
        if (timingMode_ == kAutoIntro || timingMode_ == kAutoUserIdle) SetNextTime(periodMin_, periodMax_);
        break;
    case kPhaseMain:
        RunScript(kEvPhaseMain);
        if (timingMode_ == kAutoUserIdle || timingMode_ == kAutoMain) SetNextTime(periodMin_, periodMax_);
        break;
    default: break;
    }
}

void Item::Stop() {
    if (!playing_) return;
    loops_ = 0;
    Seek(0xffff);
    Done(true);
}

void Item::Notify(uint16_t data, uint16_t from) {
    vm_->AutoEventNotify(this, data, from);
    if (timingMode_ == kAutoSync && periodMin_ == data && periodMax_ == from) SetNextTime(0, 0);
    RunScript(kEvNotified, data, from);
}

void Item::Load() {
    if (loaded_) return;
    loaded_ = true;
    // items loaded on demand (no load phase) come up enabled, like phase-loaded ones
    if (phase_ == kPhaseNone && !enabled_) SetEnabled(true);
    if (timingMode_ == kAutoLoad) SetNextTime(periodMin_, periodMax_);
    RunScript(kEvItemLoad);
    // Chute babies (TUBE): "<Baby>Comment" plays in place of the hidden "<Baby>Tube" picture
    // (same authored rect). The scripts pin the comment to the intro picture, which only lines
    // up when the babies sit in the first node's rear view; put it where its baby is now.
    if (name_.size() > 7 && name_.compare(name_.size() - 7, 7, "Comment") == 0) {
        Item* b = vm_->ItemByName(name_.substr(0, name_.size() - 7) + "Tube");
        if (b && b != this && b->origRect_.left == origRect_.left && b->origRect_.top == origRect_.top &&
            b->origRect_.right == origRect_.right && b->origRect_.bottom == origRect_.bottom)
            MoveBy(b->rect_.left - rect_.left, b->rect_.top - rect_.top);
    }
}

void Item::Unload() { vm_->EndRollover(this); loaded_ = false; }

void Item::MoveBy(int dx, int dy) {
    if (!dx && !dy) return;
    rect_.Translate(dx, dy);
    drawOffset_.x += dx;
    drawOffset_.y += dy;
    if (moves_.size() < 256) moves_.push_back(Move{++vm_->moveClock_, dx, dy});
    // children (setparent) follow
    for (Item* child : vm_->created_)
        if (child && !child->dead_ && child->parent_ == serial_ && child != this) child->MoveBy(dx, dy);
}

void Item::SetNextTime(uint32_t minMs, uint32_t maxMs) { SetNextTime(minMs, maxMs, vm_->Now()); }
void Item::SetNextTime(uint32_t minMs, uint32_t maxMs, uint32_t start) {
    if (maxMs < minMs) std::swap(minMs, maxMs);
    const uint32_t r = maxMs > minMs ? std::uniform_int_distribution<uint32_t>(minMs, maxMs)(vm_->Rng()) : minMs;
    nextTime_ = std::max<uint32_t>(1, start + r);
}

bool Item::CheckCondition(const std::string& condition) {
    Code tmp(vm_);
    const uint32_t off = tmp.Compile(condition);
    return !tmp.Run(this, off).IsZero();
}

void Item::RunCommand(const std::string& command) {
    Code tmp(vm_);
    const uint32_t off = tmp.Compile(command);
    tmp.Run(this, off);
}

void Item::RunScript(uint16_t event, uint16_t data, uint16_t from) {
    static const bool trace = DevEnv("LBTRACE") != nullptr;
    if (trace) {
        bool any = false;
        for (const ScriptEntry& e : scripts_) if (e.event == event) { any = true; break; }
        static const char* only = DevEnv("LBTRACEF");             // name filter, comma-separated
        if (only && any) {
            bool hit = false;
            std::string f(only);
            for (size_t a = 0; a <= f.size() && !hit;) {
                const size_t b = std::min(f.find(',', a), f.size());
                if (b > a && name_.find(f.substr(a, b - a)) != std::string::npos) hit = true;
                a = b + 1;
            }
            any = hit;
        }
        if (any && event != kEvRolloverMove && event != kEvMouseTrackMove) {
            char b[128];
            std::snprintf(b, sizeof b, "t=%u %s ev %u (data %u from %u)", vm_->Now(), name_.c_str(), event, data, from);
            vm_->Log(b);
        }
    }
    // entries can be added while running (not in practice); index loop keeps it safe.
    // An item destroyed by one of its own entries still finishes this event's entries (the
    // gate's breakout handler destroys the gate and then starts the cutscene).
    if (dead_) return;
    vm_->runStack_.emplace_back(this, event);
    struct Pop { Engine* e; ~Pop() { e->runStack_.pop_back(); } } pop{vm_};
    NativeEvent(event);
    for (size_t i = 0; i < scripts_.size(); ++i) {
        ScriptEntry& e = scripts_[i];
        if (e.event != event) continue;
        // from 0 in a handler matches any sender
        // match(from=65535, notify=N): the item's own cue (an animation's notify N reaches
        // every item, but only the one that sent it reacts - the stair toss's ten arm
        // animations each check their own frame against Spike) or the player's own (from 65535)
        if (event == kEvNotified && ((e.matchFrom && e.matchFrom != 0xffff && e.matchFrom != from) ||
                                     (e.matchFrom == 0xffff && from != 0xffff && from != itemId_) || e.matchNotify != data)) continue;
        bool ok = true;
        for (const std::string& c : e.conditions) if (!CheckCondition(c)) { ok = false; break; }
        if (!ok) continue;
        if (e.type == 0x67) {
            NotifyEvent n{NotifyEvent::kGotoPage};
            switch (e.opcode) {
            case 1: continue;                                   // GUI action: not used by this game's pages
            case 2: n.kind = NotifyEvent::kGotoControls; break;
            case 3: n.kind = NotifyEvent::kGotoPage; n.param = e.param; break;
            case 4: n.kind = NotifyEvent::kGotoQuit; break;
            case 5: n.kind = NotifyEvent::kIntroDone; n.param = e.param; break;
            case 6:
                if (e.param == 1) { n.kind = NotifyEvent::kChangeMode; n.mode = e.newMode; n.page = e.newPage; n.subpage = e.newSubpage; }
                else { n.kind = NotifyEvent::kCursor; n.text = e.newCursor; }
                break;
            case 0xd: n.kind = NotifyEvent::kQuit; break;
            default: continue;
            }
            vm_->PostNotify(n);
        } else {
            RunScriptEntry(e);
        }
    }
}

int Item::RunScriptEntry(ScriptEntry& e) {
    if (e.state == 0xffff) return 0;

    // Resolve the targets first (expression targets can name lists of items).
    std::vector<Item*> targets;
    if (!e.argc) targets.push_back(this);
    else if (e.targetingType == 0x3f3f || e.targetingType == 0xffff) {
        for (const std::string& t : e.targets) {
            Value v;
            if (e.targetingType == 0x3f3f && vm_->HasVar(t)) v = vm_->Var(t);
            else v = Value(t);
            if (v.type == VT::List && v.list) { for (const Value& x : v.list->a) targets.push_back(vm_->Resolve(x)); }
            else if (Lower(t) == "self") targets.push_back(this);
            else targets.push_back(vm_->Resolve(v));
        }
    } else {
        for (uint16_t id : e.argTarget) {
            // id 0xffff: the item running the script (REPTAR's thrown-trash anims disable
            // themselves with it, so a cleared item can't be caught and thrown again)
            if (id == 0xffff) { targets.push_back(this); continue; }
            if (id != 0) { targets.push_back(vm_->ItemById(id)); continue; }
            // target 0: every item. disable/enable switch the all-items lock; events go to all.
            if (e.opcode == 0x8 || e.opcode == 0x9) { vm_->LockAll(e.opcode == 0x8); continue; }
            for (Item* x : vm_->AllItems()) if (!x->Dead()) targets.push_back(x);
        }
        if (targets.empty()) return 0;
    }

    size_t start = 0, count = std::max<size_t>(1, targets.size());
    if (e.opcode != 0xfffe) switch (e.param) {
    case 0xfffe: e.state = 0xffff; break;                       // run once
    case 0xffff: break;                                         // all targets
    case 0: case 1: case 2:                                     // one target per run, in sequence
        start = e.state;
        e.state++;
        if (e.state >= count) {
            if (e.param == 0) { e.state = 0xffff; if (start >= count) return 0; }
            else if (e.param == 1) e.state = uint16_t(count - 1);
            else e.state = 0;
        }
        count = start + 1;
        break;
    case 3:                                                     // random target
        start = std::uniform_int_distribution<size_t>(0, count - 1)(vm_->Rng());
        count = start + 1;
        break;
    default: break;
    }

    for (size_t n = start; n < count; ++n) {
        Item* target = n < targets.size() ? targets[n] : nullptr;
        if (!target || (target->dead_ && target != this)) continue;

        if ((e.opcode & 0xff00) == 0x0100) {                    // run event 0xNN on the target
            // ("caller" is an ordinary script variable in this game - set with caller = self
            // before a palette switch that then plays it - so events leave it alone)
            // an event sent to a group goes to the group and on to every member
            std::vector<Item*> recipients{target};
            for (size_t k = 0; k < recipients.size() && k < 4096; ++k)
                if (recipients[k]->Type() == kItemGroup)
                    for (Item* m : recipients[k]->Members())
                        if (m && std::find(recipients.begin(), recipients.end(), m) == recipients.end()) recipients.push_back(m);
            const uint64_t self = serial_;
            for (Item* r : recipients) {
                if (!vm_->ItemBySerial(r->Serial()) || r->Dead()) continue;
                r->RunScript(uint16_t(e.opcode & 0xff), 0, itemId_);
                if (!vm_->ItemBySerial(self)) break;
            }
            continue;
        }
        switch (e.opcode) {
        case 0x0: break;
        case 0x1: target->SetVisible(true); break;
        case 0x2: target->TogglePlaying(false, true); break;
        case 0x3: target->SetVisible(false); break;
        case 0x4: target->SetVisible(true); break;
        case 0x5: target->DestroySelf(); break;
        case 0x6: target->Seek(1); break;
        case 0x7: target->Stop(); break;
        case 0x8: {                                             // disable
            // An animation switched off in mid-play stops and goes back to its first picture:
            // Angelica's spit-out disables her idle animation while it runs, then shows it and
            // her home picture again - a paused idle would carry on, or freeze half-drawn,
            // beside the home picture (two Angelicas). A baby's home animation (PhilA), switched
            // off as its idle starts, keeps its rest pose for when it is shown again.
            std::vector<Item*> all{target};
            for (size_t k = 0; k < all.size() && k < 4096; ++k)
                if (all[k]->Type() == kItemGroup)
                    for (Item* m : all[k]->Members()) if (m && std::find(all.begin(), all.end(), m) == all.end()) all.push_back(m);
            for (Item* x : all)
                if (x->Type() == kItemAnim && x->IsPlaying()) { if (DevEnv("LBTRACE")) vm_->Log("halt " + x->Name()); static_cast<AnimItem*>(x)->Halt(); }
            target->SetEnabled(false);
            break;
        }
        case 0x9: target->SetEnabled(true); break;
        case 0xb: target->SetGlobalVisible(false); break;
        case 0xc: target->SetGlobalVisible(true); break;
        case 0xd: target->SetGlobalEnabled(false); break;
        case 0xe: target->SetGlobalEnabled(true); break;
        case 0xf: target->Seek(0xffff); break;
        case 0x11: case 0x12: break;                            // mute / unmute
        case 0x13: case 0x14: target->Load(); break;
        case 0x15: target->Unload(); break;
        // next / previous picture (FrameNumber is the picture shown, counting from 1): the fishing
        // picture's pile turns up the piece under the one taken, the view selector steps both ways
        case 0x16: target->Seek(uint16_t(target->FrameNumber() + 1)); break;
        case 0x17: target->Seek(uint16_t(std::max(1, target->FrameNumber() - 1))); break;
        case 0x18: vm_->BeginDrag(target, vm_->MouseWorld()); break;
        case 0x19: vm_->EndDrag(target); break;
        case 0x1a: case 0x1b: case 0x1c: break;
        case 0x1d:
            if (page_ && page_->GetCode()) page_->GetCode()->Run(this, e.offset);
            break;
        case 0xfffe:
            for (size_t i = 0; i < e.subs.size(); ++i) {
                ScriptEntry& s = e.subs[i];
                const int r = RunScriptEntry(s);
                if (s.opcode == 0xfffb) { if (!r) i = size_t(s.jumpTarget) - 1; }
                else if (s.opcode == 0xfffc) i = e.subs.size();
                else if (s.opcode == 0xfffd) i = size_t(r) - 1;
            }
            break;
        case 0xffff: ReadData(e.dataType, e.data, false); break;
        case 0xfffb: case 0xfffc: case 0xfffd:
            return page_ && page_->GetCode() ? page_->GetCode()->Run(this, e.offset).ToInt() : 0;
        default: {
            char b[96];
            std::snprintf(b, sizeof b, "%s: script opcode 0x%04x not implemented", name_.c_str(), e.opcode);
            vm_->Log(b);
        }
        }
    }
    return 0;
}

// ---- picture ---------------------------------------------------------------------------
bool PictureItem::ReadOwnData(uint16_t t, const std::vector<uint8_t>& d) {
    if (t == 0x6b) { if (d.size() >= 2) drawMode_ = BE16(&d[0]); return true; }
    return false;
}
void PictureItem::Init() {
    vm_->GetBitmap(page_ ? page_->Archive() : nullptr, resourceId_);
    Item::Init();
}
bool PictureItem::Contains(Point pt) {
    if (!Item::Contains(pt)) return false;
    if (!doHitTest_) return true;
    const Bitmap* b = vm_->GetBitmap(page_ ? page_->Archive() : nullptr, resourceId_);
    int x = pt.x - rect_.left, y = pt.y - rect_.top;
    if (vm_->wrap_ && b) { const int ww = vm_->world_.Width(); while (x < 0) x += ww; while (x >= ww) x -= ww; }
    return OpaqueAt(b, x, y);
}
void PictureItem::Draw(Canvas& c) {
    if (!loaded_ || !visible_ || !globalVisible_) return;
    if (const Bitmap* b = vm_->GetBitmap(page_ ? page_->Archive() : nullptr, resourceId_)) Blit(c, *b, rect_.left, rect_.top, vm_);
}

// ---- animation -------------------------------------------------------------------------
void AnimItem::Init() {
    const mohawk::Archive* a = page_ ? page_->Archive() : nullptr;
    std::vector<uint8_t> ani = vm_->GetResource(a, mohawk::Tag("ANI "), resourceId_);
    if (ani.size() >= 30) {
        bounds_ = Rect{S16(&ani[2]), S16(&ani[4]), S16(&ani[6]), S16(&ani[8])};
        clip_ = Rect{S16(&ani[10]), S16(&ani[12]), S16(&ani[14]), S16(&ani[16])};
        const uint16_t spr = uint16_t(BE32(&ani[22]));
        std::vector<uint8_t> s = vm_->GetResource(a, mohawk::Tag("SPR#"), spr);
        std::vector<uint16_t> scripts;
        if (s.size() >= 24) {
            const uint16_t front = BE16(&s[2]);
            const uint16_t shp = uint16_t(BE32(&s[4]));
            scripts.push_back(uint16_t(BE32(&s[12])));
            for (uint16_t k = 0; k < front && 24 + size_t(k + 1) * 24 <= s.size(); ++k)
                scripts.push_back(uint16_t(BE32(&s[24 + size_t(k) * 24 + 12])));
            std::vector<uint8_t> sh = vm_->GetResource(a, mohawk::Tag("SHP#"), shp);
            if (sh.size() >= 2) {
                const uint16_t n = BE16(&sh[0]);
                for (uint16_t k = 0; k < n && 2 + size_t(k + 1) * 6 <= sh.size(); ++k) {
                    shapes_.push_back(BE16(&sh[2 + size_t(k) * 6]));
                    offsets_.push_back(Point{S16(&sh[4 + size_t(k) * 6]), S16(&sh[6 + size_t(k) * 6])});
                }
            }
        }
        for (uint16_t sc : scripts) {
            AnimNode node;
            std::vector<uint8_t> d = vm_->GetResource(a, mohawk::Tag("SCRP"), sc);
            size_t p = 0;
            while (p + 2 <= d.size()) {
                const uint8_t op = d[p], len = d[p + 1];
                if (op == 0) break;
                if (p + 2 + len > d.size()) break;
                node.ops.push_back(AnimNode::Op{op, std::vector<uint8_t>(d.begin() + long(p) + 2, d.begin() + long(p) + 2 + len)});
                p += 2 + size_t(len);
            }
            if (const char* f = DevEnv("ANIMOPS"); f && name_ == f) {
                std::string line = name_ + " node " + std::to_string(nodes_.size()) + ":";
                for (const auto& o : node.ops) {
                    char b[48]; std::snprintf(b, sizeof b, " %x", o.op); line += b;
                    for (size_t i = 0; i + 1 < o.d.size() && i < 8; i += 2) { std::snprintf(b, sizeof b, "%s%d", i ? "," : "(", S16(&o.d[i])); line += b; }
                    if (o.d.size() >= 2) line += ")";
                }
                vm_->Log(line);
            }
            nodes_.push_back(std::move(node));
        }
        built_ = true;
    } else {
        char b[96];
        std::snprintf(b, sizeof b, "%s: no ANI %u", name_.c_str(), resourceId_);
        vm_->Log(b);
    }
    Item::Init();
}

void AnimItem::SetEnabled(bool enabled) {
    if (running_) {
        if (enabled && globalEnabled_ && !loaded_) AnimStart();
        else if (loaded_ && !enabled && En()) AnimStop();
    }
    Item::SetEnabled(enabled);
}

bool AnimItem::CelTransparentAt(const AnimNode& n, int x, int y) const {
    if (!n.cel || n.cel > shapes_.size()) return true;
    const Bitmap* b = const_cast<Engine*>(vm_)->GetBitmap(page_ ? page_->Archive() : nullptr, shapes_[n.cel - 1]);
    const int ox = bounds_.left + drawOffset_.x + n.x - offsets_[n.cel - 1].x;
    const int oy = bounds_.top + drawOffset_.y + n.y - offsets_[n.cel - 1].y;
    return !OpaqueAt(b, x - ox, y - oy);
}

bool AnimItem::Contains(Point pt) {
    // animations always hit-test their drawn pixels (TDROP's TableMask is a shaped zone)
    if (!Item::Contains(pt)) return false;
    for (const AnimNode& n : nodes_)
        if (!CelTransparentAt(n, pt.x, pt.y)) return true;
    if (vm_->wrap_) {
        const int ww = vm_->world_.Width();
        for (const AnimNode& n : nodes_)
            if (!CelTransparentAt(n, pt.x + ww, pt.y) || !CelTransparentAt(n, pt.x - ww, pt.y)) return true;
    }
    return false;
}

AnimItem::NodeState AnimItem::Step(AnimNode& n, bool seeking) {
    // a delay (op 0xf) at the end of a node still runs out before the node is done: the
    // trash highlights hold their picture for the length of the babies' jump
    if (n.delay > 0 && --n.delay) return kRunning;
    if (n.cur >= n.ops.size()) return kDone;
    while (n.cur < n.ops.size()) {
        const AnimNode::Op& o = n.ops[n.cur++];
        const auto& d = o.d;
        switch (o.op) {
        case 0x2:                                               // notify
            if (!seeking && d.size() >= 2) vm_->NotifyAll(BE16(&d[0]), itemId_);
            break;
        case 0x3:                                               // tempo (1/60 s units)
            if (d.size() >= 2) tempo_ = std::max<uint32_t>(1, uint32_t(BE16(&d[0])) * 1000 / 60);
            break;
        case 0x5:
            if (d.size() >= 4) { n.x = S16(&d[0]); n.y = S16(&d[2]); }
            break;
        case 0x6: return kRunning;                              // wait one frame
        case 0x7:
            if (d.size() >= 2) n.cel = BE16(&d[0]) <= shapes_.size() ? BE16(&d[0]) : 0;
            break;
        case 0x8:                                               // sleep until frame
            if (d.size() >= 4 && BE32(&d[0]) > frame_) { n.cur--; return kRunning; }
            break;
        case 0x9: break;                                        // draw mode
        case 0xa: case 0xb: case 0xc: case 0xd: {
            if (d.size() < 2) break;
            const uint16_t id = BE16(&d[0]);
            if (o.op == 0xa) {
                if (seeking) break;
                // while the page is still fading in, pictures may come up but voices wait
                if (vm_->holdVoices_) { n.cur--; return kWaiting; }
                if (sound_) vm_->StopSound(sound_);
                sound_ = vm_->PlaySound(nullptr, id, page_ ? page_->Archive() : nullptr);
                soundId_ = id;
                if (sound_) soundWaitFrom_ = vm_->Now();
            } else if (o.op == 0xb) {
                if (seeking) break;
                if (soundId_ == id && sound_ && vm_->SoundPlaying(sound_)) { n.cur--; return kWaiting; }
            } else {
                if (soundId_ == id && sound_) { vm_->StopSound(sound_); sound_ = 0; }
            }
            break;
        }
        case 0xe:                                               // tempo divisor: frames per second
            if (d.size() >= 2 && BE16(&d[0])) tempo_ = std::max<uint32_t>(1, 1000u / BE16(&d[0]));
            break;
        case 0xf:
            if (d.size() >= 4) { n.delay = BE32(&d[0]); if (n.delay) return kRunning; }
            break;
        default: break;
        }
    }
    return kRunning;
}

bool AnimItem::AnimUpdate() {
    if (!animRunning_) return false;
    const uint32_t now = vm_->Now();
    // a voice starts at the sound device's next buffer, a moment after the frame that started
    // it: the picture waits for it (up to 0.3 s), so mouths stay with the words
    if (soundWaitFrom_) {
        const double st = sound_ ? vm_->SoundTime(sound_) : -2;
        if (st == 0 && now - soundWaitFrom_ < 300) { lastTime_ = now; return false; }
        soundWaitFrom_ = 0;
    }
    if (lastTime_ && now <= lastTime_ + tempo_) return false;
    if (lastTime_ == 0 || now > lastTime_ + tempo_ * 2) lastTime_ = now;
    else lastTime_ += tempo_;
    if (sound_ && !vm_->SoundPlaying(sound_)) sound_ = 0;
    NodeState state = kDone;
    for (AnimNode& n : nodes_) {
        NodeState s = Step(n, false);
        if (s == kWaiting) { state = kWaiting; break; }
        if (s == kRunning) state = kRunning;
    }
    if (const char* f = DevEnv("ANIMDBG"); f && name_.find(f) != std::string::npos) {
        static std::map<const void*, int> lastState;
        if (DevEnv("ANIMFRAMES") && state == kRunning) {
            char b[160];
            std::snprintf(b, sizeof b, " frame %u t=%u tempo %u sound %d at %.3f", frame_, vm_->Now(), tempo_, sound_, sound_ ? vm_->SoundTime(sound_) : -1.0);
            vm_->Log(name_ + b);
        }
        if (lastState[this] != int(state) + 1) {
            lastState[this] = int(state) + 1;
            vm_->Log(name_ + " frame " + std::to_string(frame_) + (state == kRunning ? " running" : state == kWaiting ? " waiting for its sound" : sound_ ? " done, sound still playing" : " done"));
        }
    }
    if (state == kRunning) frame_++;
    else if (state == kDone && !sound_) { animRunning_ = false; return true; }
    return false;
}

void AnimItem::Halt() {
    AnimStop();
    running_ = false;
    playing_ = false;
    loops_ = 0;
    Seek(1);                                   // back to its first picture (a baby's rest pose)
    animRunning_ = false;
}

void AnimItem::AnimStop() {
    animRunning_ = false;
    soundWaitFrom_ = 0;
    if (sound_) { vm_->StopSound(sound_); sound_ = 0; }
}

void AnimItem::Update() {
    if (built_ && loaded_ && En() && running_) {
        if (!animRunning_) { animRunning_ = true; }
        if (AnimUpdate()) {
            running_ = false;
            if (!vm_->DeferAnimDone(this)) Done(true);
        }
    }
    const bool wasRunning = running_;
    Item::Update();
    // started just now (its timer came due): its first picture goes up in this same frame -
    // a baby's home picture hidden by a click must not leave the spot empty for a frame
    if (!wasRunning && running_ && built_ && loaded_ && En()) {
        animRunning_ = true;
        if (AnimUpdate()) {
            running_ = false;
            if (!vm_->DeferAnimDone(this)) Done(true);
        }
    }
}

bool AnimItem::TogglePlaying(bool playing, bool restart) {
    if (playing) {
        if (loaded_ && En()) {
            if (restart) Seek(1);
            running_ = true;
            lastTime_ = 0;
            animRunning_ = true;
        }
        return running_;
    }
    return Item::TogglePlaying(playing, restart);
}

void AnimItem::Done(bool onlyNotify) {
    if (!onlyNotify) AnimStop();
    Item::Done(onlyNotify);
}

void AnimItem::Seek(uint16_t pos) {
    lastTime_ = 0;
    frame_ = 0;
    if (sound_) { vm_->StopSound(sound_); sound_ = 0; }
    for (AnimNode& n : nodes_) n.Reset();
    for (uint16_t k = 0; k < pos; ++k) {
        bool ran = false;
        for (AnimNode& n : nodes_) ran |= Step(n, true) != kDone;
        if (!ran) { animRunning_ = false; break; }   // past the end: stays on the last picture
        frame_++;
    }
}

void AnimItem::SeekToTime(uint32_t ms) {
    lastTime_ = 0;
    frame_ = 0;
    if (sound_) { vm_->StopSound(sound_); sound_ = 0; }
    for (AnimNode& n : nodes_) n.Reset();
    uint32_t elapsed = 0;
    while (elapsed <= ms) {
        bool ran = false;
        for (AnimNode& n : nodes_) ran |= Step(n, true) != kDone;
        elapsed += tempo_;
        frame_++;
        if (!ran) { animRunning_ = false; break; }
    }
}

void AnimItem::StartPhase(uint16_t phase) {
    if (phase == phase_) Seek(1);
    Item::StartPhase(phase);
}

void AnimItem::Load() {
    const bool was = loaded_;
    // an anim loaded on demand shows its first frame, like one loaded in its phase
    if (!was && phase_ == kPhaseNone) Seek(1);
    Item::Load();
}

void AnimItem::SilenceVoice() { if (sound_) { vm_->StopSound(sound_); sound_ = 0; } soundWaitFrom_ = 0; }

void AnimItem::Stop() {
    if (running_) { AnimStop(); Seek(0xffff); }
    running_ = false;
    Item::Stop();
}

void AnimItem::OnDestroyed() { AnimStop(); }

Rect AnimItem::FrameRect() const {
    Rect r{};
    bool any = false;
    for (const AnimNode& n : nodes_) {
        if (!n.cel || n.cel > shapes_.size()) continue;
        const Bitmap* b = const_cast<Engine*>(vm_)->GetBitmap(page_ ? page_->Archive() : nullptr, shapes_[n.cel - 1]);
        if (!b) continue;
        const int ox = bounds_.left + drawOffset_.x + n.x - offsets_[n.cel - 1].x;
        const int oy = bounds_.top + drawOffset_.y + n.y - offsets_[n.cel - 1].y;
        Rect c{ox, oy, ox + b->w, oy + b->h};
        if (DevEnv("FRDBG") && name_.find(DevEnv("FRDBG")) != std::string::npos) {
            char t[160]; std::snprintf(t, sizeof t, "framerect %s cel %d at %d,%d-%d,%d", name_.c_str(), int(n.cel), c.left, c.top, c.right, c.bottom);
            const_cast<Engine*>(vm_)->Log(t);
        }
        if (!any) { r = c; any = true; }
        else { r.left = std::min(r.left, c.left); r.top = std::min(r.top, c.top); r.right = std::max(r.right, c.right); r.bottom = std::max(r.bottom, c.bottom); }
    }
    return any ? r : rect_;
}

void AnimItem::Draw(Canvas& c) {
    if (!loaded_ || !visible_ || !globalVisible_) return;
    // The ANI clip only matters when it is tighter than the screen.
    SDL_Rect clipRect;
    const SDL_Rect* clip = nullptr;
    if (clip_.Width() > 0 && clip_.Height() > 0 && (clip_.Width() < 640 || clip_.Height() < 480)) {
        clipRect = SDL_Rect{clip_.left + drawOffset_.x, clip_.top + drawOffset_.y, clip_.Width(), clip_.Height()};
        clip = &clipRect;
    }
    for (const AnimNode& n : nodes_) {
        if (!n.cel || n.cel > shapes_.size()) continue;
        const Bitmap* b = vm_->GetBitmap(page_ ? page_->Archive() : nullptr, shapes_[n.cel - 1]);
        if (!b) continue;
        Blit(c, *b, bounds_.left + drawOffset_.x + n.x - offsets_[n.cel - 1].x,
             bounds_.top + drawOffset_.y + n.y - offsets_[n.cel - 1].y, vm_, clip);
    }
}

// ---- sound -------------------------------------------------------------------------------
void SoundItem::Update() {
    // a sound started while the page was coming up from black begins once it is fully up
    // (reference: the smash page's tube hum only starts when the fade-in is over)
    if (heldStart_ && !vm_->holdVoices_) { heldStart_ = false; TogglePlaying(true, true); }
    if (running_ && !vm_->SoundPlaying(voice_)) { running_ = false; voice_ = 0; Done(true); }
    Item::Update();
}
bool SoundItem::TogglePlaying(bool playing, bool restart) {
    if (!playing) return Item::TogglePlaying(playing, restart);
    if (running_) { running_ = false; vm_->StopSound(voice_); voice_ = 0; }
    paused_ = false;
    heldStart_ = false;
    if (!loaded_ || !En()) return false;
    if (vm_->holdVoices_) { heldStart_ = true; return true; }
    voice_ = vm_->PlaySound(this, resourceId_, page_ ? page_->Archive() : nullptr);
    running_ = voice_ != 0;
    if (!running_) { vm_->QueueDone(this); return true; }       // missing file: finish at once
    return true;
}
void SoundItem::Stop() {
    heldStart_ = false;
    if (running_) { running_ = false; vm_->StopSound(voice_); voice_ = 0; }
    paused_ = false;
    Item::Stop();
}
void SoundItem::OnDestroyed() { if (running_) vm_->StopSound(voice_); running_ = false; paused_ = false; }
// Switched off globally, a playing sound holds where it is and carries on when switched back
// on: the story cutscenes silence the room music (xvStateSnd) while they run.
void SoundItem::SetGlobalEnabled(bool enabled) {
    Item::SetGlobalEnabled(enabled);
    if (!running_ || !voice_) return;
    const bool off = !globalEnabled_;
    if (off != paused_) { paused_ = off; vm_->PauseSound(voice_, off); }
}

// ---- group -------------------------------------------------------------------------------
bool GroupItem::ReadOwnData(uint16_t t, const std::vector<uint8_t>& d) {
    if (t != 0x64) return false;
    ids_.clear();
    if (d.size() >= 2) {
        const uint16_t n = BE16(&d[0]);
        for (uint16_t k = 0; k < n && 2 + size_t(k + 1) * 4 <= d.size(); ++k) {
            const uint16_t id = BE16(&d[4 + size_t(k) * 4]);
            if (id != itemId_) ids_.push_back(id);
        }
    }
    return true;
}
std::vector<Item*> GroupItem::Members() {
    std::vector<Item*> v;
    for (uint16_t id : ids_) if (Item* it = vm_->ItemById(id)) v.push_back(it);
    for (uint64_t s : serials_) if (Item* it = vm_->ItemBySerial(s)) v.push_back(it);
    return v;
}
void GroupItem::DestroySelf() { Item::DestroySelf(); for (Item* it : Members()) it->DestroySelf(); }
void GroupItem::SetEnabled(bool enabled) {
    if (starting_) { Item::SetEnabled(enabled); return; }
    for (Item* it : Members()) it->SetEnabled(enabled);
}
void GroupItem::SetGlobalEnabled(bool enabled) { for (Item* it : Members()) it->SetGlobalEnabled(enabled); }
bool GroupItem::TogglePlaying(bool playing, bool restart) { for (Item* it : Members()) it->TogglePlaying(playing, restart); return false; }
void GroupItem::Seek(uint16_t pos) { for (Item* it : Members()) it->Seek(pos); }
// A group's own show / hide (local visibility) is just the group's flag, as in the original
// player, which forwards only the global show / hide to the members (the toolbar opens and
// closes its buttons that way). Forwarding the local one would make every hidden member
// visible when the inventory closes and shows grpZones again: each baby's hidden reaction
// animations (Angelica's spit-out-cookie one then sat invisibly over her and took the cookie
// click) and a baby's home picture beside his playing animation.
void GroupItem::SetVisible(bool v) { Item::SetVisible(v); }
void GroupItem::SetGlobalVisible(bool v) { for (Item* it : Members()) it->SetGlobalVisible(v); }
void GroupItem::StartPhase(uint16_t phase) { starting_ = true; Item::StartPhase(phase); starting_ = false; }
void GroupItem::Stop() { for (Item* it : Members()) it->Stop(); }
void GroupItem::Load() {
    Item::Load();
    if (starting_) return;              // its own load phase: members load in their own phases
    for (Item* it : Members()) it->Load();
}
void GroupItem::Unload() { for (Item* it : Members()) it->Unload(); }
void GroupItem::MoveBy(int dx, int dy) { for (Item* it : Members()) it->MoveBy(dx, dy); }

// ---- movie -------------------------------------------------------------------------------
void MovieItem::Close() {
    if (voice_) { vm_->StopSound(voice_); voice_ = 0; }
    video_.reset();
    ended_ = false;
}
void MovieItem::Update() {
    if (closeAt_ && vm_->Now() >= closeAt_) { closeAt_ = 0; if (ended_) Close(); }
    if (playing_ && video_ && !ended_) {
        if (video_->AtEnd() || !video_->Playing()) {
            // A climb or descent between floors ends on the empty stairs, and the original
            // stays on that picture for a good second (loading the next floor from the CD)
            // before the new floor appears. The port loads at once, which cut the end short.
            if (!holdUntil_) {
                const std::string tr = vm_->HasVar("xvTransition") ? vm_->Str(vm_->Var("xvTransition")) : std::string();
                const bool floors = !tr.empty() && tr != "NONE" && tr.find("to") != std::string::npos && name_.rfind("Zoom", 0) == 0;
                holdUntil_ = floors ? vm_->Now() + 1200 : vm_->Now();
            }
            if (vm_->Now() < holdUntil_) { Item::Update(); return; }
            holdUntil_ = 0;
            // its last picture stays up as QuickTime leaves it: a zoom until the script moves
            // it away; any other movie for a moment (kHoldLastMs) - long enough for a page change
            // its done event starts (Tommy running upstairs from the control room) to leave
            // from that picture rather than from the empty page behind it
            if (voice_) { vm_->StopSound(voice_); voice_ = 0; }
            video_->Pause();
            ended_ = true;
            if (name_.rfind("Zoom", 0) != 0) closeAt_ = vm_->Now() + kHoldLastMs;
            vm_->movieEndAt_ = vm_->Now();
            Done(true);
        }
    } else if (playing_ && !video_) {
        Done(true);
    }
    Item::Update();
}
bool MovieItem::TogglePlaying(bool playing, bool restart) {
    if (playing) {
        holdUntil_ = 0;
        if ((loaded_ && En()) || phase_ == kPhaseNone) {
            Close();
            OpenVideo();
            if (video_) {
                if (DevEnv("CHLOG")) vm_->Log(name_ + " plays, channels " + std::to_string(video_->Channels()));
                video_->Play();
                if (!video_->Asset().audio.empty()) voice_ = vm_->PlayWav(video_->Asset().audio);
                // a full-screen cutscene takes the stage: a character still talking behind it
                // (Chuckie peeking from the trash can when Go is pressed) falls silent instead
                // of talking over the movie
                if (rect_.Width() >= 640 && rect_.Height() >= 400)
                    for (Item* it : vm_->Created())
                        if (auto* a = dynamic_cast<AnimItem*>(it); a && !a->Dead()) a->SilenceVoice();
            }
            if (!video_) {
                char b[96];
                std::snprintf(b, sizeof b, "%s: movie %u not available, skipped", name_.c_str(), resourceId_);
                vm_->Log(b);
            }
            return true;
        }
    }
    return Item::TogglePlaying(playing, restart);
}
void MovieItem::OpenVideo() {
    if (!page_) return;
    if (auto m = vm_->MovieAsset(*page_->Archive(), resourceId_)) {
        auto v = std::make_unique<port::VideoPlayer>();
        if (v->Open(*m)) {
            for (auto& [ch, on] : channels_) v->SetChannelEnabled(ch, on);
            video_ = std::move(v);
        }
    }
}
// A zoom between rooms shows its first picture as soon as it is loaded (QuickTime puts the
// movie up when it opens): the babies hidden by the click are already in it, so the room
// doesn't stand empty while the script waits (MAIN's Cludge) before starting it.
void MovieItem::Load() {
    const bool was = loaded_;
    Item::Load();
    if (!was && loaded_ && !video_ && name_.rfind("Zoom", 0) == 0) OpenVideo();
}
void MovieItem::Unload() {
    if (!playing_) Close();
    Item::Unload();
}
void MovieItem::Stop() { Close(); Item::Stop(); }
void MovieItem::SetChannel(int channel, bool shown) {
    if (DevEnv("CHLOG")) vm_->Log(name_ + (shown ? " showchannel " : " hidechannel ") + std::to_string(channel));
    channels_[channel] = shown;
    if (video_) video_->SetChannelEnabled(channel, shown);
}
void MovieItem::OnDestroyed() { Close(); }

// ---- palette -----------------------------------------------------------------------------
bool PaletteItem::ReadOwnData(uint16_t t, const std::vector<uint8_t>& d) {
    if (t != 0x72) return false;
    if (d.size() < 8) return true;
    period_ = BE16(&d[0]); step_ = std::max<uint16_t>(1, BE16(&d[2])); start_ = BE16(&d[4]); count_ = BE16(&d[6]);
    colors_.clear();
    for (size_t k = 0; k < count_ && 8 + (k + 1) * 4 <= d.size(); ++k)
        colors_.push_back({d[8 + k * 4], d[9 + k * 4], d[10 + k * 4]});
    return true;
}
bool PaletteItem::TogglePlaying(bool playing, bool restart) {
    if (playing) {
        const mohawk::Palette& cur = vm_->CurrentPalette();
        from_.clear();
        for (size_t k = 0; k < colors_.size(); ++k) from_.push_back({cur.rgb[k][0], cur.rgb[k][1], cur.rgb[k][2]});
        fadeStart_ = vm_->Now();
        fading_ = true;
        return true;
    }
    return Item::TogglePlaying(playing, restart);
}
double PaletteItem::ApplyAt(uint32_t now) {
    const uint32_t total = uint32_t(period_) * step_;
    const uint32_t el = now - fadeStart_;
    const double t = total ? std::min(1.0, double(el) / total) : 1.0;
    std::vector<std::array<uint8_t, 3>> mix(colors_.size());
    for (size_t k = 0; k < colors_.size(); ++k)
        for (int ch = 0; ch < 3; ++ch)
            mix[k][size_t(ch)] = uint8_t(from_[k][size_t(ch)] + (int(colors_[k][size_t(ch)]) - from_[k][size_t(ch)]) * t + 0.5);
    if (visible_ && globalVisible_) {
        // entries start_..count_-1 (count_ is really an end index)
        std::vector<std::array<uint8_t, 3>> part(mix.begin() + std::min<size_t>(start_, mix.size()), mix.end());
        vm_->SetPalette(start_, part);
    }
    return t;
}
void PaletteItem::Update() {
    if (fading_ && ApplyAt(vm_->Now()) >= 1.0) { fading_ = false; if (playing_) Done(true); }
    Item::Update();
}

// ---- proxy -------------------------------------------------------------------------------
ProxyItem::~ProxyItem() = default;
void ProxyItem::Load() {
    if (loaded_) return;
    sub_ = vm_->LoadProxyPage(name_, this);
    Item::Load();
}
void ProxyItem::Unload() {
    // the player tells a page's items it is going away (0xFFF7): scripts clean up variables and groups
    if (sub_) vm_->NotifyUnload(sub_->Items());
    // the page goes away at the end of this update: a script of its own that unloads it
    // (a replayed cutscene's done event unloads its episode proxy, then sets the fade back
    // to the Replay screen) still runs to its end with its code
    if (sub_) { for (Item* it : sub_->Items()) if (it && !it->Dead()) vm_->Destroy(it); vm_->RetirePage(std::move(sub_)); }
    Item::Unload();
}
std::vector<Item*> ProxyItem::Members() { return sub_ ? sub_->Items() : std::vector<Item*>{}; }

// ---- VectorMotionItem -----------------------------------------------------------------------
void VectorMotionItem::Take() {
    Value& name = vm_->Var("_vmName");
    Item* target = vm_->Resolve(name);
    VectorMotionItem* t = target ? dynamic_cast<VectorMotionItem*>(target) : nullptr;
    if (!t) t = this;
    const Value& b = vm_->Var("_vmBounds");
    t->hasBounds_ = b.type == VT::Rect || b.type == VT::String;
    if (t->hasBounds_) t->bounds_ = b.ToRect();
    t->vx_ = vm_->Var("_vmVx").ToReal();
    t->vy_ = vm_->Var("_vmVy").ToReal();
    t->ax_ = vm_->Var("_vmAx").ToReal();
    t->ay_ = vm_->Var("_vmAy").ToReal();
    t->x_ = t->rect_.left; t->y_ = t->rect_.top;      // from wherever the scripts last put it
    t->outside_ = false;
}

void VectorMotionItem::NativeEvent(uint16_t event) {
    if (event == 252) Take();
    else if (event == 251) {
        vm_->SetVar("_vmVx", Value(vx_)); vm_->SetVar("_vmVy", Value(vy_));
        vm_->SetVar("_vmAx", Value(ax_)); vm_->SetVar("_vmAy", Value(ay_));
    }
}

void VectorMotionItem::Update() {
    if (!En() || (phase_ != kPhaseNone && !loaded_)) return;
    const uint32_t now = vm_->Now();
    if (timingMode_ == kAutoNone) { running_ = false; return; }
    if (!running_) {
        running_ = true; last_ = now;
        x_ = rect_.left; y_ = rect_.top;
        nextTick_ = now + std::max<uint32_t>(1, periodMin_);
        return;
    }
    // the scripts moved it meanwhile (moveto): go on from there
    if (rect_.left != int(std::lround(x_)) || rect_.top != int(std::lround(y_))) { x_ = rect_.left; y_ = rect_.top; }
    const double dt = std::min(0.1, (now - last_) / 1000.0);
    last_ = now;
    if (dt > 0) {
        x_ += vx_ * dt + 0.5 * ax_ * dt * dt; vx_ += ax_ * dt;
        y_ += vy_ * dt + 0.5 * ay_ * dt * dt; vy_ += ay_ * dt;
        bool out = false;
        if (hasBounds_) {
            const double w = rect_.Width(), h = rect_.Height();
            if (x_ < bounds_.left) { x_ = bounds_.left; vx_ = 0; out = true; }
            if (x_ + w > bounds_.right) { x_ = std::max<double>(bounds_.left, bounds_.right - w); vx_ = 0; out = true; }
            if (y_ < bounds_.top) { y_ = bounds_.top; vy_ = 0; out = true; }
            if (y_ + h > bounds_.bottom) { y_ = std::max<double>(bounds_.top, bounds_.bottom - h); vy_ = 0; out = true; }
        }
        MoveTo(int(std::lround(x_)), int(std::lround(y_)));
        if (out && !outside_) { outside_ = true; RunScript(248); if (dead_ || timingMode_ == kAutoNone) return; }
        else if (!out) outside_ = false;
    }
    if (now >= nextTick_) {
        nextTick_ = now + std::max<uint32_t>(1, periodMax_ > periodMin_
                    ? std::uniform_int_distribution<uint32_t>(periodMin_, periodMax_)(vm_->Rng()) : periodMin_);
        RunScript(kEvDone);
    }
}

} // namespace lb
