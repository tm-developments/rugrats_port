#include "devenv.h"
#include <cstring>
// lb_engine.cpp - Living Books page engine: pages, item list, phases, input, drawing, LBX.
#include "lb/lb_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lb/lb_code.h"

namespace lb {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
int16_t S16(const uint8_t* p) { return int16_t(BE16(p)); }
std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }
std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}
} // namespace

// ---- LBX 1001: INI data file (player saves, GLOBALS) --------------------------------------
// Calls (the original RUGRATS.EXE / ScummVM LBXDataFile): 1 open(file), 3 addSection(name), 4 getSectionList,
// 5 setCurSection(name), 7 setKey(key, value), 8 loadCurSectionVars, 10 deleteCurSection,
// 14 sectionExists(name). Values are stored as script text and read back by evaluating
// "key = value", so strings are kept quoted and lists in [a, b] form.
class LBXDataFile : public LBXObject {
public:
    explicit LBXDataFile(Engine* e) : vm_(e) {}
    bool Call(int id, const std::vector<Value>& ps, Value& result) override {
        auto P = [&](size_t i) { return i < ps.size() ? ps[i] : Value(); };
        switch (id) {
        case 1: Open(vm_->Str(P(0))); return false;
        case 3: cur_ = vm_->Str(P(0)); Section(cur_); Save(); return false;
        case 4: {
            auto l = std::make_shared<List>();
            for (auto& s : sections_) l->a.push_back(Value(s.name));
            result = Value::MakeList(l);
            return true;
        }
        case 5: cur_ = vm_->Str(P(0)); return false;
        case 7: {
            auto& sec = Section(cur_);
            const std::string key = vm_->Str(P(0)), text = Encode(P(1));
            bool found = false;
            for (auto& kv : sec.keys) if (Lower(kv.first) == Lower(key)) { kv.second = text; found = true; }
            if (!found) sec.keys.emplace_back(key, text);
            Save();
            return false;
        }
        case 8: {
            for (auto& s : sections_) {
                if (Lower(s.name) != Lower(cur_)) continue;
                for (auto& kv : s.keys) {
                    Code tmp(vm_);
                    const uint32_t off = tmp.Compile(kv.first + " = " + kv.second);
                    tmp.Run(nullptr, off);
                }
            }
            // Angelica's "no" count isn't among the saved variables, and her first "no" moves
            // the cookie chapter on: resuming a game saved while looking for her cookie, one
            // click on her (or a coconut cookie, then a click) would skip the cookie hunt. A
            // player who got that far has heard her first "no" already.
            if (vm_->HasVar("xvCallBack") && Lower(vm_->Str(vm_->Var("xvCallBack"))) == "e1chapter2" &&
                vm_->HasVar("xnCurPuzzle") && vm_->Var("xnCurPuzzle").ToInt() == 3 &&
                (!vm_->HasVar("xnAngChewCount") || vm_->Var("xnAngChewCount").ToInt() < 1))
                vm_->SetVar("xnAngChewCount", Value(1));
            return false;
        }
        case 10:
            sections_.erase(std::remove_if(sections_.begin(), sections_.end(), [&](const Sec& s) { return Lower(s.name) == Lower(cur_); }), sections_.end());
            Save();
            return false;
        case 14:
            result = Value(std::any_of(sections_.begin(), sections_.end(), [&](const Sec& s) { return Lower(s.name) == Lower(vm_->Str(P(0))); }));
            return true;
        default: {
            char b[64];
            std::snprintf(b, sizeof b, "LBX data file call %d not implemented", id);
            vm_->Log(b);
            return false;
        }
        }
    }

private:
    struct Sec { std::string name; std::vector<std::pair<std::string, std::string>> keys; };
    Sec& Section(const std::string& name) {
        for (auto& s : sections_) if (Lower(s.name) == Lower(name)) return s;
        sections_.push_back(Sec{name, {}});
        return sections_.back();
    }
    std::string Encode(const Value& v) {
        switch (v.type) {
        case VT::Integer: case VT::Real: return v.ToString();
        case VT::List: {
            std::string s = "[";
            if (v.list) for (size_t k = 0; k < v.list->a.size(); ++k) s += (k ? ", " : "") + Encode(v.list->a[k]);
            return s + "]";
        }
        default: return "\"" + vm_->Str(v) + "\"";
        }
    }
    void Open(const std::string& name) {
        path_ = vm_->ProgramFile(name);
        sections_.clear();
        std::ifstream in(path_);
        std::string line;
        Sec* cur = nullptr;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']') { cur = &Section(line.substr(1, line.size() - 2)); continue; }
            const size_t eq = line.find('=');
            if (eq == std::string::npos || !cur) continue;
            std::string key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
            if (!val.empty() && val.back() == ';') val = Trim(val.substr(0, val.size() - 1));
            cur->keys.emplace_back(key, val);
        }
    }
    void Save() {
        if (path_.empty()) return;
        // written to a temporary file and then swapped in, so a crash or power cut mid-write
        // never leaves a half-written player file
        const std::string tmp = path_ + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc | std::ios::binary);   // binary: lines end in CR LF exactly once
            for (auto& s : sections_) {
                out << "[" << s.name << "]\r\n";
                for (auto& kv : s.keys) out << kv.first << "=" << kv.second << ";\r\n";
                out << "\r\n";
            }
            out.flush();
            if (!out) { std::remove(tmp.c_str()); return; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path_, ec);
        if (ec) {
            // files copied off the CD keep their read-only attribute: clear it and retry
            namespace fs = std::filesystem;
            fs::permissions(path_, fs::perms::owner_write, fs::perm_options::add, ec);
            ec.clear();
            fs::rename(tmp, path_, ec);
            if (ec) { ec.clear(); fs::remove(path_, ec); ec.clear(); fs::rename(tmp, path_, ec); }
            if (ec) {   // last resort: copy over the original in place
                ec.clear();
                fs::copy_file(tmp, path_, fs::copy_options::overwrite_existing, ec);
                fs::remove(tmp, ec);
            }
        }
    }
    Engine* vm_;
    std::string path_, cur_;
    std::vector<Sec> sections_;
};

std::shared_ptr<LBXObject> CreateLBX(Engine* e, int type) {
    if (type == 1001) return std::make_shared<LBXDataFile>(e);
    char b[64];
    std::snprintf(b, sizeof b, "lbxcreate(%d): unknown LBX type", type);
    e->Log(b);
    return nullptr;
}

// ---- pages -------------------------------------------------------------------------------
Page::Page(Engine* e, mohawk::Archive* a, uint16_t baseId, Item* owner) : vm_(e), archive_(a), baseId_(baseId), owner_(owner) {}

Page::~Page() {
    // the items outlive the page until the engine frees them: they must not reach back into it
    // (a retired proxy page is freed an update later: some of its items may be gone by then)
    for (Item* it : items_) if (it && vm_->Owns(it)) { if (!it->Dead()) vm_->Destroy(it); it->DetachPage(); }
}

bool Engine::CreditsRolling() const {
    for (const auto& [k, v] : items_) {
        const Item* it = v.get();
        if (it && !it->Dead() && it->Type() == kItemMovie && it->Name() == "Credits" && it->IsPlaying()) {
            auto f = vars_.find("xbfromgame");
            return f != vars_.end() && !f->second.IsZero();
        }
    }
    return false;
}

bool Engine::Owns(const Item* it) const {
    for (const auto& [k, v] : items_) if (v.get() == it) return true;
    for (const auto& g : graveyard_) if (g.get() == it) return true;
    return false;
}

void Page::Forget(Item* it) { items_.erase(std::remove(items_.begin(), items_.end(), it), items_.end()); }

bool Page::Open() {
    code_ = std::make_unique<Code>(vm_);
    if (const mohawk::Resource* r = archive_->Find(mohawk::Tag("BCOD"), baseId_)) code_->Load(archive_->Read(*r));
    const mohawk::Resource* br = archive_->Find(mohawk::Tag("BITL"), baseId_);
    if (!br) { vm_->Log("page " + archive_->Path() + ": no BITL " + std::to_string(baseId_)); return false; }
    const std::vector<uint8_t> b = archive_->Read(*br);
    const uint8_t* p = b.data();
    const uint8_t* end = b.data() + b.size();
    while (p + 10 <= end) {
        Rect r{S16(p), S16(p + 2), S16(p + 4), S16(p + 6)};
        const uint16_t type = BE16(p + 8);
        p += 10;
        Item* it = nullptr;
        switch (type) {
        case kItemPicture: it = new PictureItem(vm_, this, r, type); break;
        case kItemAnim: it = new AnimItem(vm_, this, r, type); break;
        case kItemSound: it = new SoundItem(vm_, this, r, type); break;
        case kItemGroup: it = new GroupItem(vm_, this, r, type); break;
        case kItemMovie: it = new MovieItem(vm_, this, r, type); break;
        case kItemPalette: it = new PaletteItem(vm_, this, r, type); break;
        case kItemProxy: it = new ProxyItem(vm_, this, r, type); break;
        case kItemVectorMotion: it = new VectorMotionItem(vm_, this, r, type); break;
        default: it = new Item(vm_, this, r, type); break;
        }
        it->ReadFrom(p, end);
        vm_->Register(it);
        items_.push_back(it);
    }
    // BITL order: later items on top. A proxied page is layered where its proxy item sits.
    if (owner_ && owner_->inOrder_) vm_->InsertInFront(items_, owner_);
    else for (Item* it : items_) vm_->AddToOrder(it);
    // load-time code (0x194), then init, then the load phase
    for (Item* it : items_) for (uint32_t off : it->loadCode_) if (!it->Dead()) code_->Run(it, off);
    for (Item* it : std::vector<Item*>(items_)) if (!it->Dead()) it->Init();
    for (Item* it : std::vector<Item*>(items_)) if (!it->Dead()) it->StartPhase(kPhaseLoad);
    return true;
}

// ---- engine ------------------------------------------------------------------------------
Engine::Engine(Host host) : host_(std::move(host)) {}

Engine::~Engine() { ClosePage(); }

void Engine::NotifyUnload(const std::vector<Item*>& pageItems) {
    std::vector<Item*> all(pageItems.begin(), pageItems.end());
    for (size_t k = 0; k < all.size(); ++k)
        if (all[k] && all[k]->Type() == kItemProxy) for (Item* m : all[k]->Members()) all.push_back(m);
    for (Item* it : all) if (it && !it->Dead()) it->Notify(0xfff7, 0xffff);
}

// Item values in variables would dangle once the page's items are freed: they become the
// items' names, which is what the scripts compare them with and how the player file keeps them.
static void Stringify(Engine* e, Value& v, int depth = 0) {
    if (v.type == VT::Item) { v = Value(e->Str(v)); return; }
    if (v.type == VT::List && v.list && depth < 8) for (Value& x : v.list->a) Stringify(e, x, depth + 1);
}

void Engine::ClosePage() {
    if (page_) NotifyUnload(page_->Items());
    for (auto& kv : vars_) Stringify(this, kv.second);
    pageGraveyard_.clear();
    page_.reset();
    // Proxied pages first, while every item is still alive: a sub-page's destructor walks
    // its items, which must not have been freed yet (the item map frees in serial order).
    for (auto& kv : items_)
        if (kv.second && kv.second->Type() == kItemProxy) static_cast<ProxyItem*>(kv.second.get())->ReleaseSubPage();
    for (auto& kv : items_) kv.second->OnDestroyed();
    items_.clear();
    created_.clear();
    order_.clear();
    pendingDestroy_.clear();
    pendingDone_.clear();
    graveyard_.clear();
    archives_.clear();
    namedGroups_.clear();
    keyEvents_.clear();
    timers_.clear();
    autoEvents_.clear();
    pendingRollEnd_.clear();
    focus_ = nullptr;
    drags_.clear();
    LockAll(false);
}

// Between the 60 Hz game updates the screen shows a palette fade where it is at that moment
// (the game's own update still sets the step and finishes the fade on time).
void Engine::SmoothPaletteFades(uint32_t extraMs) {
    if (!extraMs) return;
    for (Item* it : created_)
        if (!it->Dead() && it->Type() == kItemPalette && static_cast<PaletteItem*>(it)->Fading())
            static_cast<PaletteItem*>(it)->ApplyAt(now_ + extraMs);
}

bool Engine::PaletteFading() const {
    // only the fades to black before a cut scene; the colour switches between floors (going up
    // or down the stairs, the basement) are quick and show no running man
    for (Item* it : created_)
        if (!it->Dead() && it->Type() == kItemPalette && it->Name().rfind("FadeToBlack", 0) == 0 &&
            static_cast<const PaletteItem*>(it)->Fading()) return true;
    return false;
}

bool Engine::OpenPage(mohawk::Archive* a, uint16_t baseId, int mode, int page, int subpage) {
    ClosePage();
    curMode_ = mode; curPage_ = page; curSubpage_ = subpage;
    phase_ = kPhaseInit;
    introDone_ = false;
    phaseStep_ = 0;
    if (const mohawk::Resource* r = a->Find(mohawk::Tag("tPAL"), baseId)) {
        if (auto pal = mohawk::ParsePalette(a->Read(*r))) palette_ = *pal;
    }
    archives_.push_back(a);
    page_ = std::make_unique<Page>(this, a, baseId, nullptr);
    return page_->Open();
}

std::unique_ptr<Page> Engine::LoadProxyPage(const std::string& name, Item* owner) {
    const auto& proxies = outline_ ? outline_->proxies : std::map<std::string, port::Outline::Proxy>{};
    auto it = proxies.find(name);
    if (it == proxies.end()) {
        for (auto& kv : proxies) if (Lower(kv.first) == Lower(name)) { it = proxies.find(kv.first); break; }
    }
    if (it == proxies.end()) { Log("proxy '" + name + "' not in OUTLINE"); return nullptr; }
    mohawk::Archive* a = host_.openArchive ? host_.openArchive(it->second.path) : nullptr;
    if (!a) { Log("proxy '" + name + "': can't open " + it->second.path); return nullptr; }
    archives_.push_back(a);
    auto p = std::make_unique<Page>(this, a, it->second.baseId, owner);
    const size_t before = created_.size();
    if (!p->Open()) return p;
    // Catch the new page up with the phases its parent has already been through.
    std::vector<Item*> fresh(created_.begin() + long(before), created_.end());
    const uint16_t done[] = {kPhaseCreate, kPhaseInit, kPhaseIntro, kPhaseMain};
    for (int k = 0; k < 4 && k < phaseStep_; ++k)
        for (Item* x : fresh) if (!x->Dead() && x->GetPage() == p.get()) x->StartPhase(done[k]);
    return p;
}

void Engine::Register(Item* it) {
    it->serial_ = nextSerial_++;
    items_[it->serial_].reset(it);
    created_.push_back(it);
}

void Engine::AddToOrder(Item* it) {
    if (it->inOrder_) order_.erase(it->orderIt_);
    order_.push_front(it);
    it->orderIt_ = order_.begin();
    it->inOrder_ = true;
}

void Engine::InsertInFront(const std::vector<Item*>& items, Item* ref) {
    auto pos = ref->orderIt_;
    for (Item* it : items) {
        if (it->inOrder_) order_.erase(it->orderIt_);
        pos = order_.insert(pos, it);
        it->orderIt_ = pos;
        it->inOrder_ = true;
    }
}

void Engine::RemoveFromOrder(Item* it) {
    if (!it->inOrder_) return;
    order_.erase(it->orderIt_);
    it->inOrder_ = false;
}

static void CollectBlock(Item* it, std::vector<Item*>& out) {
    // A proxy moves together with the items of the page it loaded.
    out.push_back(it);
    if (it->Type() == kItemProxy) for (Item* m : it->Members()) CollectBlock(m, out);
}

void Engine::SetZOrder(Item* it, int where) {
    std::vector<Item*> block;
    CollectBlock(it, block);
    // keep the block's relative order
    std::vector<Item*> ordered;
    for (Item* x : order_) if (std::find(block.begin(), block.end(), x) != block.end()) ordered.push_back(x);
    for (Item* x : ordered) { RemoveFromOrder(x); x->zBack_ = where < 0; }
    if (where < 0) {
        for (Item* x : ordered) { order_.push_back(x); x->orderIt_ = std::prev(order_.end()); x->inOrder_ = true; }
    } else {
        for (auto r = ordered.rbegin(); r != ordered.rend(); ++r) { order_.push_front(*r); (*r)->orderIt_ = order_.begin(); (*r)->inOrder_ = true; }
    }
}

void Engine::SetZBehind(Item* it, Item* ref) {
    // setzorder(item, ref): item goes directly behind ref (the toolbar stays over the episode
    // proxies, a held tool under the arrow cursor, a target zone under the held tool)
    if (it == ref) return;
    std::vector<Item*> block;
    CollectBlock(it, block);
    if (std::find(block.begin(), block.end(), ref) != block.end()) return;
    std::vector<Item*> ordered;
    for (Item* x : order_) if (std::find(block.begin(), block.end(), x) != block.end()) ordered.push_back(x);
    for (Item* x : ordered) { RemoveFromOrder(x); x->zBack_ = false; }
    // just below ref's block: after its bottom-most member (order_ runs front to back)
    std::vector<Item*> refBlock;
    CollectBlock(ref, refBlock);
    auto pos = order_.end();
    for (auto i = order_.begin(); i != order_.end(); ++i)
        if (std::find(refBlock.begin(), refBlock.end(), *i) != refBlock.end()) pos = std::next(i);
    if (pos == order_.end() && std::none_of(order_.begin(), order_.end(), [&](Item* x) { return x == ref; }) && refBlock.size() <= 1) pos = order_.end();
    for (Item* x : ordered) { auto ins = order_.insert(pos, x); x->orderIt_ = ins; x->inOrder_ = true; }
}

Item* Engine::ZPrev(Item* it) {
    if (!it->inOrder_) return nullptr;
    auto n = std::next(it->orderIt_);
    return n == order_.end() ? nullptr : *n;
}

void Engine::Destroy(Item* it) {
    if (!it || it->dead_) return;
    // the pointer is no longer over an item that goes away: its rollover ends (the navigation
    // zones keep "over a zone" flags in variables, which would otherwise stay set after a walk)
    EndRollover(it);
    it->dead_ = true;
    pendingDestroy_.push_back(it);
}

void Engine::FlushRolloverEnds(bool all) {
    std::vector<uint64_t> due;
    bool fired = false;
    for (auto& [serial, t] : pendingRollEnd_) if (all || t <= now_) due.push_back(serial);
    for (uint64_t s : due) {
        pendingRollEnd_.erase(s);
        if (Item* it = ItemBySerial(s); it && it->rolloverInside_) { it->rolloverInside_ = false; it->RunScript(kEvRolloverEnd); fired = true; }
    }
    // the pointer is now over whatever lay beneath: like the original's single rollover item,
    // that item takes the rollover again (its begin puts its own cursor back), back to front
    if (fired && !all) RebeginRollovers();
}

void Engine::RebeginRollovers() {
    for (auto i = order_.rbegin(); i != order_.rend(); ++i) {
        Item* it = *i;
        if (it->Dead() || !it->rolloverInside_ || !it->rollover_ || !it->Loaded() || !it->Enabled()) continue;
        it->RunScript(kEvRolloverBegin);
    }
}

void Engine::EndRollover(Item* it) {
    if (it) pendingRollEnd_.erase(it->Serial());
    if (!it || !it->rolloverInside_) return;
    it->rolloverInside_ = false;
    it->RunScript(kEvRolloverEnd);
}

void Engine::ArmAutoEvent(AutoEvent& a) {
    const uint32_t r = a.delayMax > a.delayMin ? std::uniform_int_distribution<uint32_t>(a.delayMin, a.delayMax)(rng_) : a.delayMin;
    a.due = std::max<uint32_t>(1, now_ + std::max<uint32_t>(1, r));
}

void Engine::AddAutoEvent(AutoEvent a) {
    autoEvents_.erase(std::remove_if(autoEvents_.begin(), autoEvents_.end(), [&](const AutoEvent& x) { return x.item == a.item && x.event == a.event; }), autoEvents_.end());
    if (a.timing != kAutoSync) { a.left = a.loops >= 0xfffe ? 0xffff : std::max<uint16_t>(1, a.loops); ArmAutoEvent(a); }
    autoEvents_.push_back(a);
}

void Engine::AutoEventNotify(Item* it, uint16_t data, uint16_t from) {
    for (AutoEvent& a : autoEvents_) {
        if (a.item != it->Serial() || a.timing != kAutoSync) continue;
        if (uint16_t(a.sync.y) != data || (a.sync.x != 0 && uint16_t(a.sync.x) != from)) continue;
        a.left = a.loops >= 0xfffe ? 0xffff : std::max<uint16_t>(1, a.loops);   // a new round
        ArmAutoEvent(a);
    }
}

void Engine::QueueDone(Item* it) { pendingDone_.push_back(it); }

Item* Engine::Clone(Item* s, uint16_t newId, const std::string& newName) {
    Item* it = nullptr;
    switch (s->Type()) {
    case kItemPicture: it = new PictureItem(this, s->page_, s->rect_, s->type_); static_cast<PictureItem*>(it)->drawMode_ = static_cast<PictureItem*>(s)->drawMode_; break;
    case kItemAnim: it = new AnimItem(this, s->page_, s->rect_, s->type_); break;
    case kItemSound: it = new SoundItem(this, s->page_, s->rect_, s->type_); break;
    case kItemMovie: it = new MovieItem(this, s->page_, s->rect_, s->type_); break;
    case kItemGroup: { auto* g = new GroupItem(this, s->page_, s->rect_, s->type_); g->ids_ = static_cast<GroupItem*>(s)->ids_; it = g; break; }
    case kItemVectorMotion: it = new VectorMotionItem(this, s->page_, s->rect_, s->type_); break;
    default: it = new Item(this, s->page_, s->rect_, s->type_); break;
    }
    it->name_ = newName;
    it->itemId_ = newId;
    it->resourceId_ = s->resourceId_;
    it->drawOffset_ = s->drawOffset_;
    it->loopMode_ = s->loopMode_; it->timingMode_ = s->timingMode_; it->controlMode_ = s->controlMode_; it->soundMode_ = s->soundMode_;
    it->delayMin_ = s->delayMin_; it->delayMax_ = s->delayMax_; it->periodMin_ = s->periodMin_; it->periodMax_ = s->periodMax_;
    it->relocPoint_ = s->relocPoint_; it->rollover_ = s->rollover_; it->doHitTest_ = s->doHitTest_; it->parent_ = s->parent_;
    it->props_ = s->props_;
    it->visible_ = s->visible_; it->globalVisible_ = s->globalVisible_;
    it->globalEnabled_ = s->globalEnabled_; it->isAmbient_ = s->isAmbient_; it->phase_ = s->phase_;
    it->scripts_ = s->scripts_;
    for (ScriptEntry& e : it->scripts_) e.state = 0;
    Register(it);
    if (s->page_) s->page_->Adopt(it);
    // a clone takes its template's place in the drawing order (directly in front of it), so
    // items layered above the template (the sign-in list's click area over its letters) stay above
    if (s->inOrder_) InsertInFront(std::vector<Item*>{it}, s);
    else AddToOrder(it);
    it->Init();
    if (s->loaded_) it->Load();
    it->enabled_ = s->enabled_;
    return it;
}

void Engine::NotifyAll(uint16_t data, uint16_t from) {
    for (Item* it : std::vector<Item*>(created_)) if (!it->Dead()) it->Notify(data, from);
}

void Engine::SetEnableForAll(bool enable, Item* except) {
    // control mode 2: everything else pauses while the item plays
    for (Item* it : created_) {
        if (it == except || it->Dead()) continue;
        it->paused_ = std::max(0, it->paused_ + (enable ? -1 : 1));
    }
}

Value& Engine::Var(const std::string& name) { return vars_[Lower(name)]; }
bool Engine::HasVar(const std::string& name) const { return vars_.count(Lower(name)) != 0; }
void Engine::DeleteVar(const std::string& name) { vars_.erase(Lower(name)); }

Item* Engine::ItemById(uint16_t id) {
    // newest first, so proxies loaded later win (ids are unique per page range anyway)
    for (auto i = created_.rbegin(); i != created_.rend(); ++i) if (!(*i)->Dead() && (*i)->Id() == id) return *i;
    return nullptr;
}
Item* Engine::ItemByName(const std::string& name) {
    const std::string l = Lower(name);
    for (auto i = created_.rbegin(); i != created_.rend(); ++i) if (!(*i)->Dead() && Lower((*i)->Name()) == l) return *i;
    return nullptr;
}
Item* Engine::ItemBySerial(uint64_t s) {
    if (!s) return nullptr;
    auto it = items_.find(s);
    return it == items_.end() || it->second->Dead() ? nullptr : it->second.get();
}
Item* Engine::Resolve(const Value& v) {
    switch (v.type) {
    case VT::Item: return ItemBySerial(v.item);
    case VT::String: return v.s.empty() ? nullptr : ItemByName(v.s);
    case VT::Integer: return v.i > 0 ? ItemById(uint16_t(v.i)) : nullptr;
    default: return nullptr;
    }
}
std::string Engine::Str(const Value& v) {
    if (v.type == VT::Item) { Item* it = ItemBySerial(v.item); return it ? it->Name() : std::string(); }
    return v.ToString();
}
bool Engine::Equal(const Value& a, const Value& b) {
    if (a.type == VT::Item || b.type == VT::Item) {
        if (a.type == VT::Item && b.type == VT::Item) return a.item == b.item;
        const Value& itv = a.type == VT::Item ? a : b;
        const Value& other = a.type == VT::Item ? b : a;
        Item* it = ItemBySerial(itv.item);
        if (other.type == VT::String) return it && Lower(it->Name()) == Lower(other.s);
        if (other.type == VT::Integer) return it ? it->Id() == other.i : other.i == 0;
        return false;
    }
    if (a.type == VT::String && b.type == VT::String) return Lower(a.s) == Lower(b.s);
    if (a.IsNumeric() && b.IsNumeric() && !(a.type == VT::String && b.type == VT::String)) {
        if (a.type == VT::String && a.s.empty()) return false;
        if (b.type == VT::String && b.s.empty()) return false;
        return a.ToReal() == b.ToReal();
    }
    if ((a.type == VT::String) != (b.type == VT::String)) {
        // string vs number: compare as strings ("" vs 0 is false)
        return Lower(Str(a)) == Lower(Str(b));
    }
    if (a.type == VT::Point && b.type == VT::Point) return a.p.x == b.p.x && a.p.y == b.p.y;
    if (a.type == VT::Rect && b.type == VT::Rect) return a.rc == b.rc;
    if (a.type == VT::List && b.type == VT::List) return a.list == b.list;
    return a.ToInt() == b.ToInt();
}

GroupItem* Engine::GroupByName(const std::string& name, bool create) {
    if (Item* it = ItemByName(name); it && it->Type() == kItemGroup) return static_cast<GroupItem*>(it);
    auto f = namedGroups_.find(Lower(name));
    if (f != namedGroups_.end()) return f->second;
    if (!create) return nullptr;
    auto* g = new GroupItem(this, nullptr, Rect{}, kItemGroup);
    g->name_ = name;
    Register(g);
    namedGroups_[Lower(name)] = g;
    return g;
}

// ---- resources ---------------------------------------------------------------------------
std::vector<uint8_t> Engine::GetResource(const mohawk::Archive* preferred, uint32_t tag, uint16_t id, const mohawk::Archive** found) {
    if (preferred) if (const mohawk::Resource* r = preferred->Find(tag, id)) { if (found) *found = preferred; return preferred->Read(*r); }
    for (auto i = archives_.rbegin(); i != archives_.rend(); ++i)
        if (const mohawk::Resource* r = (*i)->Find(tag, id)) { if (found) *found = *i; return (*i)->Read(*r); }
    return {};
}

const Bitmap* Engine::GetBitmap(const mohawk::Archive* preferred, uint16_t id) {
    const mohawk::Archive* a = preferred;
    if (!a || !a->Find(mohawk::Tag("tBMP"), id)) {
        a = nullptr;
        for (auto i = archives_.rbegin(); i != archives_.rend(); ++i) if ((*i)->Find(mohawk::Tag("tBMP"), id)) { a = *i; break; }
        if (!a) return nullptr;
    }
    auto key = std::make_pair(a, id);
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.w ? &it->second : nullptr;
    Bitmap& b = bitmaps_[key];
    if (const mohawk::Resource* r = a->Find(mohawk::Tag("tBMP"), id))
        if (auto img = mohawk::DecodeBitmap(a->Read(*r))) { b.w = img->width; b.h = img->height; b.px = std::move(img->pixels); }
    return b.w ? &b : nullptr;
}

void Engine::SetPalette(int first, const std::vector<std::array<uint8_t, 3>>& colors) {
    for (size_t k = 0; k < colors.size() && first + int(k) < 256; ++k)
        for (int c = 0; c < 3; ++c) palette_.rgb[first + int(k)][c] = colors[k][size_t(c)];
}

std::optional<port::MovieAsset> Engine::MovieAsset(const mohawk::Archive& a, uint16_t id) {
    return host_.movie ? host_.movie(a, id) : std::nullopt;
}

// ---- sound -------------------------------------------------------------------------------
int Engine::PlaySound(Item* source, uint16_t resourceId, const mohawk::Archive* a) {
    const mohawk::Archive* found = nullptr;
    if (a && a->Find(mohawk::Tag("tWAV"), resourceId)) found = a;
    else for (auto i = archives_.rbegin(); i != archives_.rend(); ++i) if ((*i)->Find(mohawk::Tag("tWAV"), resourceId)) { found = *i; break; }
    if (!found || !host_.playSound) return 0;
    if (source) {
        // LBItem::playSound priorities: a lower-priority sound doesn't cut off the current one
        if (lastSoundVoice_ && !SoundPlaying(lastSoundVoice_)) lastSoundVoice_ = 0;
        if (!source->IsAmbient() || !lastSoundVoice_) {
            if (!soundLockOwner_) {
                if (lastSoundVoice_ && lastSoundOwner_ != source->Id() && source->SoundPriority() >= lastSoundPriority_ && lastSoundPriority_)
                    return 0;
            } else if (soundLockOwner_ != source->Id() && source->SoundPriority() >= maxSoundPriority_ && maxSoundPriority_) {
                return 0;
            }
            if (lastSoundVoice_ && !source->IsAmbient()) { /* the original stops it; keep overlapping ambient ones */ }
            lastSoundOwner_ = source->Id();
            lastSoundPriority_ = source->SoundPriority();
        }
    }
    const int v = host_.playSound(*found, resourceId);
    if (source && !source->IsAmbient()) lastSoundVoice_ = v;
    return v;
}
void Engine::StopSound(int voice) { if (voice && host_.stopSound) host_.stopSound(voice); }
void Engine::PauseSound(int voice, bool paused) { if (voice && host_.pauseSound) host_.pauseSound(voice, paused); }
bool Engine::SoundPlaying(int voice) const { return voice && host_.soundPlaying && host_.soundPlaying(voice); }
double Engine::SoundTime(int voice) const { return voice && host_.soundTime ? host_.soundTime(voice) : -2; }
void Engine::LockSound(Item* owner, bool lock) {
    if (!lock) { soundLockOwner_ = 0; return; }
    if (soundLockOwner_) return;
    soundLockOwner_ = owner->Id();
    maxSoundPriority_ = owner->SoundPriority();
}

// ---- world & view ------------------------------------------------------------------------
Point Engine::WrapPoint(Point p) const {
    if (wrap_ && world_.Width() > 0) {
        const int ww = world_.Width();
        p.x = ((p.x - world_.left) % ww + ww) % ww + world_.left;
    }
    return p;
}
void Engine::SetViewOrigin(Point p) {
    // A page that starts somewhere other than the left edge (the chute game sets 1000,0 while
    // it loads) takes its view once its items have run their init events: the side zones
    // anchor themselves to the screen in init, and then move with the view to the screen edges.
    if (phaseStep_ == 0 && (p.x || p.y)) { pendingView_ = p; hasPendingView_ = true; return; }
    if (phaseStep_ == 0) hasPendingView_ = false;
    const Point old = view_;
    view_ = WrapPoint(p);
    int dx = view_.x - old.x, dy = view_.y - old.y;
    if (wrap_ && world_.Width() > 0) { const int ww = world_.Width(); dx = ((dx % ww) + ww + ww / 2) % ww - ww / 2; }
    if (!dx && !dy) return;
    // items anchored to the screen (setparent(-1)) keep their place on screen
    const int ww = (wrap_ && world_.Width() > 0) ? world_.Width() : 0;
    for (Item* it : created_) {
        if (!it || it->dead_ || it->parent_ != Item::kScreenParent) continue;
        if (IsDragged(it)) continue;                 // a dragged item follows the pointer instead
        int mx = dx;
        if (ww) {                                   // stay within one wrap of the view
            const int rel = it->rect_.left + dx - view_.x;
            if (rel > ww / 2 + 640) mx -= ((rel - 640 - ww / 2) / ww + 1) * ww;
            else if (rel < -ww / 2) mx += ((-ww / 2 - rel) / ww + 1) * ww;
        }
        it->MoveBy(mx, dy);
    }
}
Point Engine::MouseWorld() const {
    Point p{mouse_.x + view_.x, mouse_.y + view_.y};
    return WrapPoint(p);
}

// ---- drag --------------------------------------------------------------------------------
void Engine::BeginDrag(Item* it, Point from) {
    // several items can follow the pointer at once (the arrow cursor and a held tool)
    drags_.erase(std::remove_if(drags_.begin(), drags_.end(), [&](const Drag& d) { return d.serial == it->Serial(); }), drags_.end());
    // the grab point is given in the item's authored (BITL) coordinates
    // ... or, for a piece the script has just moved somewhere else (the fishing-picture strips
    // jump to a holder spot first), as a point on the screen: whichever lands on the item
    // (the original tucks the pointer into the piece it picks up).
    const Point moved{from.x + it->rect_.left - it->origRect_.left, from.y + it->rect_.top - it->origRect_.top};
    if (it->rect_.Contains(moved) || !it->rect_.Contains(from)) from = moved;
    drags_.push_back(Drag{it->Serial(), from, Point{from.x - it->rect_.left, from.y - it->rect_.top}});
    DragFollow(it, drags_.back());
    it->RunScript(kEvDragStart);
}

bool Engine::IsDragged(const Item* it) const {
    for (const Drag& d : drags_) if (d.serial == it->Serial()) return true;
    return false;
}

void Engine::FollowPointer(bool moved) {
    for (size_t k = 0; k < drags_.size(); ++k) {
        Item* d = ItemBySerial(drags_[k].serial);
        if (!d || d->Dead()) continue;
        DragFollow(d, drags_[k]);
        if (moved) d->RunScript(kEvDragMove);
    }
    drags_.erase(std::remove_if(drags_.begin(), drags_.end(), [&](const Drag& d) { Item* i = ItemBySerial(d.serial); return !i || i->Dead(); }), drags_.end());
}

void Engine::DragFollow(Item* d, Drag& g) {
    // g.last = the grab point's unclamped world position; put the grab point under the pointer
    const Point w = MouseWorld();
    int dx = w.x - g.last.x, dy = w.y - g.last.y;
    if (wrap_ && world_.Width() > 0) { const int ww = world_.Width(); dx = ((dx % ww) + ww + ww / 2) % ww - ww / 2; }
    g.last = Point{g.last.x + dx, g.last.y + dy};
    // item top-left wanted = grab point - grab offset; clamp; move there
    const Point want{g.last.x - g.grab.x, g.last.y - g.grab.y};
    int mx = want.x - d->rect_.left, my = want.y - d->rect_.top;
    if (wrap_ && world_.Width() > 0) { const int ww = world_.Width(); mx = ((mx % ww) + ww + ww / 2) % ww - ww / 2; }
    if (mx || my) d->MoveBy(mx, my);
    ApplyDragLimit(d);
}

void Engine::ApplyDragLimit(Item* it) {
    auto f = dragLimits_.find(it->Serial());
    if (f == dragLimits_.end()) return;
    const Rect& l = f->second.r;
    // the allowed range of the item's top-left (x is free when the world wraps)
    // (the game's pointer cursors follow the mouse exactly: their limit only keeps the pointer
    // itself on screen, so the arrow still reaches the bottom of the toolbar)
    bool whole = f->second.whole;
    if (whole) if (GroupItem* g = GroupByName("grpCursor", false)) for (Item* m : g->Members()) if (m == it) whole = false;
    const int w = whole ? it->rect_.Width() : 0, h = whole ? it->rect_.Height() : 0;
    const int maxX = std::max(l.left, l.right - w), maxY = std::max(l.top, l.bottom - h);
    int dx = 0, dy = 0;
    if (!wrap_) {
        if (it->rect_.left < l.left) dx = l.left - it->rect_.left;
        else if (it->rect_.left > maxX) dx = maxX - it->rect_.left;
    }
    if (it->rect_.top < l.top) dy = l.top - it->rect_.top;
    else if (it->rect_.top > maxY) dy = maxY - it->rect_.top;
    if (dx || dy) it->MoveBy(dx, dy);       // the pointer keeps its own place; the grab offset stays
}

void Engine::EndDrag(Item* it) {
    if (!it || !IsDragged(it)) return;
    drags_.erase(std::remove_if(drags_.begin(), drags_.end(), [&](const Drag& d) { return d.serial == it->Serial(); }), drags_.end());
    it->RunScript(kEvDragEnd);
}

// ---- input -------------------------------------------------------------------------------
Point Engine::Near(const Item* it, Point w) const {
    // the world wraps: use the copy of the point closest to the item
    if (!wrap_ || world_.Width() <= 0) return w;
    const int ww = world_.Width();
    const Rect r = it->GetRect();
    const int cx = (r.left + r.right) / 2;
    int d = cx - w.x;
    const int k = (d >= 0 ? d + ww / 2 : d - ww / 2) / ww;
    return Point{w.x + k * ww, w.y};
}

Item* Engine::HitTest(Point w) {
    for (Item* it : order_) {
        if (it->Dead()) continue;
        // the dragged cursor takes part: arrow cursors are drawn beside the mouse point, while
        // held tools (Cur*) cover it and act on the zone under them in their own mousedown
        if (it->Contains(Near(it, w))) return it;
    }
    return nullptr;
}

void Engine::MouseMove(int vx, int vy) {
    mouse_ = Point{vx, vy};
    FollowPointer(true);
    RolloverPass(true);
    TrackFocus();
}

// An item switched on under a pointer that isn't moving gets its rollover at once (the
// stacking game's drop regions light up as soon as a piece is picked up over them).
void Engine::RefreshRollover() {
    rolloverDirty_ = false;
    if (!page_) return;
    RolloverPass(false);
}

void Engine::RolloverPass(bool moved) {
    const Point w = MouseWorld();
    // rollovers, front to back: every rollover item under the pointer gets begin/move until
    // one that blocks (objects); zones let the search go on. Items the pointer left get end.
    std::vector<Item*> inside;
    for (Item* it : order_) {
        if (it->Dead() || !it->rollover_ || !it->Loaded() || !it->Enabled()) continue;
        if (IsDragged(it)) continue;
        if (!it->Contains(Near(it, w))) continue;
        inside.push_back(it);
        if (it->rolloverBlocks_) break;
    }
    // Leaving a pass-through zone for no other zone (the thin gaps between the cookie game's
    // drop zones, the edges of the turn zones) waits a moment: crossing such a gap would
    // otherwise flash the arrow between two "no cursor" zones. If the pointer enters another
    // item meanwhile, the end runs first, as usual; if it comes back, nothing happens.
    bool anyBegin = false;
    for (Item* it : inside) if (!it->Dead() && !it->rolloverInside_) anyBegin = true;
    for (Item* it : inside) pendingRollEnd_.erase(it->Serial());
    bool ended = false;
    if (anyBegin) {
        for (auto& [serial, t] : pendingRollEnd_) if (Item* p = ItemBySerial(serial); p && p->rolloverInside_) ended = true;
        FlushRolloverEnds(true);
    }
    for (Item* it : std::vector<Item*>(created_)) {
        if (it->Dead()) continue;
        if (it->rolloverInside_ && std::find(inside.begin(), inside.end(), it) == inside.end()) {
            if (!it->rolloverBlocks_ && !anyBegin) { pendingRollEnd_.emplace(it->Serial(), now_ + 150); continue; }
            pendingRollEnd_.erase(it->Serial());
            it->rolloverInside_ = false; it->RunScript(kEvRolloverEnd);
            ended = true;
        }
    }
    // back to front, so the frontmost item's begin (its cursor) has the last word; an item the
    // pointer was already over takes the rollover again when one above it has just ended,
    // as it would with the original's single rollover item
    for (auto i = inside.rbegin(); i != inside.rend(); ++i) {
        Item* it = *i;
        if (it->Dead()) continue;
        if (!it->rolloverInside_) { it->rolloverInside_ = true; it->RunScript(kEvRolloverBegin); }
        else if (ended) it->RunScript(kEvRolloverBegin);
        else if (moved) it->RunScript(kEvRolloverMove);
    }
}

void Engine::TrackFocus() {
    const Point w = MouseWorld();
    if (focus_ && !focus_->Dead()) {
        // while the button is held: track in/out as the pointer leaves or re-enters the item
        Item* f = focus_;
        const bool inside = f->Contains(Near(f, w));
        if (inside != focusInside_) { focusInside_ = inside; f->RunScript(inside ? kEvMouseTrackIn : kEvMouseTrackOut); }
        if (focus_ == f && !f->Dead()) {
            f->RunScript(kEvMouseTrackMove);
            if (focus_ == f && !f->Dead()) f->RunScript(inside ? kEvMouseTrackMoveIn : kEvMouseTrackMoveOut);
        }
    }
}

void Engine::MouseDown(int vx, int vy) {
    mouse_ = Point{vx, vy};
    const Point w = MouseWorld();
    // like the original player: no clicks while the game has hidden the cursor (its own
    // cursor items - the arrow and a held tool - count as the cursor, unless the game has put
    // them out of sight behind the room with setzorder(-1), as it does while a baby talks)
    bool cursorItem = false;
    for (const Drag& d : drags_)
        if (Item* c = ItemBySerial(d.serial); c && c->Loaded() && c->Visible() && !c->zBack_) cursorItem = true;
    if (!cursorShown_ && !cursorItem) { if (DevEnv("LBDEBUG")) Log("mousedown ignored (cursor hidden)"); return; }
    Item* it = HitTest(w);
    // Cabinet Climb: while Tommy warns that Didi is coming (TomWarn1-3) the drawers don't take
    // clicks - a locked drawer clicked meanwhile would start his "it's locked" line on top of
    // the warning (two Tommys)
    if (it && (it->Name().rfind("Drw", 0) == 0 || it->Name().rfind("QStart", 0) == 0)) {
        for (Item* x : created_)
            if (!x->Dead() && x->Name().rfind("TomWarn", 0) == 0 && x->IsPlaying()) {
                if (DevEnv("LBDEBUG")) Log("mousedown on " + it->Name() + " ignored (Tommy is warning)");
                return;
            }
    }
    if (DevEnv("LBDEBUG")) {
        char b[160];
        std::snprintf(b, sizeof b, "mousedown at world %d,%d -> %s (enabled %d loaded %d)", w.x, w.y,
                      it ? it->Name().c_str() : "nothing", it ? it->Enabled() : 0, it ? it->Loaded() : 0);
        Log(b);
    }
    if (it) it->HandleMouseDown(Near(it, w));
}

void Engine::MouseUp(int vx, int vy) {
    mouse_ = Point{vx, vy};
    if (focus_ && !focus_->Dead()) focus_->HandleMouseUp(Near(focus_, MouseWorld()));
    focus_ = nullptr;
}

void Engine::Key(int code, int mods, int ch) {
    for (const KeyEvent& k : std::vector<KeyEvent>(keyEvents_)) {
        const bool match = (k.key & 0x10000) ? (ch && std::tolower(ch) == std::tolower(k.key & 0xff) && (k.key & 0xff) == ch)
                                             : (k.key == code && k.mods == mods);
        if (!match) continue;
        if (Item* it = ItemBySerial(k.item)) it->RunScript(k.event);
    }
}

// ---- update ------------------------------------------------------------------------------
void Engine::RunPhases() {
    // ScummVM LB updatePage: init (create + init), intro, then main once the intro is done.
    auto all = [&](uint16_t ph) { for (Item* it : std::vector<Item*>(created_)) if (!it->Dead()) it->StartPhase(ph); };
    switch (phaseStep_) {
    case 0: phaseStep_ = 1; all(kPhaseCreate); phaseStep_ = 2; all(kPhaseInit);
        if (hasPendingView_) { hasPendingView_ = false; SetViewOrigin(pendingView_); }
        break;
    case 2: phaseStep_ = 3; all(kPhaseIntro); break;
    // the main phase starts once the page is fully up from black (reference: the control
    // room's Chuckie pops out of the trash can as the fade-in ends, not during it)
    case 3: if (introDone_ && !holdVoices_) { phaseStep_ = 4; all(kPhaseMain); } break;
    default: break;
    }
}

// While a new page fades in: its setup runs (create, init and intro - what the first picture
// shows: the sign-in list, which buttons are up) but nothing plays yet (no timers, animation
// frames, movie clocks or automatic starts); the page comes alive once it is fully up.
void Engine::UpdateSetup() {
    for (int k = 0; k < 3 && phaseStep_ < 3; ++k) RunPhases();
    std::vector<NotifyEvent> ns;
    ns.swap(notifies_);
    for (const NotifyEvent& n : ns) {
        if (n.kind == NotifyEvent::kIntroDone) { if (n.param == 1) introDone_ = true; continue; }
        if (n.kind == NotifyEvent::kShowCursor) { cursorShown_ = n.param != 0; continue; }
        if (host_.notify) host_.notify(n);
    }
}

void Engine::Update(uint32_t nowMs) {
    pageGraveyard_.clear();
    const uint32_t dt = now_ ? nowMs - now_ : 0;
    now_ = nowMs;
    RunPhases();
    if (rolloverDirty_ && phaseStep_ >= 4) RefreshRollover();
    // the dragged item (the cursor) stays under the pointer when scripts move the view
    FollowPointer(false);
    if (!pendingRollEnd_.empty()) FlushRolloverEnds(false);

    for (Item* it : std::vector<Item*>(pendingDone_)) if (!it->Dead()) it->Done(true);
    pendingDone_.clear();
    for (Item* it : std::vector<Item*>(pendingDestroy_)) {
        it->OnDestroyed();
        RemoveFromOrder(it);
        if (Page* p = it->GetPage()) p->Forget(it);
        created_.erase(std::remove(created_.begin(), created_.end(), it), created_.end());
        if (focus_ == it) focus_ = nullptr;
        auto f = items_.find(it->Serial());
        if (f != items_.end()) { graveyard_.push_back(std::move(f->second)); items_.erase(f); }
    }
    pendingDestroy_.clear();
    if (graveyard_.size() > 64) graveyard_.erase(graveyard_.begin(), graveyard_.begin() + 32);

    // movie clocks
    for (Item* it : created_) {
        if (it->Dead() || it->Type() != kItemMovie) continue;
        if (port::VideoPlayer* v = static_cast<MovieItem*>(it)->Player()) {
            // Keep the picture with its soundtrack (QuickTime plays them on one clock), without
            // ever stepping the picture backwards: it waits for a sound that hasn't started
            // yet (the device takes a moment to begin a new voice) and holds while it is
            // ahead; only a picture that has fallen behind jumps (forwards) to the sound.
            auto* mi = static_cast<MovieItem*>(it);
            const int voice = mi->Voice();
            const double st = voice ? SoundTime(voice) : -2;
            const double vt = v->Time() / v->Asset().timeScale;
            const double step = dt / 1000.0;
            if (DevEnv("MOVDBG")) { char b[128]; std::snprintf(b, sizeof b, "movsync %s vt=%.3f st=%.3f", it->Name().c_str(), vt, st); Log(b); }
            if (!v->Playing() || st <= -1) { v->Update(step); mi->syncWait_ = 0; }
            else if (st == 0) {                               // queued, not playing yet
                mi->syncWait_ += dt;
                if (mi->syncWait_ > 400) v->Update(step);      // a sound that never starts: go on
            } else if (vt - st > 0.5) v->SyncTo(st);          // far ahead (a stall): rejoin it
            else if (vt - st > 0.07) { /* ahead: wait for the sound (it moves in ~46 ms mixer chunks) */ }
            else if (st - vt > 0.08) v->SyncTo(st);           // behind: catch up
            else v->Update(step);
        }
    }
    // autoevents (each can run its item's event again while running it)
    for (size_t k = 0; k < autoEvents_.size(); ++k) {
        if (!autoEvents_[k].due || autoEvents_[k].due > now_) continue;
        Item* it = ItemBySerial(autoEvents_[k].item);
        if (!it) { autoEvents_[k].due = 0; continue; }
        const uint16_t ev = autoEvents_[k].event;
        if (autoEvents_[k].left != 0xffff && autoEvents_[k].left > 0) --autoEvents_[k].left;
        if (autoEvents_[k].left > 0) ArmAutoEvent(autoEvents_[k]); else autoEvents_[k].due = 0;
        it->RunScript(ev, 0, it->Id());
    }
    // timers (sendsync)
    for (size_t k = 0; k < timers_.size();) {
        if (timers_[k].due <= now_) {
            Timer t = timers_[k];
            timers_.erase(timers_.begin() + long(k));
            if (Item* it = ItemBySerial(t.item)) {
                if (t.notify) it->Notify(t.data, it->Id());
                else it->RunScript(t.event, t.data, it->Id());
            }
        } else ++k;
    }
    collectAnimDone_ = true;
    for (Item* it : std::vector<Item*>(created_)) if (!it->Dead()) it->Update();
    collectAnimDone_ = false;
    if (!animDone_.empty()) {
        std::vector<Item*> fin; fin.swap(animDone_);
        for (auto r = fin.rbegin(); r != fin.rend(); ++r) if (!(*r)->Dead()) (*r)->Done(true);
    }
    if (DevEnv("TOOLCHK")) {
        // debugging: Tommy holds a tool, yet it isn't on the pointer
        static std::string last;
        std::string now;
        const std::string tool = HasVar("xvTool") ? Str(Var("xvTool")) : std::string();
        if (cursorShown_ && !tool.empty() && tool != "NONE") {
            Item* t = ItemByName(tool);
            if (!t) now = tool + " missing";
            else if (!t->Loaded() || !t->Visible() || !IsDragged(t) || t->zBack_)
                now = tool + (t->Loaded() ? "" : " unloaded") + (t->Visible() ? "" : " hidden") + (IsDragged(t) ? "" : " not-dragged") + (t->zBack_ ? " behind" : "");
        }
        if (now != last) { Log("TOOLCHK t=" + std::to_string(now_) + " " + (now.empty() ? std::string("ok ") + tool : now)); last = now; }
    }
    if (DevEnv("LBDUP")) {
        // debugging: two drawings of one baby on screen at once (home picture + an animation)
        static std::string last;
        std::string now;
        for (Item* a : created_) {
            if (a->Dead() || !a->Loaded() || !a->Visible() || a->Name().size() < 4) continue;
            static const std::map<std::string, std::string> homes{{"ChuckA", "Chuck"}, {"PhilA", "Phil"}, {"LilA", "Lil"}, {"TommyA", "Tom"}, {"AngChewA", "AngChew"}, {"AngBookA", "AngBook"}};
            auto h = homes.find(a->Name());
            if (h == homes.end()) continue;
            const std::string pre = h->second;
            for (Item* b : created_) {
                if (b == a || b->Dead() || !b->Loaded() || !b->Visible() || b->Type() != kItemAnim) continue;
                if (b->Name() == "TomToInv" || b->Name().find("Tommy") == 0) continue;
                if (b->Name().compare(0, pre.size(), pre) != 0) continue;
                if (b->FrameRect().Width() <= 0) continue;
                if (auto* an = dynamic_cast<AnimItem*>(b); !an || !(an->IsPlaying() || an->Drawn())) continue;
                now += a->Name() + "+" + b->Name() + (static_cast<AnimItem*>(b)->IsPlaying() ? "" : "(frozen)") + " ";
            }
        }
        // a baby's home animation shown with no picture, and nothing else of that baby drawn
        for (Item* a : created_) {
            if (a->Dead() || !a->Loaded() || !a->Visible() || a->Type() != kItemAnim) continue;
            static const std::map<std::string, std::string> homes2{{"ChuckA", "Chuck"}, {"PhilA", "Phil"}, {"LilA", "Lil"}, {"TommyA", "Tom"}};
            auto h = homes2.find(a->Name());
            if (h == homes2.end() || static_cast<AnimItem*>(a)->Drawn()) continue;
            bool other = false;
            for (Item* b : created_)
                if (b != a && !b->Dead() && b->Loaded() && b->Visible() && b->Type() == kItemAnim &&
                    b->Name().compare(0, h->second.size(), h->second) == 0 && static_cast<AnimItem*>(b)->Drawn()) other = true;
            if (!other) now += a->Name() + "(gone) ";
        }
        // and two animations of one baby playing at once (Lil1 + LilSpike2)
        for (const char* pre : {"Lil", "Phil", "Chuck", "Ang", "Tom"}) {
            std::vector<Item*> on;
            for (Item* b : created_) {
                if (b->Dead() || !b->Loaded() || !b->Visible() || b->Type() != kItemAnim) continue;
                if (b->Name().compare(0, std::strlen(pre), pre) != 0 || b->FrameRect().Width() <= 0) continue;
                if (auto* an = dynamic_cast<AnimItem*>(b); !an || !an->IsPlaying()) continue;
                on.push_back(b);
            }
            if (on.size() > 1) { for (Item* b : on) now += b->Name() + "&"; now += " "; }
        }
        if (now != last) { if (!now.empty()) { char t[32]; std::snprintf(t, sizeof t, "t=%u ", now_); Log(std::string("DUP ") + t + now); } last = now; }
    }

    // notifications
    std::vector<NotifyEvent> ns;
    ns.swap(notifies_);
    for (const NotifyEvent& n : ns) {
        if (n.kind == NotifyEvent::kIntroDone) { if (n.param == 1) introDone_ = true; continue; }
        if (n.kind == NotifyEvent::kShowCursor) { cursorShown_ = n.param != 0; continue; }
        if (host_.notify) host_.notify(n);
    }
}

void Engine::DumpState() {
    std::printf("== engine: phase step %d, intro done %d, view %d,%d, world %d..%d wrap %d, %zu items\n", phaseStep_, introDone_,
                view_.x, view_.y, world_.left, world_.right, wrap_, created_.size());
    int shown = 0;
    for (auto i = order_.begin(); i != order_.end(); ++i) {
        Item* it = *i;
        if (it->Dead() || !it->Loaded()) continue;
        std::printf("  %-5u %-22s type %02x loaded %d vis %d/%d en %d/%d/%d play %d rect %d,%d,%d,%d off %d,%d\n", it->Id(), it->Name().c_str(), it->Type(),
                    it->loaded_, it->visible_, it->globalVisible_, it->enabled_, it->globalEnabled_, it->paused_, it->playing_,
                    it->rect_.left, it->rect_.top, it->rect_.right, it->rect_.bottom, it->drawOffset_.x, it->drawOffset_.y);
        if (auto* a = dynamic_cast<AnimItem*>(it)) if (a->Visible()) std::printf("        %s\n", a->DebugCels().c_str());
        if (++shown > 400) break;
    }
    std::printf("== vars:");
    for (auto& kv : vars_) std::printf(" %s=%s", kv.first.c_str(), Str(kv.second).c_str());
    std::printf("\n");
}

// ---- drawing -----------------------------------------------------------------------------
void Engine::Render(Canvas& c) {
    // back to front. Wide pictures (panoramas) may fill the widescreen bands; everything
    // else is clipped to the original 640x480 frame at buffer x = view.x - originX.
    // smoothDx_: the host draws the room a fraction of a turn step behind the view (smooth
    // turns); things fixed on the screen (toolbar, cursor, zoom movies) stay where they are.
    const SDL_Rect full = c.clip;
    const int baseOrigin = c.originX;
    const int fx = view_.x - baseOrigin;
    const SDL_Rect frame{std::max(full.x, fx), full.y, std::min(640, full.w), full.h};
    for (auto i = order_.rbegin(); i != order_.rend(); ++i) {
        Item* it = *i;
        if (it->Dead() || !it->Loaded() || !it->Visible()) continue;
        if (hideCursorItems_ && IsDragged(it)) continue;   // the busy cursor stands in for the game's cursor
        const bool wide = it->Type() == kItemPicture && it->GetRect().Width() >= 640;
        c.clip = wide ? full : frame;
        c.originX = baseOrigin + (smoothDx_ && !OnScreenLayer(it) ? smoothDx_ : 0);
        it->Draw(c);
    }
    c.originX = baseOrigin;
    c.clip = full;
}

bool Engine::OnScreenLayer(const Item* it) const {
    // anchored to the screen (setparent(-1)), following the pointer, or parented to such an item
    for (int depth = 0; it && depth < 8; ++depth) {
        if (it->parent_ == Item::kScreenParent || IsDragged(it)) return true;
        if (!it->parent_) return false;
        auto f = items_.find(it->parent_);
        it = f == items_.end() ? nullptr : f->second.get();
    }
    return false;
}

std::vector<Engine::MovieDraw> Engine::Movies(bool withEnded) const {
    std::vector<MovieDraw> v;
    for (auto i = order_.rbegin(); i != order_.rend(); ++i) {
        Item* it = *i;
        if (it->Dead() || it->Type() != kItemMovie || !it->Visible()) continue;
        port::VideoPlayer* p = static_cast<MovieItem*>(it)->Player();
        if (!p) continue;
        if (static_cast<MovieItem*>(it)->Ended() && (!withEnded || !it->Loaded())) continue;
        // movies are drawn on the screen: world rect -> view coordinates (nearest wrap copy)
        Rect r = it->GetRect();
        int dx = -view_.x;
        if (wrap_ && world_.Width() > 0) {
            const int ww = world_.Width();
            int rel = r.left - view_.x;
            rel = ((rel % ww) + ww + ww / 2) % ww - ww / 2;
            dx = rel - r.left;
        }
        r.Translate(dx, -view_.y);
        v.push_back(MovieDraw{p, r});
    }
    return v;
}

} // namespace lb
