// script_interpreter.h - page object tables (BITL), BCOD structure, world placement, events.
//
// BITL (verified: frames exactly, every byte, in 13 archives / 284 objects):
//   repeated objects: Rect (s16 l,t,r,b) | u16 type | u16 id1 | u16 objectId | u16 codeLength
//                     | name, NUL-terminated (no padding) | code[codeLength]
//   type 0x02  drawable "patch": id1 = tBMP id; rect size == bitmap size (all 6 in N01)
//   type 0x03  hotspot (clickable zone)
//   type 0x42  group: code = 0x0064, len, count, count x {0x0000, objectId}
//   objects named View1..ViewN (type 0x02): the panorama, rect.left = -(view origin)
//   Rects are relative to the view the object belongs to (group grpViewN):
//   world x = view origin + rect.x. Checked by image matching on N01: BallGag, Screw (View4),
//   BlockMask (View2) match the panorama 100% at that offset.
//   Other type values (0x40, 0x41, 0x43, 0x45, 0x46) are not identified yet.
//
// Object code and BCOD are bytecode whose instruction set is NOT decoded yet: object code
// starts with a 16-bit header word (0x0079, 0x007B, 0x006E, ...) and is partly made of
// {0x0066, u16 length, payload} records; BCOD starts with u32 total length, u32 split
// offset. Events are routed to objects here, but no bytecode is executed.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mohawk.h"
#include "video_sprites.h"   // Rect16

namespace mohawk {

enum ObjectType : uint16_t { kObjPatch = 0x02, kObjHotspot = 0x03, kObjGroup = 0x42 };

struct ScriptObject {
    Rect16 rect;
    uint16_t type = 0, resourceId = 0, objectId = 0;
    std::string name;
    std::vector<uint8_t> code;
    uint16_t CodeHeader() const { return code.size() >= 2 ? uint16_t(code[0] << 8 | code[1]) : 0; }
};

struct ObjectTable {
    std::vector<ScriptObject> objects;
    std::map<std::string, size_t> byName;            // "XyloGag" -> index
    std::map<uint16_t, size_t> byId;                 // objectId -> index
    std::map<std::string, int> viewOrigin;           // "View3" -> 1371 (world x)
    std::map<uint16_t, std::string> viewOf;          // objectId -> "View3" (from grpViewN)

    const ScriptObject* Find(const std::string& name) const;
    // World-space rectangle (x may exceed the world width; callers wrap). nullopt when the
    // object's view is not known from the groups.
    std::optional<SDL_Rect> WorldRect(const ScriptObject& o) const;
};

std::optional<ObjectTable> ParseBITL(const std::vector<uint8_t>& d, std::string* err = nullptr);

struct BytecodeBlock {
    uint32_t totalLength = 0, splitOffset = 0;
    std::vector<uint8_t> bytes;
};
std::optional<BytecodeBlock> ParseBCOD(const std::vector<uint8_t>& d, std::string* err = nullptr);

// Event routing for one page/node. Clicks are hit-tested against hotspots of the view
// that is on screen; the handler hook receives the object and the event. No bytecode is
// run: the hook is where an interpreter will go.
enum class ScriptEvent { Tick, Click };
class ScriptRuntime {
public:
    using Handler = std::function<void(const ScriptObject&, ScriptEvent)>;
    bool Load(const Archive& a, std::string* err = nullptr);
    const ObjectTable& Objects() const { return table_; }
    const BytecodeBlock* Bytecode() const { return bcod_ ? &*bcod_ : nullptr; }
    // View whose origin is nearest the camera (circular over worldWidth).
    std::string ActiveView(double scrollX, int worldWidth) const;
    const ScriptObject* HitTest(int worldX, int worldY, const std::string& view, int worldWidth) const;
    void SetHandler(Handler h) { handler_ = std::move(h); }
    void Tick() { ++ticks_; }
    const ScriptObject* Click(int worldX, int worldY, const std::string& view, int worldWidth);
    uint64_t Ticks() const { return ticks_; }
private:
    ObjectTable table_;
    std::optional<BytecodeBlock> bcod_;
    Handler handler_;
    uint64_t ticks_ = 0;
};

} // namespace mohawk
