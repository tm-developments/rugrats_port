// lb_code.h - the BCOD script interpreter.
//
// BCOD: u32 total size, u32 code size, code bytes, then string table entries
// {u16 0, NUL-terminated string}; identifiers and string literals in the code refer to the
// table by byte offset. Code is a token stream evaluated as expressions (statements end
// with 0x07, a block with 0x08). Commands: 0x4d general (u8 id, u8 argc), 0x4e item
// commands, 0x4f notify commands; ids come from the original RUGRATS.EXE's keyword table (eval=1, random=2,
// ..., setvieworigin=0x23, exec=0x3b, return=0x3c, setplayparams=0x42; item clone=1, ...,
// play=0x1a, seek=0x1b; notify click=1, goto=3, startphasemain=5, showcursor=7, quit=0xd).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lb/lb_value.h"

namespace lb {

class Engine;
class Item;

class Code {
public:
    Code(Engine* e) : vm_(e) {}
    bool Load(const std::vector<uint8_t>& bcod);
    Value Run(Item* source, uint32_t offset);
    // Compile script text ("x = 3", conditions, saved variables) into this code block;
    // returns the offset to run.
    uint32_t Compile(const std::string& text);
    std::string Disassemble(uint32_t offset, size_t maxLen = 200) const;

    std::vector<uint8_t> code_;
    std::map<uint16_t, std::string> strings_;
private:
    uint16_t AddString(const std::string& s);
    Engine* vm_;
};

} // namespace lb
