#include "devenv.h"
#include "lb/lb_code.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "lb/lb_engine.h"

namespace lb {

// ---- values ----------------------------------------------------------------------------
bool Value::IsNumeric() const {
    if (type == VT::Integer || type == VT::Real) return true;
    if (type != VT::String || s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && *end == 0;
}
int Value::ToInt() const {
    switch (type) {
    case VT::Integer: return i;
    case VT::Real: return int(r);
    case VT::String: return IsNumeric() ? int(std::strtod(s.c_str(), nullptr)) : 0;
    case VT::Item: return item ? 1 : 0;
    default: return 0;
    }
}
double Value::ToReal() const {
    if (type == VT::Real) return r;
    if (type == VT::String && IsNumeric()) return std::strtod(s.c_str(), nullptr);
    return ToInt();
}
std::string Value::ToString() const {
    char b[64];
    switch (type) {
    case VT::String: return s;
    case VT::Integer: std::snprintf(b, sizeof b, "%d", i); return b;
    case VT::Real: std::snprintf(b, sizeof b, "%g", r); return b;
    case VT::Point: std::snprintf(b, sizeof b, "%d, %d", p.x, p.y); return b;
    case VT::Rect: std::snprintf(b, sizeof b, "%d, %d, %d, %d", rc.left, rc.top, rc.right, rc.bottom); return b;
    default: return s;
    }
}
Point Value::ToPoint() const {
    switch (type) {
    case VT::String: { Point q; std::sscanf(s.c_str(), " %d , %d", &q.x, &q.y); return q; }
    case VT::Integer: return Point{i, i};
    case VT::Real: return Point{int(r), int(r)};
    case VT::Point: return p;
    case VT::Rect: return Point{rc.left, rc.top};
    default: return Point{};
    }
}
Rect Value::ToRect() const {
    switch (type) {
    case VT::String: { Rect q; std::sscanf(s.c_str(), " %d , %d , %d , %d", &q.left, &q.top, &q.right, &q.bottom); return q; }
    case VT::Integer: return Rect{i, i, i, i};
    case VT::Point: return Rect{p.x, p.y, p.x, p.y};
    case VT::Rect: return rc;
    default: return Rect{};
    }
}
bool Value::IsZero() const {
    switch (type) {
    case VT::Integer: return i == 0;
    case VT::Real: return r == 0;
    case VT::String: return s.empty() || (IsNumeric() && std::strtod(s.c_str(), nullptr) == 0);
    case VT::Item: return item == 0;
    case VT::List: return !list;
    case VT::LBX: return !lbx;
    default: return false;
    }
}

// ---- tokens -----------------------------------------------------------------------------
enum Tok : uint8_t {
    tIdent = 0x01, tNull = 0x04, tLiteral = 0x05, tString = 0x06, tEOS = 0x07, tEOF = 0x08, tConcat = 0x0b,
    tMul = 0x0e, tOpen = 0x0f, tClose = 0x10, tMinus = 0x11, tMinusMinus = 0x12, tPlusEq = 0x13, tPlus = 0x14,
    tPlusPlus = 0x15, tEq = 0x16, tMinusEq = 0x17, tMulEq = 0x18, tDivEq = 0x19, tListStart = 0x1a, tListEnd = 0x1b,
    tLess = 0x1d, tGreater = 0x1e, tAndEq = 0x1f, tDot = 0x20, tDiv = 0x21, tAssign = 0x22, tLessEq = 0x23,
    tGreaterEq = 0x24, tNotEq = 0x25, tAnd = 0x2a, tComma = 0x2c, tConstMode = 0x31, tIntDiv = 0x32, tMod = 0x34,
    tNot = 0x35, tOr = 0x37, tTrue = 0x39, tFalse = 0x3a, tConstEvent = 0x42, tEval = 0x4b, tGeneral = 0x4d,
    tItemCmd = 0x4e, tNotifyCmd = 0x4f, tUnk5e = 0x5e, tKeycode = 0x5f
};

static std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }

bool Code::Load(const std::vector<uint8_t>& b) {
    if (b.size() < 8) return false;
    const uint32_t size = uint32_t(b[4]) << 24 | uint32_t(b[5]) << 16 | uint32_t(b[6]) << 8 | b[7];
    if (8 + size > b.size()) return false;
    code_.assign(b.begin() + 8, b.begin() + 8 + size);
    size_t p = 8 + size;
    uint16_t pos = 0;
    while (p + 2 < b.size()) {
        p += 2;                                       // u16 0
        size_t e = p;
        while (e < b.size() && b[e]) ++e;
        std::string s(reinterpret_cast<const char*>(b.data() + p), e - p);
        strings_[pos] = s;
        pos = uint16_t(pos + 2 + s.size() + 1);
        p = e + 1;
    }
    return true;
}

uint16_t Code::AddString(const std::string& s) {
    uint16_t id = 0xf000;
    while (strings_.count(id)) ++id;
    strings_[id] = s;
    return id;
}

// ---- interpreter ---------------------------------------------------------------------------
namespace {

struct Interp {
    Engine* vm;
    Code* code;
    Item* src;
    size_t pos;
    uint8_t tok = 0;
    Value val;
    std::vector<Value> stack;
    int depth = 0;

    [[noreturn]] void Fail(const char* what) {
        char b[160];
        std::snprintf(b, sizeof b, "script error at %zu (token %02x): %s", pos, tok, what);
        throw std::runtime_error(b);
    }
    uint16_t U16() {
        if (pos + 2 > code->code_.size()) Fail("ran off the end");
        uint16_t v = uint16_t(code->code_[pos] << 8 | code->code_[pos + 1]);
        pos += 2;
        return v;
    }
    void Next() {
        if (pos >= code->code_.size()) { tok = tEOF; return; }
        tok = code->code_[pos++];
        switch (tok) {
        case tIdent: case tString: {
            uint16_t off = U16();
            auto it = code->strings_.find(off);
            val = Value(it == code->strings_.end() ? std::string() : it->second);
            break;
        }
        case tLiteral: {
            if (pos >= code->code_.size()) Fail("literal");
            uint8_t lt = code->code_[pos++];
            if (lt == 0x02) {                          // real: 32-bit value in ten-thousandths (0.9 = 9000)
                if (pos + 4 > code->code_.size()) Fail("literal");
                const int32_t raw = int32_t(uint32_t(code->code_[pos]) << 24 | uint32_t(code->code_[pos + 1]) << 16 |
                                            uint32_t(code->code_[pos + 2]) << 8 | code->code_[pos + 3]);
                pos += 4;
                val = Value(double(raw) / 10000.0);
                break;
            }
            if (pos + 2 > code->code_.size()) Fail("literal");
            uint16_t raw = lt == 0x11 ? uint16_t(code->code_[pos] | code->code_[pos + 1] << 8)
                                      : uint16_t(code->code_[pos] << 8 | code->code_[pos + 1]);
            pos += 2;
            val = Value(int(raw));                     // unsigned 16-bit (negatives use prefix minus)
            break;
        }
        case tConstMode: case tConstEvent: case tUnk5e: case tKeycode:
            val = Value(int(U16()));
            break;
        case tGeneral: case tItemCmd: case tNotifyCmd:
            if (pos >= code->code_.size()) Fail("command");
            val = Value(int(code->code_[pos++]));
            break;
        default:
            val = Value();
        }
    }

    Value Pop() {
        if (stack.empty()) Fail("stack empty");
        Value v = stack.back();
        stack.pop_back();
        return v;
    }

    Value RunBlock() {
        Value result;
        for (;;) {
            Next();
            if (tok == tEOF) break;
            Statement();
            // As in the original, values left by a statement stay on the stack: the
            // compiled switch (exec(x); return(a, n); return(b, m); default) relies on it.
            if (!stack.empty()) { result = stack.back(); stack.pop_back(); }
            if (tok == tEOF) break;
            if (tok != tEOS) Fail("missing end of statement");
        }
        return result;
    }

    void Statement() {
        Comparisons();
        while (tok == tAnd || tok == tOr) {
            const uint8_t op = tok;
            Next();
            Comparisons();
            Value b = Pop(), a = Pop();
            stack.push_back(Value(op == tAnd ? (!a.IsZero() && !b.IsZero()) : (!a.IsZero() || !b.IsZero())));
        }
    }
    void Comparisons() {
        Concat();
        if (tok != tEq && tok != tLess && tok != tGreater && tok != tLessEq && tok != tGreaterEq && tok != tNotEq) return;
        const uint8_t op = tok;
        Next();
        Concat();
        Value b = Pop(), a = Pop();
        bool r = false;
        switch (op) {
        case tEq: r = vm->Equal(a, b); break;
        case tNotEq: r = !vm->Equal(a, b); break;
        case tLess: r = a.ToReal() < b.ToReal(); break;
        case tGreater: r = a.ToReal() > b.ToReal(); break;
        case tLessEq: r = a.ToReal() <= b.ToReal(); break;
        case tGreaterEq: r = a.ToReal() >= b.ToReal(); break;
        }
        stack.push_back(Value(r));
    }
    void Concat() {
        Arith1();
        while (tok == tConcat) {
            Next();
            Arith1();
            Value b = Pop(), a = Pop();
            stack.push_back(Value(vm->Str(a) + vm->Str(b)));
        }
    }
    static Value Num(double d, bool real) { return real ? Value(d) : Value(int(d)); }
    void Arith1() {
        Arith2();
        while (tok == tMinus || tok == tPlus) {
            const uint8_t op = tok;
            Next();
            Arith2();
            Value b = Pop(), a = Pop();
            if (a.type == VT::Point || b.type == VT::Point) {     // point arithmetic
                Point pa = a.ToPoint(), pb = b.ToPoint();
                stack.push_back(Value(op == tPlus ? Point{pa.x + pb.x, pa.y + pb.y} : Point{pa.x - pb.x, pa.y - pb.y}));
                continue;
            }
            const bool real = a.type == VT::Real || b.type == VT::Real;
            stack.push_back(Num(op == tPlus ? a.ToReal() + b.ToReal() : a.ToReal() - b.ToReal(), real));
        }
    }
    void Arith2() {
        Main();
        for (;;) {
            const uint8_t op = tok;
            if (op != tMul && op != tDiv && op != tIntDiv && op != tMod) return;
            Next();
            Main();
            Value b = Pop(), a = Pop();
            const bool real = a.type == VT::Real || b.type == VT::Real;
            if (op == tMul) { stack.push_back(Num(a.ToReal() * b.ToReal(), real)); continue; }
            const int bi = b.ToInt();
            if (op == tDiv) {
                if (b.ToReal() == 0) { stack.push_back(Value(1)); continue; }
                if (real) stack.push_back(Value(a.ToReal() / b.ToReal()));
                else stack.push_back(Value(a.ToInt() / bi));
                continue;
            }
            if (bi == 0) { stack.push_back(Value(1)); continue; }
            stack.push_back(Value(op == tIntDiv ? a.ToInt() / bi : a.ToInt() % bi));
        }
    }

    Value* IndexedVar(const std::string& name, const std::vector<Value>& index, bool grow = false) {
        Value* v = &vm->Var(name);
        for (const Value& ix : index) {
            if (grow && v->type != VT::List) *v = Value::MakeList(std::make_shared<List>());
            if (v->type != VT::List || !v->list) return nullptr;
            const int k = ix.ToInt();
            if (grow && k > int(v->list->a.size()) && k <= 4096) v->list->a.resize(size_t(k));   // assignment extends the list
            if (k < 1 || k > int(v->list->a.size())) return nullptr;
            v = &v->list->a[size_t(k - 1)];
        }
        return v;
    }

    // a variable reference (identifier or eval(name)): read, assign, compound assign, ++/--
    void VarRef(const std::string& name) {
            std::vector<Value> index;
        while (tok == tListStart) {
            Next();
            Statement();
            if (tok != tListEnd) Fail("expected ]");
            Next();
            index.push_back(Pop());
        }
        if (tok == tAssign) {
            Next();
            Statement();
            Value v = Pop();
            Value* dst = index.empty() ? &vm->Var(name) : IndexedVar(name, index, true);
            if (dst) *dst = v;
            stack.push_back(v);
        } else if (tok == tPlusEq || tok == tMinusEq || tok == tAndEq || tok == tMulEq || tok == tDivEq) {
            const uint8_t op = tok;
            Next();
            Statement();
            Value v = Pop();
            Value* dst = index.empty() ? &vm->Var(name) : IndexedVar(name, index);
            if (dst) {
                if (op == tAndEq) *dst = Value(vm->Str(*dst) + vm->Str(v));
                else {
                    const bool real = dst->type == VT::Real || v.type == VT::Real;
                    double a = dst->ToReal(), b = v.ToReal();
                    double r = op == tPlusEq ? a + b : op == tMinusEq ? a - b : op == tMulEq ? a * b : (b ? a / b : 1);
                    *dst = Num(r, real);
                }
                stack.push_back(*dst);
            } else stack.push_back(Value());
        } else {
            Value* v = index.empty() ? &vm->Var(name) : IndexedVar(name, index);
            stack.push_back(v ? *v : Value());
        }
        if (tok == tPlusPlus || tok == tMinusMinus) {          // post-increment
            Value* v = index.empty() ? &vm->Var(name) : IndexedVar(name, index);
            if (v) *v = Value(v->ToInt() + (tok == tPlusPlus ? 1 : -1));
            Next();
        }
    }

    void Main() {
        uint8_t prefix = 0;
        if (tok == tMinus || tok == tPlus) { prefix = tok; Next(); }
        switch (tok) {
        case tIdent: {
            const std::string name = val.s;
            Next();
            const std::string low = Lower(name);
            if (low == "self") { stack.push_back(vm->Ref(src)); break; }
            VarRef(name);
            break;
        }
        case tPlusPlus: case tMinusMinus: {
            const uint8_t op = tok;
            Next();
            if (tok != tIdent) Fail("expected identifier after ++/--");
            Value& v = vm->Var(val.s);
            v = Value(v.ToInt() + (op == tPlusPlus ? 1 : -1));
            stack.push_back(v);
            Next();
            break;
        }
        case tLiteral: case tConstMode: case tConstEvent: case tUnk5e: case tKeycode:
            stack.push_back(val);
            Next();
            break;
        case tString:
            stack.push_back(val);
            Next();
            break;
        case tTrue: stack.push_back(Value(1)); Next(); break;
        case tFalse: stack.push_back(Value(0)); Next(); break;
        case tNull: stack.push_back(Value(0)); Next(); break;
        case tOpen:
            Next();
            Statement();
            if (tok != tClose) Fail("expected )");
            Next();
            break;
        case tListStart: {
            Next();
            auto l = std::make_shared<List>();
            while (tok != tListEnd) {
                Statement();
                l->a.push_back(Pop());
                if (tok == tComma) Next();
                else if (tok != tListEnd) Fail("bad list literal");
            }
            Next();
            stack.push_back(Value::MakeList(l));
            break;
        }
        case tNot:
            Next();
            Comparisons();
            stack.push_back(Value(Pop().IsZero()));
            break;
        case tEval: {                                          // eval (expr): the variable named by expr
            Next();
            if (tok == tOpen) {
                Next();
                Statement();
                if (tok != tClose) Fail("expected )");
                Next();
            } else Main();
            const std::string name = vm->Str(Pop());
            VarRef(name);
            break;
        }
        case tGeneral: General(uint8_t(val.i)); break;
        case tItemCmd: ItemCommand(uint8_t(val.i)); break;
        case tNotifyCmd: NotifyCommand(uint8_t(val.i)); break;
        default: Fail("unknown token");
        }
        if (prefix == tMinus) {
            Value v = Pop();
            if (v.type == VT::Real) v.r = -v.r;
            else if (v.type == VT::Point) { v.p.x = -v.p.x; v.p.y = -v.p.y; }
            else v = Value(-v.ToInt());
            stack.push_back(v);
        }
    }

    std::vector<Value> Params() {
        if (pos >= code->code_.size()) Fail("params");
        const uint8_t n = code->code_[pos++];
        std::vector<Value> ps;
        Next();
        if (!n) {
            // parameterless calls may still carry "()"
            if (tok == tOpen) { Next(); if (tok == tClose) Next(); }
            return ps;
        }
        if (tok != tOpen) Fail("expected ( before parameters");
        Next();
        for (uint8_t i = 0; i < n; ++i) {
            // (the original takes a missing comma: SMASH has "substring(self, 1stringlen(self) - 4)")
            if (i && tok == tComma) Next();
            const size_t depthBefore = stack.size();
            Statement();
            if (stack.size() <= depthBefore) Fail("empty parameter");
            ps.push_back(Pop());
        }
        if (tok != tClose) Fail("expected )");
        Next();
        return ps;
    }

    Item* Target(const Value& v) { return vm->Resolve(v); }
    Rect RectOf(const std::vector<Value>& ps, size_t i = 0) {
        if (ps.size() <= i) return src ? src->GetRect() : Rect{};
        const Value& v = ps[i];
        if (v.type == VT::Item || v.type == VT::String) {
            if (Item* it = Target(v)) return it->GetRect();
        }
        return v.ToRect();
    }

    void General(uint8_t id);
    void ItemCommand(uint8_t id);
    void NotifyCommand(uint8_t id);
};

static Rect Normalize(Rect r, const Engine* vm) {
    if (!vm->wrap_) return r;
    const int ww = vm->world_.Width();
    if (ww <= 0) return r;
    int x = ((r.left % ww) + ww) % ww;
    r.Translate(x - r.left, 0);
    return r;
}

void Interp::General(uint8_t id) {
    std::vector<Value> ps = Params();
    auto P = [&](size_t i) -> const Value& { static const Value zero; return i < ps.size() ? ps[i] : zero; };
    switch (id) {
    case 1: {                                                  // eval
        Code tmp(vm);
        tmp.strings_ = code->strings_;
        const uint32_t off = tmp.Compile(vm->Str(P(0)));
        stack.push_back(tmp.Run(src, off));
        break;
    }
    case 2: {                                                  // random(min, max)
        int a = P(0).ToInt(), b = P(1).ToInt();
        if (b < a) std::swap(a, b);
        stack.push_back(Value(std::uniform_int_distribution<int>(a, b)(vm->Rng())));
        break;
    }
    case 3: stack.push_back(Value(int(vm->Str(P(0)).size()))); break;                         // stringlen
    case 4: {                                                  // substring(s, from, to) 1-based inclusive
        const std::string s = vm->Str(P(0));
        int a = P(1).ToInt(), b = P(2).ToInt();
        if (a < 1) a = 1;
        if (b > int(s.size())) b = int(s.size());
        stack.push_back(Value(a > b ? std::string() : s.substr(size_t(a - 1), size_t(b - a + 1))));
        break;
    }
    case 5: stack.push_back(P(0).ToReal() >= P(1).ToReal() ? P(0) : P(1)); break;            // max
    case 6: stack.push_back(P(0).ToReal() <= P(1).ToReal() ? P(0) : P(1)); break;            // min
    case 7: { Value v = P(0); if (v.type == VT::Real) v.r = std::fabs(v.r); else v = Value(std::abs(v.ToInt())); stack.push_back(v); break; }
    case 8:                                                    // getrect / makerect
        if (ps.size() < 2) stack.push_back(Value(RectOf(ps)));
        else if (ps.size() == 2) { Point a = P(0).ToPoint(), b = P(1).ToPoint(); stack.push_back(Value(Rect{a.x, a.y, b.x, b.y})); }
        else stack.push_back(Value(Rect{P(0).ToInt(), P(1).ToInt(), P(2).ToInt(), P(3).ToInt()}));
        break;
    case 9: stack.push_back(Value(Point{P(0).ToInt(), P(1).ToInt()})); break;                  // makept
    case 10: { Rect r = RectOf(ps); stack.push_back(Value(Point{r.left, r.top})); break; }     // topleft
    case 11: { Rect r = RectOf(ps); stack.push_back(Value(Point{r.right, r.bottom})); break; } // bottomright
    case 12: stack.push_back(Value(vm->MouseWorld())); break;                                  // mousepos
    case 13: stack.push_back(Value(RectOf(ps).top)); break;
    case 14: stack.push_back(Value(RectOf(ps).left)); break;
    case 15: stack.push_back(Value(RectOf(ps).bottom)); break;
    case 16: stack.push_back(Value(RectOf(ps).right)); break;
    case 17: stack.push_back(Value(P(0).ToPoint().x)); break;                                  // xpos
    case 18: stack.push_back(Value(P(0).ToPoint().y)); break;                                  // ypos
    case 19: {                                                 // playfrom(item, position)
        if (Item* it = Target(P(0))) {
            it->SeekToTime(uint32_t(std::max(0, P(1).ToInt())));
            it->TogglePlaying(false, false);
        }
        break;
    }
    case 20: {                                                 // move([item,] delta)
        Item* it = ps.size() >= 2 ? Target(P(0)) : src;
        Point d = (ps.size() >= 2 ? P(1) : P(0)).ToPoint();
        if (!it) break;
        if (DevEnv("MOVELOG") && it->Name() == DevEnv("MOVELOG")) {
            char b[160]; std::snprintf(b, sizeof b, "MOVE %s by %d,%d params %zu top %s ev %d", it->Name().c_str(), d.x, d.y, ps.size(),
                vm->runStack_.empty() ? "-" : vm->runStack_.back().first->Name().c_str(), vm->runStack_.empty() ? -1 : vm->runStack_.back().second);
            vm->Log(b);
        }
        // The babies' placement (MAIN and the baby pages): an item moves itself by the
        // node's shift when placed (events 26, 178, 204) and back by it when the game moves
        // on (179, MainControl's unwind of xvBabyList). Every move in those events is that
        // pair. Kept exact here: placing an item that is already placed moves it to the new
        // shift instead of adding a second one, and the unwind undoes exactly what was
        // applied (once). Otherwise a second placement in one view - or an unwind of an
        // item placed twice - left Tommy's idle animations a shift away (drawn at the screen
        // edge or off it) or put Tommy himself back at his unshifted spot.
        if (ps.size() == 1 && !vm->runStack_.empty() && vm->runStack_.back().first == it) {
            const uint16_t ev = vm->runStack_.back().second;
            if (ev == 26 || ev == 178 || ev == 204) {
                if (it->babyShifted_) it->MoveBy(d.x - it->babyShift_.x, d.y - it->babyShift_.y);
                else it->MoveBy(d.x, d.y);
                it->babyShift_ = d; it->babyShifted_ = true;
                break;
            }
            if (ev == 179) {
                if (it->babyShifted_) it->MoveBy(-it->babyShift_.x, -it->babyShift_.y);
                it->babyShifted_ = false; it->babyShift_ = Point{};
                break;
            }
        }
        it->MoveBy(d.x, d.y);
        break;
    }
    case 23: {                                                 // setdragparams([item,] [point,] rect)
        Item* it = src;
        size_t k = 0;
        if (ps.size() >= 2 && (P(0).type == VT::Item || Target(P(0)))) { it = Target(P(0)); k = 1; }
        const bool point = ps.size() >= k + 2;
        if (it && !ps.empty()) vm->dragLimits_[it->Serial()] = Engine::DragLimit{ps.back().ToRect(), !point};
        (void)k;
        break;
    }
    case 24: break;                                            // resetdragparams
    case 25: {                                                 // rollover([item,] blocking, enable)
        // zones use rollover(0, on): the pointer is also seen by items beneath them;
        // objects use rollover(1, on): they stop the search
        Item* it = src;
        size_t k = 0;
        if (ps.size() == 3) { it = Target(P(0)); k = 1; }
        if (!it) break;
        if (ps.size() >= k + 2) { it->rolloverBlocks_ = !P(k).IsZero(); it->rollover_ = !P(k + 1).IsZero(); }
        else it->rollover_ = !P(k).IsZero();
        if (it->rollover_) vm->rolloverDirty_ = true;
        if (!it->rollover_) vm->EndRollover(it);
        break;
    }
    case 26:                                                   // setcursor(name)
        vm->PostNotify(NotifyEvent{NotifyEvent::kCursor, 0, 0, 0, 0, vm->Str(P(0))});
        break;
    case 27: stack.push_back(Value(RectOf(ps).Width())); break;
    case 28: stack.push_back(Value(RectOf(ps).Height())); break;
    case 29: {                                                 // getframerect([item])
        Item* it = ps.empty() ? src : Target(P(0));
        stack.push_back(Value(it ? it->FrameRect() : Rect{}));
        break;
    }
    case 31: stack.push_back(Value(std::sqrt(P(0).ToReal()))); break;
    case 32: vm->DeleteVar(vm->Str(P(0))); break;                                             // deletevar
    case 35: vm->SetViewOrigin(P(0).ToPoint()); break;                                         // setvieworigin
    case 36: {                                                 // rectsect(a, b)
        Rect a = P(0).ToRect(), b = P(1).ToRect();
        bool hit = a.Intersects(b);
        if (!hit && vm->wrap_) { Rect a2 = Normalize(a, vm), b2 = Normalize(b, vm); hit = a2.Intersects(b2); }
        stack.push_back(Value(hit));
        break;
    }
    case 37: stack.push_back(Value(vm->view_)); break;                                         // getvieworigin
    case 38: stack.push_back(Value(Rect{vm->view_.x, vm->view_.y, vm->view_.x + 640, vm->view_.y + 480})); break;
    case 39: stack.push_back(Value(vm->PageNumber())); break;                                  // getpage
    case 40: stack.push_back(Value(vm->world_)); break;                                        // getworldrect
    case 41: stack.push_back(Value(vm->wrap_)); break;                                         // isworldwrap
    case 42: stack.push_back(Value::MakeList(std::make_shared<List>())); break;                // newlist
    case 43: break;                                                                            // deletelist
    case 44: if (P(0).type == VT::List && P(0).list) P(0).list->a.push_back(P(1)); break;      // add
    case 46: {                                                 // addat(list, index, value)
        if (P(0).type != VT::List || !P(0).list) break;
        auto& a = P(0).list->a;
        int k = std::max(1, P(1).ToInt());
        if (size_t(k) > a.size() + 1) a.resize(size_t(k - 1));
        a.insert(a.begin() + (k - 1), P(2));
        break;
    }
    case 47: {                                                 // getat(list, index)
        Value r;
        if (P(0).type == VT::List && P(0).list) {
            int k = P(1).ToInt();
            if (k >= 1 && k <= int(P(0).list->a.size())) r = P(0).list->a[size_t(k - 1)];
        }
        stack.push_back(r);
        break;
    }
    case 49: {                                                 // getindex(list, value)
        int r = 0;
        if (P(0).type == VT::List && P(0).list)
            for (size_t k = 0; k < P(0).list->a.size(); ++k)
                if (vm->Equal(P(0).list->a[k], P(1))) { r = int(k + 1); break; }
        stack.push_back(Value(r));
        break;
    }
    case 50: {                                                 // setat(list, index, value)
        if (P(0).type != VT::List || !P(0).list) break;
        int k = P(1).ToInt();
        if (k < 1) break;
        if (size_t(k) > P(0).list->a.size()) P(0).list->a.resize(size_t(k));
        P(0).list->a[size_t(k - 1)] = P(2);
        break;
    }
    case 51: stack.push_back(Value(P(0).type == VT::List && P(0).list ? int(P(0).list->a.size()) : 0)); break;
    case 52: {                                                 // deleteat(list, index)
        if (P(0).type != VT::List || !P(0).list) break;
        int k = P(1).ToInt();
        if (k >= 1 && k <= int(P(0).list->a.size())) P(0).list->a.erase(P(0).list->a.begin() + (k - 1));
        break;
    }
    case 53: if (P(0).type == VT::List && P(0).list) P(0).list->a.clear(); break;              // clearlist
    case 54:                                                   // setworld(rect, wrap)
        vm->world_ = P(0).ToRect();
        vm->wrap_ = !P(1).IsZero();
        break;
    case 55: {                                                 // setproperty([item,] name, value)
        Item* it = ps.size() >= 3 ? Target(P(0)) : src;
        const size_t k = ps.size() >= 3 ? 1 : 0;
        if (it) it->props_[Lower(vm->Str(P(k)))] = P(k + 1);
        if (DevEnv("LBDEBUG2")) vm->Log("setproperty " + (it ? it->Name() : std::string("?")) + "." + vm->Str(P(k)) + "=" + vm->Str(P(k + 1)));
        break;
    }
    case 56: {                                                 // getproperty([item,] name)
        Item* it = ps.size() >= 2 ? Target(P(0)) : src;
        const size_t k = ps.size() >= 2 ? 1 : 0;
        Value r;
        if (it) { auto f = it->props_.find(Lower(vm->Str(P(k)))); if (f != it->props_.end()) r = f->second; }
        stack.push_back(r);
        break;
    }
    case 57: {                                                 // copylist
        auto l = std::make_shared<List>();
        if (P(0).type == VT::List && P(0).list) l->a = P(0).list->a;
        stack.push_back(Value::MakeList(l));
        break;
    }
    case 59: {                                                 // exec(offset): run code at offset
        Interp sub{vm, code, src, size_t(std::max(0, P(0).ToInt()))};
        sub.depth = depth + 1;
        if (sub.depth > 64) Fail("exec too deep");
        Value v = sub.RunBlock();
        stack.push_back(v);
        stack.push_back(v);
        break;
    }
    case 60: {                                                 // return(match, result): switch arm
        if (stack.empty()) Fail("return on empty stack");
        if (vm->Equal(ps.size() ? P(0) : Value(), stack.back())) {
            stack.pop_back();
            stack.push_back(P(1));
            tok = tEOF;
            pos = code->code_.size();
            return;
        }
        stack.push_back(stack.back());
        break;
    }
    case 61: {                                                 // sendsync(item, data[, delay])
        Item* it = Target(P(0));
        if (it) vm->timers_.push_back(Engine::Timer{vm->Now(), it->Serial(), uint16_t(P(1).ToInt()), kEvNotified, true});
        break;
    }
    case 62: { Point d = P(0).ToPoint(); vm->SetViewOrigin(Point{vm->view_.x + d.x, vm->view_.y + d.y}); break; }
    case 63: {                                                 // addtogroup(item, groupName) / addtogroup(groupName)
        Item* it = ps.size() >= 2 ? Target(P(0)) : src;
        const std::string g = vm->Str(ps.size() >= 2 ? P(1) : P(0));
        if (GroupItem* grp = vm->GroupByName(g, true); grp && it) {
            if (std::find(grp->serials_.begin(), grp->serials_.end(), it->Serial()) == grp->serials_.end())
                grp->serials_.push_back(it->Serial());
        }
        break;
    }
    case 64: {                                                 // removefromgroup
        Item* it = ps.size() >= 2 ? Target(P(0)) : src;
        const std::string g = vm->Str(ps.size() >= 2 ? P(1) : P(0));
        if (GroupItem* grp = vm->GroupByName(g, false); grp && it) {
            auto& v = grp->serials_;
            v.erase(std::remove(v.begin(), v.end(), it->Serial()), v.end());
            auto& ids = grp->ids_;
            ids.erase(std::remove(ids.begin(), ids.end(), it->Id()), ids.end());
        }
        break;
    }
    case 65: if (GroupItem* grp = vm->GroupByName(vm->Str(P(0)), false)) { grp->serials_.clear(); grp->ids_.clear(); } break;
    case 66: {                                                 // setplayparams(item, loop, delay, timing, period, reloc, control, sound)
        Item* it = Target(P(0));
        if (!it) break;
        const size_t n = ps.size();
        if (n == 1) { it->loopMode_ = 0; it->timingMode_ = 0; break; }
        if (n >= 2) it->loopMode_ = uint16_t(P(1).ToInt());
        if (n >= 3) { Point d = P(2).ToPoint(); it->delayMin_ = uint32_t(std::max(0, d.x)); it->delayMax_ = uint32_t(std::max(0, d.y)); }
        if (n >= 4) it->timingMode_ = uint16_t(P(3).ToInt());
        if (n >= 5) { Point d = P(4).ToPoint(); it->periodMin_ = uint32_t(std::max(0, d.x)); it->periodMax_ = uint32_t(std::max(0, d.y)); }
        if (n >= 6) it->relocPoint_ = P(5).ToPoint();
        if (n >= 7) it->controlMode_ = uint16_t(P(6).ToInt());
        if (n >= 8) it->soundMode_ = uint16_t(P(7).ToInt());
        // timing off: drop a pending automatic start too (a "stop" just before may have re-armed
        // the idle timer, which would otherwise fire once more: Didi's walk-by started twice)
        if (it->timingMode_ == kAutoNone) it->CancelTimer();
        if ((it->timingMode_ == kAutoLoad || it->timingMode_ == kAutoUserIdle) && it->Loaded() && !it->IsPlaying())
            it->SetNextTime(it->periodMin_, it->periodMax_);
        break;
    }
    case 67: {                                                 // autoevent(item, event, loops, "delay min,max", timing, period)
        // Like setplayparams, but what repeats is "run event N": loops times (65534 = for
        // ever), the delay between runs, started by the timing mode (7 = at once, 4 = when the
        // item is notified with the period's sync data). The cookie game drops each cookie
        // with one: on its sync, event 254 thirty-three times, 10 pixels a step.
        Item* it = Target(P(0));
        if (!it) break;
        Engine::AutoEvent a;
        a.item = it->Serial();
        a.event = uint16_t(P(1).ToInt());
        a.loops = uint16_t(P(2).ToInt());
        const Point d = P(3).ToPoint();
        a.delayMin = uint32_t(std::max(0, d.x)); a.delayMax = uint32_t(std::max(d.x, d.y));
        a.timing = uint16_t(P(4).ToInt());
        a.sync = P(5).ToPoint();
        vm->AddAutoEvent(a);
        break;
    }
    case 70: stack.push_back(Value(src ? int(src->Id()) : 0)); break;                       // getid
    case 72: stack.push_back(Value(int(vm->Now()))); break;                                   // gettime
    case 73: case 74: vm->Log("script log: " + vm->Str(P(0))); break;                          // logwriteln / logwrite
    case 75: stack.push_back(Value(1)); break;                                                // getlanguage
    case 80: {                                                 // setkeyevent(key, event)
        int key = 0, mods = 0;
        const Value& k = P(0);
        if (k.type == VT::String) key = k.s.empty() ? 0 : (unsigned char)k.s[0] | 0x10000;    // character
        else { key = k.ToInt() & 0xffff; mods = (k.ToInt() >> 16) & 0xff; }
        vm->keyEvents_.push_back(Engine::KeyEvent{key, mods, src ? src->Serial() : 0, uint16_t(P(1).ToInt())});
        break;
    }
    case 81: if (src) src->doHitTest_ = ps.empty() ? false : !P(ps.size() - 1).IsZero(); break; // sethittest
    case 82: {                                                 // key(k[, mods]) -> key code value
        const Value& k = P(0);
        int v = k.type == VT::String ? (k.s.empty() ? 0 : (unsigned char)k.s[0]) : k.ToInt();
        if (ps.size() >= 2) v |= (P(1).ToInt() & 0xff) << 16;
        stack.push_back(Value(v));
        break;
    }
    case 83: break;                                                                           // deletekeyevent
    case 89: vm->waitCursor_ = ps.empty() ? true : !P(0).IsZero(); break;      // waitcursor(on): the busy cursor
    case 84: case 90: break;                                  // setdisplay, debugbreak
    case 91: vm->SetMenuItemEnabled(P(0).ToInt(), !P(1).IsZero()); break;          // menuitemenable(id, on)
    case 92: case 93: {                                        // showchannel / hidechannel(movie, track)
        if (auto* mv = dynamic_cast<MovieItem*>(Target(P(0)))) mv->SetChannel(P(1).ToInt(), id == 92);
        break;
    }
    case 85: stack.push_back(Value(0)); break;                                                 // getdisplay
    case 87: {                                                 // lbxcreate(type)
        stack.push_back(Value::Lbx(CreateLBX(vm, P(0).ToInt())));
        break;
    }
    case 88: {                                                 // lbxfunc(obj, call, ...)
        if (P(0).type != VT::LBX || !P(0).lbx) { vm->Log("lbxfunc on a non-LBX value"); break; }
        std::vector<Value> args(ps.begin() + std::min<size_t>(2, ps.size()), ps.end());
        Value r;
        if (P(0).lbx->Call(P(1).ToInt(), args, r)) stack.push_back(r);
        break;
    }
    case 94: vm->SetPageFade(!P(0).IsZero()); break;                                           // setpagefade
    case 95: stack.push_back(Value(Normalize(P(0).ToRect(), vm))); break;                      // normalize
    default: {
        char b[64];
        std::snprintf(b, sizeof b, "general command %d not implemented", id);
        vm->Log(b);
        break;
    }
    }
}

void Interp::ItemCommand(uint8_t id) {
    std::vector<Value> ps = Params();
    auto P = [&](size_t i) -> const Value& { static const Value zero; return i < ps.size() ? ps[i] : zero; };
    // Item commands take an optional target first; with fewer arguments they act on self.
    auto TargetOr = [&](size_t minArgsWithTarget) -> std::pair<Item*, size_t> {
        if (ps.size() >= minArgsWithTarget) return {Target(P(0)), 1};
        return {src, 0};
    };
    switch (id) {
    case 1:                                                    // clone(newId, newName) of self
        if (src) vm->Clone(src, uint16_t(P(0).ToInt()), vm->Str(P(1)));
        break;
    case 2: { auto [it, k] = TargetOr(1); if (it) it->DestroySelf(); break; }                  // destroy
    case 3: {                                                  // dragbeginfrom(item, point)
        auto [it, k] = TargetOr(2);
        if (it) vm->BeginDrag(it, P(k).ToPoint());
        break;
    }
    case 4: { auto [it, k] = TargetOr(1); if (it) vm->EndDrag(it); break; }                   // dragend
    case 5: case 6: { auto [it, k] = TargetOr(2); if (it) it->SetEnabled(ps.empty() ? true : !P(k).IsZero()); break; }
    case 7: case 8: { auto [it, k] = TargetOr(2); if (it) it->SetVisible(ps.empty() ? true : !P(k).IsZero()); break; }
    case 9: { auto [it, k] = TargetOr(1); stack.push_back(Value(it ? it->FrameNumber() : 0)); break; }           // getframe
    case 10: { auto [it, k] = TargetOr(1); Item* p = it ? vm->ItemBySerial(it->parent_) : nullptr; stack.push_back(vm->Ref(p)); break; }
    case 11: {                                                 // getposition: playback position (ms)
        auto [it, k] = TargetOr(1);
        stack.push_back(Value(it ? it->PositionMs() : 0));
        break;
    }
    case 13: { auto [it, k] = TargetOr(1); Item* nx = nullptr; (void)it; stack.push_back(vm->Ref(nx)); break; }  // getznext
    case 14: { auto [it, k] = TargetOr(1); stack.push_back(vm->Ref(it ? vm->ZPrev(it) : nullptr)); break; }     // getzprev
    case 15: {                                                 // hittest([item,] point)
        auto [it, k] = TargetOr(2);
        stack.push_back(Value(it && it->Contains(vm->Near(it, P(k).ToPoint()))));
        break;
    }
    case 16: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && it->IsAmbient())); break; }
    case 17: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && it->Enabled())); break; }
    case 18: stack.push_back(Value(0)); break;                                                 // ismuted
    case 19: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && it->IsPlaying())); break; }
    case 20: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && it->Visible())); break; }
    case 21: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && it->Loaded())); break; }
    case 22: { auto [it, k] = TargetOr(1); stack.push_back(Value(it && vm->Focus() == it)); break; }
    case 23: { auto [it, k] = TargetOr(1); if (it) it->Load(); break; }
    case 24: {                                                 // moveto([item,] point)
        auto [it, k] = TargetOr(2);
        Point p = P(k).ToPoint();
        if (it) {
            it->MoveTo(p.x, p.y);
            // an absolute place ends any placement shift (Lil, Phil and Chuckie go back to
            // their AmbientOrigin this way in event 179)
            it->babyShifted_ = false; it->babyShift_ = Point{};
        }
        break;
    }
    case 25: break;                                                                            // mute
    case 26: { auto [it, k] = TargetOr(1); if (it) it->TogglePlaying(false, true); break; }   // play
    case 27: { auto [it, k] = TargetOr(2); if (it) it->SeekToTime(uint32_t(std::max(0, P(k).ToInt()))); break; }
    case 28: { auto [it, k] = TargetOr(2); if (it) it->Seek(uint16_t(std::max(0, P(k).ToInt()))); break; }
    case 29: {                                                 // setparent([item,] parent)
        Item* it = src;
        Value parent = P(0);
        if (ps.size() >= 2) { it = Target(P(0)); parent = P(1); }
        if (!it) break;
        if (parent.IsNumeric() && parent.ToInt() < 0) {
            // -1: anchored to the screen from where it is now (scripts place it first, e.g. the
            // zoom movies do move(getvieworigin()); setparent(-1)); it then moves with the view
            it->parent_ = Item::kScreenParent;
            break;
        }
        Item* p = (parent.IsNumeric() && parent.ToInt() == 0) ? nullptr : Target(parent);
        const bool already = p && it->parent_ == p->Serial();
        it->parent_ = p ? p->Serial() : 0;
        if (already) break;                                   // (loaded again: it has kept up)
        // The original sets a parent up when the page's items are created, even for one that
        // loads later: such a child has followed every move its parent made since then. The
        // chute babies' talking pictures (loaded on a click) follow the intro picture's move
        // 1000 to the right at the start; the chute zones, created later with a new chute,
        // don't.
        if (p) {
            int dx = 0, dy = 0;
            for (const auto& m : p->moves_) if (m.stamp > it->bornStamp_) { dx += m.dx; dy += m.dy; }
            if (dx || dy) it->MoveBy(dx, dy);
        }
        break;
    }
    case 30: {                                                 // setzorder([item,] where)
        Item* it = src;
        Value where = P(0);
        if (ps.size() >= 2) { it = Target(P(0)); where = P(1); }
        if (!it) break;
        if (where.type == VT::Integer || where.type == VT::Real) vm->SetZOrder(it, where.ToInt());
        else if (Item* ref = Target(where)) vm->SetZBehind(it, ref);
        break;
    }
    case 31: break;                                                                            // settext
    case 32: { auto [it, k] = TargetOr(1); if (it) it->Stop(); break; }                       // stop
    case 33: { auto [it, k] = TargetOr(1); if (it) it->Unload(); break; }                     // unload
    default: {
        char b[64];
        std::snprintf(b, sizeof b, "item command %d not implemented", id);
        vm->Log(b);
        break;
    }
    }
}

void Interp::NotifyCommand(uint8_t id) {
    std::vector<Value> ps = Params();
    NotifyEvent n{NotifyEvent::kGotoPage};
    switch (id) {
    case 1: break;                                             // click
    case 2: n.kind = NotifyEvent::kGotoControls; vm->PostNotify(n); break;
    case 3:                                                    // goto
        if (ps.size() >= 4) {
            n.kind = NotifyEvent::kChangeMode;
            n.mode = ps[1].ToInt(); n.page = ps[2].ToInt(); n.subpage = ps[3].ToInt();
        } else if (ps.size() >= 1) n.param = ps[0].ToInt();
        vm->PostNotify(n);
        break;
    case 4: n.kind = NotifyEvent::kGotoQuit; vm->PostNotify(n); break;
    case 5: n.kind = NotifyEvent::kIntroDone; n.param = 1; vm->PostNotify(n); break;
    case 7: n.kind = NotifyEvent::kShowCursor; n.param = ps.empty() ? 1 : ps[0].ToInt(); if (src) n.text = src->Name(); vm->PostNotify(n); break;
    case 13: n.kind = NotifyEvent::kQuit; vm->PostNotify(n); break;
    default: break;
    }
}

} // namespace

Value Code::Run(Item* source, uint32_t offset) {
    if (offset >= code_.size()) return Value();
    Interp in{vm_, this, source, offset};
    try {
        return in.RunBlock();
    } catch (const std::exception& e) {
        vm_->Log(std::string(e.what()) + " in {" + Disassemble(offset) + "}");
        return Value();
    }
}

// ---- compiler for script text ------------------------------------------------------------
// Living Books evaluates text in a few places (conditions on script entries, kLBCommand
// data, saved variables in the player file). Same token stream as BCOD.
uint32_t Code::Compile(const std::string& src) {
    static const char* const general[] = {
        "", "eval", "random", "stringlen", "substring", "max", "min", "abs", "getrect", "makept", "topleft",
        "bottomright", "mousepos", "top", "left", "bottom", "right", "xpos", "ypos", "playfrom", "move"};
    std::vector<uint8_t> out;
    std::vector<size_t> counters;
    bool wasFunction = false;
    size_t i = 0;
    auto pushU16 = [&](uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); };
    while (i < src.size()) {
        const char c = src[i];
        const char la = i + 1 < src.size() ? src[i + 1] : 0;
        ++i;
        if (c == '/' && la == '/') break;
        struct Op { char c; uint8_t tok; char la1; uint8_t tok1; char la2; uint8_t tok2; };
        static const Op ops[] = {
            {'+', tPlus, '+', tPlusPlus, '=', tPlusEq}, {'-', tMinus, '-', tMinusMinus, '=', tMinusEq},
            {'/', tDiv, '=', tDivEq, 0, 0}, {'*', tMul, '=', tMulEq, 0, 0}, {'=', tAssign, '=', tEq, 0, 0},
            {'>', tGreater, '=', tGreaterEq, 0, 0}, {'<', tLess, '=', tLessEq, 0, 0}, {'!', tNot, '=', tNotEq, 0, 0},
            {'&', tConcat, '&', tAnd, '=', tAndEq}, {'|', 0, '|', tOr, 0, 0}, {';', tEOS, 0, 0, 0, 0}};
        bool handled = false;
        for (const Op& o : ops) {
            if (c != o.c) continue;
            if (la && la == o.la1) { out.push_back(o.tok1); ++i; }
            else if (la && o.la2 && la == o.la2) { out.push_back(o.tok2); ++i; }
            else if (o.tok) out.push_back(o.tok);
            handled = true;
            break;
        }
        if (handled) continue;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '"' || c == '\'') {
            std::string s;
            while (i < src.size() && src[i] != c) s += src[i++];
            ++i;
            out.push_back(tString);
            pushU16(AddString(s));
            continue;
        }
        if (c == '(') {
            if (wasFunction) {
                wasFunction = false;
                counters.push_back(out.size());
                out.push_back(1);
                size_t j = i;
                while (j < src.size() && src[j] == ' ') ++j;
                if (j < src.size() && src[j] == ')') out.back() = 0;
            } else counters.push_back(0);
            out.push_back(tOpen);
            continue;
        }
        if (c == ')') {
            if (!counters.empty()) counters.pop_back();
            out.push_back(tClose);
            continue;
        }
        if (c == ',') {
            out.push_back(tComma);
            if (!counters.empty() && counters.back()) out[counters.back()]++;
            continue;
        }
        if (c == '[') { out.push_back(tListStart); continue; }
        if (c == ']') { out.push_back(tListEnd); continue; }
        if (std::isdigit((unsigned char)c)) {
            const char* in = src.c_str() + i - 1;
            char* end = nullptr;
            long v = std::strtol(in, &end, 0);
            if (*end == '.' && std::isdigit((unsigned char)end[1])) {      // a real (saved values like 0.5)
                const double d = std::strtod(in, &end);
                i += size_t(end - in) - 1;
                const int32_t raw = int32_t(std::lround(d * 10000.0));
                out.push_back(tLiteral);
                out.push_back(2);
                pushU16(uint16_t(uint32_t(raw) >> 16));
                pushU16(uint16_t(raw));
                continue;
            }
            i += size_t(end - in) - 1;
            out.push_back(tLiteral);
            out.push_back(1);
            pushU16(uint16_t(v));
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::string w(1, c);
            while (i < src.size() && (std::isalnum((unsigned char)src[i]) || src[i] == '_')) w += src[i++];
            const std::string lw = Lower(w);
            if (lw == "true") { out.push_back(tTrue); continue; }
            if (lw == "false") { out.push_back(tFalse); continue; }
            int fn = 0;
            for (int k = 1; k < int(sizeof general / sizeof *general); ++k) if (lw == general[k]) fn = k;
            if (fn) { out.push_back(tGeneral); out.push_back(uint8_t(fn)); wasFunction = true; continue; }
            out.push_back(tIdent);
            pushU16(AddString(w));
            continue;
        }
        // unknown character: ignore
    }
    out.push_back(tEOF);
    const uint32_t off = uint32_t(code_.size());
    code_.insert(code_.end(), out.begin(), out.end());
    return off;
}

std::string Code::Disassemble(uint32_t offset, size_t maxLen) const {
    std::string s;
    char b[32];
    for (size_t p = offset; p < code_.size() && s.size() < maxLen; ) {
        uint8_t t = code_[p++];
        if (t == tEOF) break;
        if ((t == tIdent || t == tString) && p + 2 <= code_.size()) {
            uint16_t o = uint16_t(code_[p] << 8 | code_[p + 1]); p += 2;
            auto it = strings_.find(o);
            s += t == tString ? "\"" + (it != strings_.end() ? it->second : "?") + "\"" : (it != strings_.end() ? it->second : "?");
        } else if (t == tLiteral && p + 5 <= code_.size() && code_[p] == 0x02) {
            const int32_t raw = int32_t(uint32_t(code_[p + 1]) << 24 | uint32_t(code_[p + 2]) << 16 | uint32_t(code_[p + 3]) << 8 | code_[p + 4]);
            std::snprintf(b, sizeof b, "%g", raw / 10000.0);
            s += b; p += 5;
        } else if (t == tLiteral && p + 3 <= code_.size()) {
            std::snprintf(b, sizeof b, "%d", code_[p] == 0x11 ? (code_[p + 1] | code_[p + 2] << 8) : (code_[p + 1] << 8 | code_[p + 2]));
            s += b; p += 3;
        } else if ((t == tGeneral || t == tItemCmd || t == tNotifyCmd) && p + 2 <= code_.size()) {
            std::snprintf(b, sizeof b, "%c%d", t == tGeneral ? 'g' : t == tItemCmd ? 'i' : 'n', code_[p]);
            s += b; p += 2;
        } else if ((t == tConstMode || t == tConstEvent || t == tKeycode || t == tUnk5e) && p + 2 <= code_.size()) {
            std::snprintf(b, sizeof b, "#%d", code_[p] << 8 | code_[p + 1]);
            s += b; p += 2;
        } else {
            std::snprintf(b, sizeof b, " %02x ", t);
            s += b;
        }
    }
    return s;
}

} // namespace lb
