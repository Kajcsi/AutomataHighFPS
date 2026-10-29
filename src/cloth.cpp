// Cloth and hair physics. Cloth::Step runs once per frame with a variable dt,
// and its constraint passes, damping and velocity recompute are applied per
// step, so above 60 fps cloth turns stiffer and slower. No rescaling of the
// solver's constants can reproduce 60 Hz, so the simulation itself is kept at 60 Hz:
//  - on each 60 Hz tick the cloth takes one real 60 Hz step from its committed state;
//  - between ticks it is shown as a preview step of the elapsed fraction from that state;
//  - before every step the previous preview is rolled back word by word, but only
//    words nothing else rewrote since (the skeleton update and other systems rewrite
//    bones and inputs every frame), so external writes always survive.
// Resets (teleports, model scale jumps) are recorded by a hook and become the new
// committed state. At <= 60 fps the original Step runs unchanged.
#include "subsystems.h"
#include "patchkit.h"
#include "game.h"
#include "timing.h"
#include <cstring>

namespace {

constexpr size_t kNodeStride = 0x130;
constexpr uint32_t kMaxBones = 256;
constexpr size_t kMaxStates = 512;

using StepFn = void (*)(uint8_t* cloth, float dt, int flag);
StepFn g_origStep;
volatile uint32_t* g_resetGen;  // incremented by the reset hook (arena)
uint8_t** g_resetRing;          // last 64 reset cloth pointers (arena)

struct ClothState {
    uint8_t* cloth;
    uint8_t* nodes;
    uint32_t n;
    void* model;
    uint8_t* bones[kMaxBones];
    uint32_t nBones;
    uint32_t* commit;
    uint32_t* last;
    size_t words;
    size_t capacity;
    bool valid;
    uint64_t commitFrame;
    uint64_t lastUsed;
    uint32_t gen;
};
ClothState g_states[kMaxStates];
SRWLOCK g_lock = SRWLOCK_INIT;

uint8_t* P(uint8_t* c, int off) { return *(uint8_t**)(c + off); }
uint32_t U(uint8_t* c, int off) { return *(uint32_t*)(c + off); }

bool ResetSince(const ClothState* s) {
    uint32_t g = *g_resetGen;
    if (g - s->gen >= 64) return g != s->gen;
    for (uint32_t k = s->gen; k != g; ++k)
        if (g_resetRing[k & 63] == s->cloth) return true;
    return false;
}

// Distinct bone pointers of the node array (node+0x110).
uint32_t CollectBones(uint8_t* nodes, uint32_t n, uint8_t** out) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* b = *(uint8_t**)(nodes + i * kNodeStride + 0x110);
        if (!b) continue;
        bool seen = false;
        for (uint32_t k = 0; k < count && !seen; ++k) seen = out[k] == b;
        if (!seen) {
            if (count == kMaxBones) return kMaxBones + 1;  // too many: not tracked
            out[count++] = b;
        }
    }
    return count;
}

bool LayoutMatches(uint8_t* c, const ClothState* s) {
    if (P(c, 0x28) != s->nodes || U(c, 0x20) != s->n || (void*)P(c, 0xB10) != s->model) return false;
    uint8_t* bones[kMaxBones];
    uint32_t nb = CollectBones(s->nodes, s->n, bones);
    return nb == s->nBones && memcmp(bones, s->bones, nb * sizeof(uint8_t*)) == 0;
}

bool Rebuild(ClothState* s, uint8_t* c) {
    s->nodes = P(c, 0x28);
    s->n = U(c, 0x20);
    s->model = P(c, 0xB10);
    s->valid = false;
    s->nBones = s->nodes ? CollectBones(s->nodes, s->n, s->bones) : 0;
    if (!s->nodes || s->n > 4096 || s->nBones > kMaxBones) return false;
    size_t bytes = 0x10 + 0x900 + 0x1B0 + s->n * kNodeStride + s->nBones * 0x40;
    s->words = bytes / 4;
    if (s->words > s->capacity) {
        HANDLE heap = GetProcessHeap();
        if (s->commit) HeapFree(heap, 0, s->commit);
        if (s->last) HeapFree(heap, 0, s->last);
        s->commit = (uint32_t*)HeapAlloc(heap, 0, s->words * 4);
        s->last = (uint32_t*)HeapAlloc(heap, 0, s->words * 4);
        s->capacity = (s->commit && s->last) ? s->words : 0;
    }
    return s->capacity >= s->words;
}

// Visits the saved ranges in a fixed order: cloth [0x04,0x14), [0x50,0x950),
// [0x960,0xB10), the node array, then each bone's world matrix.
template <class F>
void ForEachRange(ClothState* s, F f) {
    size_t k = 0;
    auto range = [&](uint8_t* p, size_t bytes) {
        f((uint32_t*)p, k, bytes / 4);
        k += bytes / 4;
    };
    range(s->cloth + 0x04, 0x10);
    range(s->cloth + 0x50, 0x900);
    range(s->cloth + 0x960, 0x1B0);
    range(s->nodes, s->n * kNodeStride);
    for (uint32_t b = 0; b < s->nBones; ++b) range(s->bones[b], 0x40);
}

bool Gather(ClothState* s, uint32_t* dst) {
    __try {
        ForEachRange(s, [&](uint32_t* p, size_t at, size_t n) { memcpy(dst + at, p, n * 4); });
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Rolls back what the last preview left, keeping anything rewritten since.
bool Merge(ClothState* s) {
    __try {
        ForEachRange(s, [&](uint32_t* p, size_t at, size_t n) {
            for (size_t i = 0; i < n; ++i)
                if (p[i] == s->last[at + i]) p[i] = s->commit[at + i];
        });
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ClothState* Lookup(uint8_t* c, bool create) {
    AcquireSRWLockExclusive(&g_lock);
    ClothState *found = nullptr, *empty = nullptr, *stale = nullptr;
    for (ClothState& s : g_states) {
        if (s.cloth == c) {
            found = &s;
            break;
        }
        if (!s.cloth) {
            if (!empty) empty = &s;
        } else if (s.lastUsed + 8 < g_frame && (!stale || s.lastUsed < stale->lastUsed)) {
            stale = &s;  // not stepped for a while: its cloth is gone or inactive
        }
    }
    if (!found && create && (empty || stale)) {
        found = empty ? empty : stale;
        found->cloth = c;
        found->nodes = nullptr;
        found->valid = false;
        found->commitFrame = ~0ull;
    }
    if (found) found->lastUsed = g_frame;
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

void ClothStepHook(uint8_t* c, float dt, int flag) {
    if (!(S->step < 1.0f)) {
        // <= 60 fps: undo a pending preview, then the original step.
        if (ClothState* s = Lookup(c, false)) {
            if (s->valid && LayoutMatches(c, s) && !ResetSince(s) && !U(c, 0xC04)) Merge(s);
            s->valid = false;
        }
        g_origStep(c, dt, flag);
        return;
    }
    ClothState* s = Lookup(c, true);
    if (!s) {
        g_origStep(c, dt, flag);
        return;
    }
    bool layout = s->nodes && LayoutMatches(c, s);
    bool fresh = !s->valid || !layout || ResetSince(s) || U(c, 0xC04) != 0;
    if (!fresh && !Merge(s)) fresh = true;
    if (!layout && !Rebuild(s, c)) {
        s->valid = false;
        g_origStep(c, dt, flag);
        return;
    }
    float e = S->stepUpToOne;
    float dt60 = e > 0.0f ? dt / e : dt;
    float phase = S->tickPhase;
    bool commit = fresh || (S->ticks60 > 0 && s->commitFrame != g_frame);
    s->gen = *g_resetGen;
    g_origStep(c, commit ? dt60 : dt60 * phase, flag);
    if (ResetSince(s)) commit = true;  // a reset inside the step becomes the new base
    if (!P(c, 0xB10) || U(c, 0xC04)) {
        s->valid = false;
        return;
    }
    if (!LayoutMatches(c, s) && !Rebuild(s, c)) return;
    if (!Gather(s, s->last)) {
        s->valid = false;
        return;
    }
    if (commit) {
        memcpy(s->commit, s->last, s->words * 4);
        s->commitFrame = g_frame;
        // Show the cloth at the current time: preview the part of a 60 Hz frame past the tick.
        if (phase > 0.0f) {
            s->gen = *g_resetGen;
            g_origStep(c, dt60 * phase, flag);
            if (!P(c, 0xB10) || U(c, 0xC04) || !LayoutMatches(c, s)) {
                s->valid = false;
                return;
            }
            if (!Gather(s, s->last)) {
                s->valid = false;
                return;
            }
            if (ResetSince(s)) memcpy(s->commit, s->last, s->words * 4);
        }
    }
    s->valid = true;
    s->gen = *g_resetGen;
}

}  // namespace

void SetClothStepForTest(void (*step)(uint8_t* cloth, float dt, int flag)) { g_origStep = step; }

void FixCloth() {
    Log("cloth:");
    uint8_t* call = nullptr;
    if (uint8_t* m = FindUnique("ClothStepCall", "48 8B 8B 00 08 00 00 0F 28 C8 45 33 C0 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9"))
        call = m + 13;
    uint8_t* reset = FindUnique("ClothReset",
        "48 8B C4 53 48 83 EC 70 83 39 00 48 8B D9 0F 84 ?? ?? ?? ?? 83 79 20 00 0F 84 ?? ?? ?? ?? 48 8B 89 10 0B 00 00");
    if (!call || call[0] != 0xE8 || !reset) {
        Log("  [skip] cloth: signatures not found");
        return;
    }
    uint8_t* entry = RipTarget(call, 1, 5);
    static const uint8_t kSteam[] = {0x40, 0x55, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD8, 0xFD, 0xFF, 0xFF};
    static const uint8_t kCompat[] = {0x48, 0x8B, 0xC4, 0x44, 0x89, 0x40, 0x18};
    size_t stolen = !memcmp(entry, kSteam, sizeof kSteam) ? sizeof kSteam : !memcmp(entry, kCompat, sizeof kCompat) ? sizeof kCompat : 0;
    if (!stolen) {
        Log("  [skip] cloth: unexpected Cloth::Step prologue");
        return;
    }
    // Reset bookkeeping lives in the arena so the reset hook can reach it RIP-relative.
    g_resetGen = (volatile uint32_t*)g_arena.Alloc(4, 4);
    g_resetRing = (uint8_t**)g_arena.Alloc(64 * sizeof(uint8_t*), 8);
    uint8_t* tramp = g_arena.Alloc(32);
    if (!g_resetGen || !g_resetRing || !tramp) return;
    memcpy(tramp, entry, stolen);
    Asm(tramp + stolen).jmp(entry + stolen);
    g_origStep = (StepFn)tramp;

    if (!InstallCave("cloth reset marker", reset, 8, {0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x70}, [&](Asm& a) {
            a.raw({0xB8, 0x01, 0x00, 0x00, 0x00});                     // mov eax, 1
            a.raw({0xF0, 0x0F, 0xC1, 0x05}).rip((const void*)g_resetGen);  // lock xadd [gen], eax
            a.raw({0x83, 0xE0, 0x3F});                                 // and eax, 63
            a.raw({0x4C, 0x8D, 0x15}).rip(g_resetRing);                // lea r10, [ring]
            a.raw({0x49, 0x89, 0x0C, 0xC2});                           // mov [r10+rax*8], rcx
            a.raw({0x48, 0x8B, 0xC4, 0x53, 0x48, 0x83, 0xEC, 0x70});   // original prologue
            a.jmp(reset + 8);
        }))
        return;

    uint8_t* thunk = g_arena.Thunk((void*)&ClothStepHook);
    uint8_t patch[16];
    patch[0] = 0xE9;
    int32_t disp = (int32_t)(thunk - (entry + 5));
    memcpy(patch + 1, &disp, 4);
    memset(patch + 5, 0x90, stolen - 5);
    bool ok = WriteProtected(entry, patch, stolen);
    FlushInstructionCache(GetCurrentProcess(), entry, stolen);
    if (ok) RecordPatch("cloth step", entry, stolen, tramp, stolen + 5);
    Log("  [%s] cloth step at %p", ok ? "ok" : "FAIL", entry);
}
