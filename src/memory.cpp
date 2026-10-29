#include "memory.h"
#include <cstring>

NearArena g_arena;

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParsePattern(const char* sig, Pattern& out) {
    out.len = 0;
    for (const char* s = sig; *s;) {
        if (*s == ' ') { ++s; continue; }
        if (out.len >= sizeof(out.bytes)) return false;
        if (s[0] == '?') {
            out.wild[out.len] = true;
            out.bytes[out.len++] = 0;
            s += (s[1] == '?') ? 2 : 1;
            continue;
        }
        int hi = HexVal(s[0]), lo = HexVal(s[1]);
        if (hi < 0 || lo < 0) return false;
        out.wild[out.len] = false;
        out.bytes[out.len++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return out.len > 0;
}

size_t ScanPattern(const uint8_t* start, size_t size, const Pattern& p, uint8_t** first) {
    size_t count = 0;
    if (first) *first = nullptr;
    if (size < p.len) return 0;
    // Anchor on the first non-wildcard byte for speed.
    size_t anchor = 0;
    while (anchor < p.len && p.wild[anchor]) ++anchor;
    const uint8_t* end = start + size - p.len;
    for (const uint8_t* c = start; c <= end; ++c) {
        if (c[anchor] != p.bytes[anchor]) continue;
        size_t i = 0;
        for (; i < p.len; ++i)
            if (!p.wild[i] && c[i] != p.bytes[i]) break;
        if (i == p.len) {
            if (!count && first) *first = (uint8_t*)c;
            ++count;
        }
    }
    return count;
}

uint8_t* RipTarget(uint8_t* insn, size_t dispOffset, size_t insnLen) {
    int32_t disp;
    memcpy(&disp, insn + dispOffset, 4);
    return insn + insnLen + disp;
}

bool WriteProtected(void* dst, const void* src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    return true;
}

bool BytesEqual(const void* a, std::initializer_list<uint8_t> b) {
    return memcmp(a, b.begin(), b.size()) == 0;
}

static PatchRecord g_patches[2048];
static size_t g_patchCount;

void RecordPatch(const char* name, uint8_t* site, size_t len, uint8_t* cave, size_t caveLen) {
    if (g_patchCount < _countof(g_patches)) g_patches[g_patchCount++] = {name, site, len, cave, caveLen};
}

const PatchRecord* Patches(size_t* count) {
    *count = g_patchCount;
    return g_patches;
}

bool PatchBytes(const char* name, uint8_t* site, std::initializer_list<uint8_t> expect, std::initializer_list<uint8_t> repl) {
    if (!site || expect.size() != repl.size() || !BytesEqual(site, expect)) {
        Log("  [skip] %s: unexpected bytes", name);
        return false;
    }
    bool ok = WriteProtected(site, repl.begin(), repl.size());
    FlushInstructionCache(GetCurrentProcess(), site, repl.size());
    if (ok) RecordPatch(name, site, repl.size());
    Log("  [%s] %s at %p", ok ? "ok" : "FAIL", name, site);
    return ok;
}

bool NearArena::Init(uint8_t* imageBase, size_t imageSize) {
    const size_t kSize = 0x80000;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = (uintptr_t)imageBase > 0x70000000 ? (uintptr_t)imageBase - 0x70000000 : gran;
    const uintptr_t hi = (uintptr_t)imageBase + 0x70000000;
    // Below the image first, then above it; every address stays well inside rel32 range.
    for (uintptr_t a = ((uintptr_t)imageBase & ~(gran - 1)) - gran; a >= lo; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE &&
            (base_ = (uint8_t*)VirtualAlloc((void*)a, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
            break;
    }
    if (!base_) {
        for (uintptr_t a = ((uintptr_t)imageBase + imageSize + gran) & ~(gran - 1); a < hi; a += gran) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            if (mbi.State == MEM_FREE &&
                (base_ = (uint8_t*)VirtualAlloc((void*)a, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
                break;
        }
    }
    if (!base_) return false;
    size_ = kSize;
    memset(base_, 0xCC, kSize);
    return true;
}

uint8_t* NearArena::Alloc(size_t n, size_t align) {
    size_t at = (used_ + align - 1) & ~(align - 1);
    if (!base_ || at + n > size_) return nullptr;
    used_ = at + n;
    memset(base_ + at, 0, n);
    return base_ + at;
}

void NearArena::Trim(uint8_t* p, size_t allocated, size_t used) {
    if (base_ && p + allocated == base_ + used_ && used <= allocated) used_ = (size_t)(p - base_) + used;
}

uint8_t* NearArena::Thunk(const void* target) {
    uint8_t* t = Alloc(16);
    if (!t) return nullptr;
    // jmp qword ptr [rip+0] ; dq target
    t[0] = 0xFF;
    t[1] = 0x25;
    memset(t + 2, 0, 4);
    memcpy(t + 6, &target, 8);
    return t;
}

bool RetargetRipOperand(const char* name, uint8_t* insn, size_t dispOffset, size_t insnLen,
                        std::initializer_list<uint8_t> opcode, const void* target) {
    if (!insn || !BytesEqual(insn, opcode)) {
        Log("  [skip] %s: unexpected bytes", name);
        return false;
    }
    int64_t disp = (const uint8_t*)target - (insn + insnLen);
    if (disp != (int32_t)disp) return false;
    int32_t d = (int32_t)disp;
    bool ok = WriteProtected(insn + dispOffset, &d, 4);
    FlushInstructionCache(GetCurrentProcess(), insn, insnLen);
    if (ok) RecordPatch(name, insn, insnLen);
    Log("  [%s] %s at %p", ok ? "ok" : "FAIL", name, insn);
    return ok;
}

bool RedirectCall(const char* name, uint8_t* site, const void* target) {
    if (!site || site[0] != 0xE8) {
        Log("  [skip] %s: no call at site", name);
        return false;
    }
    uint8_t* thunk = g_arena.Thunk(target);
    if (!thunk) return false;
    uint8_t patch[5] = {0xE8};
    int32_t disp = (int32_t)(thunk - (site + 5));
    memcpy(patch + 1, &disp, 4);
    bool ok = WriteProtected(site, patch, 5);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    if (ok) RecordPatch(name, site, 5, thunk, 14);
    Log("  [%s] %s at %p", ok ? "ok" : "FAIL", name, site);
    return ok;
}

bool RedirectFunction(const char* name, uint8_t* fn, std::initializer_list<uint8_t> expect, const void* target) {
    if (!fn || !BytesEqual(fn, expect)) {
        Log("  [skip] %s: unexpected bytes", name);
        return false;
    }
    uint8_t* thunk = g_arena.Thunk(target);
    if (!thunk) return false;
    uint8_t patch[5] = {0xE9};
    int32_t disp = (int32_t)(thunk - (fn + 5));
    memcpy(patch + 1, &disp, 4);
    bool ok = WriteProtected(fn, patch, 5);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    if (ok) RecordPatch(name, fn, 5, thunk, 14);
    Log("  [%s] %s at %p", ok ? "ok" : "FAIL", name, fn);
    return ok;
}
