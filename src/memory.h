#pragma once
#include "common.h"
#include <cstring>
#include <initializer_list>

// Byte signature such as "8B 05 ?? ?? ?? ?? 48 8D".
struct Pattern {
    uint8_t bytes[160];
    bool wild[160];
    size_t len = 0;
};
bool ParsePattern(const char* sig, Pattern& out);
// Returns the number of matches in [start, start+size); *first receives the first one.
size_t ScanPattern(const uint8_t* start, size_t size, const Pattern& p, uint8_t** first);

// Target of a RIP-relative operand: insn + insnLen + disp32 read at insn + dispOffset.
uint8_t* RipTarget(uint8_t* insn, size_t dispOffset, size_t insnLen);

bool WriteProtected(void* dst, const void* src, size_t n);
bool BytesEqual(const void* a, std::initializer_list<uint8_t> b);

// Every applied patch is recorded (site, overwritten length, optional cave).
struct PatchRecord {
    const char* name;
    uint8_t* site;
    size_t len;
    uint8_t* cave;
    size_t caveLen;
};
void RecordPatch(const char* name, uint8_t* site, size_t len, uint8_t* cave = nullptr, size_t caveLen = 0);
const PatchRecord* Patches(size_t* count);
// Overwrites code bytes after checking the originals, and records the patch.
bool PatchBytes(const char* name, uint8_t* site, std::initializer_list<uint8_t> expect, std::initializer_list<uint8_t> repl);

// Executable memory within +-2 GB of the game image, for trampolines, code caves
// and the variables that patched game code reads with RIP-relative operands.
class NearArena {
public:
    bool Init(uint8_t* imageBase, size_t imageSize);
    uint8_t* Alloc(size_t n, size_t align = 16);
    // Gives back the unused tail of the most recent allocation (no-op otherwise).
    void Trim(uint8_t* p, size_t allocated, size_t used);
    // Returns a stub inside the arena that jumps to `target`, so rel32 calls can reach it.
    uint8_t* Thunk(const void* target);
    bool Contains(const void* p) const { return p >= base_ && p < base_ + size_; }
private:
    uint8_t* base_ = nullptr;
    size_t used_ = 0;
    size_t size_ = 0;
};
extern NearArena g_arena;

// Minimal emitter for the handful of x64 instructions the code caves need.
class Asm {
public:
    explicit Asm(uint8_t* p) : start_(p), p_(p) {}
    Asm& raw(std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) *p_++ = x;
        return *this;
    }
    Asm& i32(int32_t v) {
        memcpy(p_, &v, 4);
        p_ += 4;
        return *this;
    }
    // Copies position-independent original instructions.
    Asm& copy(const uint8_t* src, size_t n) {
        memcpy(p_, src, n);
        p_ += n;
        return *this;
    }
    // disp32 of a RIP-relative operand; `trailing` = bytes of the instruction after the disp (immediates).
    Asm& rip(const void* target, int trailing = 0) {
        return i32((int32_t)((const uint8_t*)target - (p_ + 4 + trailing)));
    }
    Asm& jmp(const void* target) { raw({0xE9}); return rip(target); }
    Asm& call(const void* target) { raw({0xE8}); return rip(target); }
    uint8_t* cur() const { return p_; }
    size_t size() const { return (size_t)(p_ - start_); }
private:
    uint8_t* start_;
    uint8_t* p_;
};

// Replaces `len` bytes at `site` with a jmp (or call) into a cave built by `emit`.
// A jmp cave must jump back itself; a call cave returns with `ret`. The caller has
// already verified the original bytes.
template <class F>
bool InstallCaveUnchecked(const char* name, uint8_t* site, size_t len, bool viaCall, F emit) {
    if (!site || len < 5 || len > 64) {
        Log("  [skip] %s: bad patch length %zu", name, len);
        return false;
    }
    const size_t kMaxCave = 1024;
    uint8_t* cave = g_arena.Alloc(kMaxCave);
    if (!cave) {
        Log("  [skip] %s: code arena full", name);
        return false;
    }
    Asm a(cave);
    emit(a);
    if (a.size() > kMaxCave) {  // would spill into the next allocation
        Log("  [FAIL] %s: cave too large (%zu bytes)", name, a.size());
        return false;
    }
    g_arena.Trim(cave, kMaxCave, a.size());
    uint8_t patch[64];
    Asm p(patch);
    p.raw({(uint8_t)(viaCall ? 0xE8 : 0xE9)}).i32((int32_t)(cave - (site + 5)));
    for (size_t i = 5; i < len; ++i) patch[i] = 0x90;
    bool ok = WriteProtected(site, patch, len);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    if (ok) RecordPatch(name, site, len, cave, a.size());
    Log("  [%s] %s at %p (cave %p, %zu bytes)", ok ? "ok" : "FAIL", name, site, cave, a.size());
    return ok;
}

// As above, but first checks that `site` holds exactly `expect` (len = expect.size()).
template <class F>
bool InstallCave(const char* name, uint8_t* site, size_t len, std::initializer_list<uint8_t> expect, F emit) {
    if (!site || expect.size() != len || !BytesEqual(site, expect)) {
        Log("  [skip] %s: unexpected bytes", name);
        return false;
    }
    return InstallCaveUnchecked(name, site, len, false, emit);
}

// Points the disp32 of the RIP-relative operand of instruction `insn` at `target`.
bool RetargetRipOperand(const char* name, uint8_t* insn, size_t dispOffset, size_t insnLen,
                        std::initializer_list<uint8_t> opcode, const void* target);

// Points a `call rel32` at `site` to `target` (through an arena thunk).
bool RedirectCall(const char* name, uint8_t* site, const void* target);
// Overwrites the start of a function with a jmp to `target` (through an arena thunk).
bool RedirectFunction(const char* name, uint8_t* fn, std::initializer_list<uint8_t> expect, const void* target);
