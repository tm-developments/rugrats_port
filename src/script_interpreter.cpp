#include "script_interpreter.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mohawk {
namespace {
uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
int16_t S16(const uint8_t* p) { return int16_t(BE16(p)); }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
template <class T> std::optional<T> Fail(std::string* e, const std::string& m) { if (e) *e = m; return std::nullopt; }
} // namespace

const ScriptObject* ObjectTable::Find(const std::string& name) const {
    auto it = byName.find(name);
    return it == byName.end() ? nullptr : &objects[it->second];
}

std::optional<SDL_Rect> ObjectTable::WorldRect(const ScriptObject& o) const {
    auto v = viewOf.find(o.objectId);
    if (v == viewOf.end()) return std::nullopt;
    auto origin = viewOrigin.find(v->second);
    if (origin == viewOrigin.end()) return std::nullopt;
    return SDL_Rect{origin->second + o.rect.left, o.rect.top, o.rect.right - o.rect.left, o.rect.bottom - o.rect.top};
}

std::optional<ObjectTable> ParseBITL(const std::vector<uint8_t>& d, std::string* err) {
    ObjectTable t;
    size_t p = 0;
    while (p < d.size()) {
        if (p + 16 > d.size()) return Fail<ObjectTable>(err, "BITL object header runs past the end");
        ScriptObject o;
        o.rect = Rect16{S16(&d[p]), S16(&d[p + 2]), S16(&d[p + 4]), S16(&d[p + 6])};
        o.type = BE16(&d[p + 8]);
        o.resourceId = BE16(&d[p + 10]);
        o.objectId = BE16(&d[p + 12]);
        const size_t codeLen = BE16(&d[p + 14]);
        const auto nameEnd = std::find(d.begin() + long(p) + 16, d.end(), uint8_t(0));
        if (nameEnd == d.end()) return Fail<ObjectTable>(err, "BITL name is not terminated");
        o.name.assign(d.begin() + long(p) + 16, nameEnd);
        const size_t code = size_t(nameEnd - d.begin()) + 1;
        if (code + codeLen > d.size()) return Fail<ObjectTable>(err, "BITL code runs past the end (" + o.name + ")");
        o.code.assign(d.begin() + long(code), d.begin() + long(code + codeLen));
        p = code + codeLen;
        t.byName.emplace(o.name, t.objects.size());
        t.byId.emplace(o.objectId, t.objects.size());
        t.objects.push_back(std::move(o));
    }
    for (const ScriptObject& o : t.objects) {
        // the panorama views: rect.left = -origin
        if (o.type == kObjPatch && o.name.rfind("View", 0) == 0) t.viewOrigin[o.name] = -o.rect.left;
        // grpViewN: 0x0064, byteLength, count, count x {0x0000, objectId}
        if (o.type == kObjGroup && o.name.rfind("grp", 0) == 0 && o.code.size() >= 6 && BE16(o.code.data()) == 0x0064) {
            const size_t n = BE16(o.code.data() + 4);
            for (size_t i = 0; i < n && 6 + i * 4 + 4 <= o.code.size(); ++i)
                t.viewOf[BE16(o.code.data() + 6 + i * 4 + 2)] = o.name.substr(3);   // "grpView4" -> "View4"
        }
    }
    return t;
}

std::optional<BytecodeBlock> ParseBCOD(const std::vector<uint8_t>& d, std::string* err) {
    if (d.size() < 8) return Fail<BytecodeBlock>(err, "BCOD shorter than its header");
    BytecodeBlock b;
    b.totalLength = BE32(d.data());
    b.splitOffset = BE32(d.data() + 4);
    if (b.totalLength != d.size() || b.splitOffset > d.size())
        return Fail<BytecodeBlock>(err, "BCOD header does not match its size");
    b.bytes = d;
    return b;
}

bool ScriptRuntime::Load(const Archive& a, std::string* err) {
    table_ = ObjectTable{};
    bcod_.reset();
    ticks_ = 0;
    auto lists = a.OfType(Tag("BITL"));
    if (lists.empty()) { if (err) *err = "no BITL"; return false; }
    auto t = ParseBITL(a.Read(*lists.front()), err);
    if (!t) return false;
    table_ = std::move(*t);
    auto code = a.OfType(Tag("BCOD"));
    if (!code.empty()) bcod_ = ParseBCOD(a.Read(*code.front()), err);
    return true;
}

std::string ScriptRuntime::ActiveView(double scrollX, int worldWidth) const {
    std::string best;
    double bestDist = 1e9;
    for (const auto& [name, origin] : table_.viewOrigin) {
        double d = std::fabs(std::fmod(scrollX - origin + 1.5 * worldWidth, double(worldWidth)) - 0.5 * worldWidth);
        if (d < bestDist) { bestDist = d; best = name; }
    }
    return best;
}

const ScriptObject* ScriptRuntime::HitTest(int wx, int wy, const std::string& view, int worldWidth) const {
    const ScriptObject* hit = nullptr;
    int bestArea = 1 << 30;
    for (const ScriptObject& o : table_.objects) {
        if (o.type != kObjHotspot) continue;
        auto v = table_.viewOf.find(o.objectId);
        if (v == table_.viewOf.end() || v->second != view) continue;   // only the view on screen
        auto r = table_.WorldRect(o);
        if (!r) continue;
        const int dx = ((wx - r->x) % worldWidth + worldWidth) % worldWidth;     // wrap-aware
        if (dx < r->w && wy >= r->y && wy < r->y + r->h && r->w * r->h < bestArea) {
            bestArea = r->w * r->h;                                     // smallest zone wins
            hit = &o;
        }
    }
    return hit;
}

const ScriptObject* ScriptRuntime::Click(int wx, int wy, const std::string& view, int worldWidth) {
    const ScriptObject* o = HitTest(wx, wy, view, worldWidth);
    if (o && handler_) handler_(*o, ScriptEvent::Click);
    return o;
}

} // namespace mohawk
