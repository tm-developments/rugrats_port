// lb_value.h - values of the Living Books v3 script language (Mohawk Player 3.1).
//
// The engine this ports (the original RUGRATS.EXE) keeps every script variable as a tagged value: string,
// integer, real, point, rect, item reference, LBX object or list. Item references are held
// by serial number so a value never points at an item that has been destroyed (proxies
// unload whole pages, and scripts keep item references in globals like xvCurZoom).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lb {

struct Point { int x = 0, y = 0; };

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Contains(Point p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
    bool Intersects(const Rect& o) const { return left < o.right && o.left < right && top < o.bottom && o.top < bottom; }
    void Translate(int dx, int dy) { left += dx; right += dx; top += dy; bottom += dy; }
    void MoveTo(int x, int y) { Translate(x - left, y - top); }
    bool Empty() const { return right <= left || bottom <= top; }
    bool operator==(const Rect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
};

class Item;
class LBXObject;
struct List;

enum class VT { String, Integer, Real, Point, Rect, Item, LBX, List };

struct Value {
    VT type = VT::Integer;
    int i = 0;
    double r = 0;
    std::string s;
    Point p;
    Rect rc;
    uint64_t item = 0;                      // item serial (see Engine::Resolve)
    std::shared_ptr<LBXObject> lbx;
    std::shared_ptr<List> list;

    Value() = default;
    Value(int v) : type(VT::Integer), i(v) {}
    Value(bool v) : type(VT::Integer), i(v ? 1 : 0) {}
    Value(double v) : type(VT::Real), r(v) {}
    Value(const std::string& v) : type(VT::String), s(v) {}
    Value(const char* v) : type(VT::String), s(v) {}
    Value(Point v) : type(VT::Point), p(v) {}
    Value(Rect v) : type(VT::Rect), rc(v) {}
    static Value Item(uint64_t serial) { Value v; v.type = VT::Item; v.item = serial; return v; }
    static Value Lbx(std::shared_ptr<LBXObject> o) { Value v; v.type = VT::LBX; v.lbx = std::move(o); return v; }
    static Value MakeList(std::shared_ptr<List> l) { Value v; v.type = VT::List; v.list = std::move(l); return v; }

    bool IsNumeric() const;
    int ToInt() const;
    double ToReal() const;
    std::string ToString() const;           // items print as their name (needs the engine: see Engine::Str)
    Point ToPoint() const;
    Rect ToRect() const;
    bool IsZero() const;                    // false in a condition
};

struct List { std::vector<Value> a; };

// LBX: native extension objects created by lbxcreate(type) and driven by lbxfunc(obj, call, ...).
class LBXObject {
public:
    virtual ~LBXObject() = default;
    // Returns true when the call produces a value (stored in result).
    virtual bool Call(int callId, const std::vector<Value>& params, Value& result) = 0;
};

} // namespace lb
