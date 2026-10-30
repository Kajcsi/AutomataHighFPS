// Functional tests for subsystem patches, run by harness.exe in "functional" mode.
#include "../src/common.h"
#include "../src/memory.h"
#include "../src/game.h"
#include "../src/timing.h"
#include "../src/patchkit.h"
#include "../src/subsystems.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

static int* g_fail;
#define CHECK(cond, ...)                         \
    do {                                         \
        bool c_ = (cond);                        \
        printf("%s ", c_ ? "PASS" : "FAIL");     \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
        if (!c_) ++*g_fail;                      \
    } while (0)

static const PatchRecord* FindPatch(const char* name) {
    size_t n;
    const PatchRecord* r = Patches(&n);
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(r[i].name, name)) return &r[i];
    return nullptr;
}

// Simulates `frames` frames of `seconds` total at a constant frame time through the per-frame hook.
static void SimulateFrame(double fps) {
    alignas(16) static uint8_t obj[0x100];
    LARGE_INTEGER now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    *(float*)(obj + 0xB8) = 0.06f;
    *(int64_t*)(obj + 0xA8) = now.QuadPart - (int64_t)(freq.QuadPart / fps);
    OnUpdateTimeStep(obj, 1, 1.0f);
}

// Fake object for running patched sites (RunSite). Game fields live below 0x3E000;
// rcx/rdx/r8 point at three vectors, xmm0-15 are loaded from kXmmIn and stored to kXmmOut.
const size_t kObjSize = 0x40000, kVecRcx = 0x3E000, kVecRdx = 0x3E040, kVecR8 = 0x3E080, kXmmIn = 0x3F000,
             kXmmOut = 0x3F100;
// With the int at kRdxFlag set, the 16 bytes rdx points at after the site are copied to kRdxOut.
const size_t kRdxFlag = 0x3E100, kRdxOut = 0x3E200;
const uint8_t* g_pristineImage;  // unpatched copy of the mapped executable (set by harness.cpp)

// Runs the code at `site` up to `site + len` (where a temporary jmp returns here) in a
// function-like frame (rsp 16-byte aligned, 0x100 bytes of locals) with rbx = rsi = rdi =
// r12-r15 = obj, rbp = obj+0x20000, rcx/rdx/r8 = obj vectors and eax = eaxIn; returns eax.
static int RunSite(uint8_t* site, size_t len, uint8_t* obj, int eaxIn) {
    static uint8_t* thunk;
    static uint8_t* back;
    static uint8_t** objSlot;
    if (!thunk) {
        objSlot = (uint8_t**)g_arena.Alloc(8, 8);
        thunk = g_arena.Alloc(1024);
        Asm a(thunk);
        a.raw({0x53, 0x56, 0x57, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push rbx..r15
        a.raw({0x48, 0x81, 0xEC}).i32(0x1A8);                                          // sub rsp, 0x1A8
        for (uint8_t i = 6; i < 16; ++i) {                                            // movups [rsp+0x100+..], xmm6-15
            if (i >= 8) a.raw({0x44});
            a.raw({0x0F, 0x11, (uint8_t)(0x84 | (i & 7) << 3), 0x24}).i32(0x100 + 16 * (i - 6));
        }
        a.raw({0x44, 0x89, 0xC0});                                                    // mov eax, r8d
        a.raw({0x49, 0x89, 0xCB});                                                    // mov r11, rcx (site)
        a.raw({0x48, 0x89, 0xD3, 0x48, 0x89, 0xD6, 0x48, 0x89, 0xD7});                // mov rbx/rsi/rdi, rdx
        a.raw({0x49, 0x89, 0xD4, 0x49, 0x89, 0xD5, 0x49, 0x89, 0xD6, 0x49, 0x89, 0xD7});  // mov r12-r15, rdx
        a.raw({0x48, 0x8D, 0xAA}).i32(0x20000);                                       // lea rbp, [rdx+0x20000]
        a.raw({0x48, 0x8D, 0x8A}).i32((int32_t)kVecRcx);                              // lea rcx, [rdx+vec]
        a.raw({0x4C, 0x8D, 0x82}).i32((int32_t)kVecR8);                               // lea r8, [rdx+vec]
        a.raw({0x48, 0x8D, 0x92}).i32((int32_t)kVecRdx);                              // lea rdx, [rdx+vec]
        for (uint8_t i = 0; i < 16; ++i) {                                            // movss xmmI, [rbx+in+4i]
            if (i >= 8) a.raw({0xF3, 0x44, 0x0F, 0x10}); else a.raw({0xF3, 0x0F, 0x10});
            a.raw({(uint8_t)(0x83 | (i & 7) << 3)}).i32((int32_t)(kXmmIn + 4 * i));
        }
        a.raw({0x41, 0xFF, 0xE3});                                                    // jmp r11
        back = a.cur();
        a.raw({0x4C, 0x8B, 0x1D}).rip(objSlot);                                       // mov r11, [objSlot]
        for (uint8_t i = 0; i < 16; ++i) {                                            // movss [r11+out+4i], xmmI
            a.raw({0xF3, (uint8_t)(i >= 8 ? 0x45 : 0x41), 0x0F, 0x11});
            a.raw({(uint8_t)(0x83 | (i & 7) << 3)}).i32((int32_t)(kXmmOut + 4 * i));
        }
        a.raw({0x41, 0x83, 0xBB}).i32((int32_t)kRdxFlag).raw({0x00});                 // cmp dword [r11+flag], 0
        a.raw({0x74, 0x0B});                                                          // je skip
        a.raw({0x0F, 0x10, 0x02});                                                    // movups xmm0, [rdx]
        a.raw({0x41, 0x0F, 0x11, 0x83}).i32((int32_t)kRdxOut);                        // movups [r11+out], xmm0
        // skip:
        for (uint8_t i = 6; i < 16; ++i) {                                            // movups xmm6-15, [rsp+0x100+..]
            if (i >= 8) a.raw({0x44});
            a.raw({0x0F, 0x10, (uint8_t)(0x84 | (i & 7) << 3), 0x24}).i32(0x100 + 16 * (i - 6));
        }
        a.raw({0x48, 0x81, 0xC4}).i32(0x1A8);                                         // add rsp, 0x1A8
        a.raw({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5F, 0x5E, 0x5B, 0xC3});  // pop ...; ret
    }
    *objSlot = obj;
    uint8_t saved[5], jmp[5] = {0xE9};
    memcpy(saved, site + len, 5);
    *(int32_t*)(jmp + 1) = (int32_t)(back - (site + len + 5));
    WriteProtected(site + len, jmp, 5);
    int r = ((int (*)(uint8_t*, uint8_t*, int))thunk)(site, obj, eaxIn);
    WriteProtected(site + len, saved, 5);
    return r;
}

// Deterministic pseudo-random state: game fields and vectors get floats in [-2, 2) (no
// pointers), xmm inputs likewise.
static void FillState(uint8_t* obj, uint32_t seed) {
    memset(obj, 0, kObjSize);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < kObjSize / 4; ++i) {
        if (i * 4 >= kVecRcx && i * 4 < kXmmIn) continue;
        x = x * 1664525u + 1013904223u;
        ((float*)obj)[i] = (float)(x >> 8) / (float)(1 << 24) * 4.0f - 2.0f;
    }
    for (size_t i = 0; i < 12; ++i) {
        x = x * 1664525u + 1013904223u;
        ((float*)(obj + kVecRcx))[i] = (float)(x >> 8) / (float)(1 << 24) * 4.0f - 2.0f;
    }
    for (size_t i = 0; i < 16; ++i) {
        x = x * 1664525u + 1013904223u;
        ((float*)(obj + kXmmIn))[i] = (float)(x >> 8) / (float)(1 << 24) * 4.0f - 2.0f;
    }
}

// What a test fixes before running a site: SSE registers the game guarantees at that point
// (e.g. a register holding 0.1), memory fields, and the SSE registers the site's code leaves
// dead (not compared).
struct SiteSetup {
    std::vector<std::pair<int, float>> xmm;
    std::vector<std::pair<size_t, float>> mem;
    uint32_t deadXmm = 0;  // bit i: xmmI is dead after the site
};

static void ApplySetup(uint8_t* obj, const SiteSetup& st) {
    for (auto& x : st.xmm) ((float*)(obj + kXmmIn))[x.first] = x.second;
    for (auto& m : st.mem) *(float*)(obj + m.first) = m.second;
}

// At 60 fps (e = 1, one 60 Hz tick) a patched site must do exactly what the original game
// bytes do: same live registers, same memory, same eax.
static void CheckSameAt60(const char* name, const SiteSetup& st = {}, uint32_t seeds = 3) {
    const PatchRecord* r = FindPatch(name);
    if (!r) {
        CHECK(false, "%s patch present", name);
        return;
    }
    static uint8_t* a = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    static uint8_t* b = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const uint8_t* orig = g_pristineImage + (r->site - G.image);
    uint8_t patched[64];
    memcpy(patched, r->site, r->len);
    bool same = true;
    size_t at = 0;
    for (uint32_t seed = 1; seed <= seeds && same; ++seed) {
        FillState(a, seed);
        ApplySetup(a, st);
        int ea = RunSite(r->site, r->len, a, (int)seed * 7);
        FillState(b, seed);
        ApplySetup(b, st);
        WriteProtected(r->site, orig, r->len);
        int eb = RunSite(r->site, r->len, b, (int)seed * 7);
        WriteProtected(r->site, patched, r->len);
        for (int i = 0; i < 16; ++i)
            if (st.deadXmm >> i & 1) ((float*)(a + kXmmOut))[i] = ((float*)(b + kXmmOut))[i];
        if (ea != eb) same = false, at = (size_t)-1;
        for (size_t i = 0; same && i < kObjSize; ++i)
            if (a[i] != b[i]) same = false, at = i;
    }
    CHECK(same, "%s identical to the game's code at 60 fps", name);
    if (!same && at != (size_t)-1)
        printf("     at obj+0x%zx: patched %08x, original %08x\n", at & ~(size_t)3, *(uint32_t*)(a + (at & ~3)),
               *(uint32_t*)(b + (at & ~3)));
}

// Runs `fn` with the frame hook at the 60 fps limit (e = 1, one 60 Hz tick per frame).
template <class F>
static void At60(F fn) {
    int savedTarget = g_cfg.targetFps;
    g_cfg.targetFps = 60;
    SimulateFrame(61);
    SimulateFrame(61);
    fn();
    g_cfg.targetFps = savedTarget;
    SimulateFrame(144);
}

static void SameAt60Tests() {
    At60([] {
        CheckSameAt60("BLT_ArcGravity", {{}, {}, 1u << 1});
        CheckSameAt60("BLT_HomingTurn");
        CheckSameAt60("EM_TimeOverrideCtr");
        CheckSameAt60("CAM_PLC1_ANGVEL");
        CheckSameAt60("EM_Move_YawLerp_275C4", {{{10, 0.1f}}});
        CheckSameAt60("AN_Pl_St02_RootXZ1A7C_Lerp", {{{6, 0.1f}}});
        CheckSameAt60("EX_TurnFE650_Frac");
    });
}

// Same check over the code from patch `first` to the end of patch `last` (sites that only
// work as a pair, e.g. a value moved from one site to another).
static void CheckSameAt60Range(const char* first, const char* last, const SiteSetup& st = {}) {
    const PatchRecord* a0 = FindPatch(first);
    const PatchRecord* a1 = FindPatch(last);
    if (!a0 || !a1 || a1->site < a0->site) {
        CHECK(false, "%s..%s patches present", first, last);
        return;
    }
    static uint8_t* a = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    static uint8_t* b = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    size_t len = (size_t)(a1->site + a1->len - a0->site);
    uint8_t* saved = (uint8_t*)malloc(len);
    memcpy(saved, a0->site, len);
    bool same = true;
    for (uint32_t seed = 1; seed <= 3 && same; ++seed) {
        FillState(a, seed);
        ApplySetup(a, st);
        RunSite(a0->site, len, a, 0);
        FillState(b, seed);
        ApplySetup(b, st);
        WriteProtected(a0->site, g_pristineImage + (a0->site - G.image), len);
        RunSite(a0->site, len, b, 0);
        WriteProtected(a0->site, saved, len);
        for (int i = 0; i < 16; ++i)
            if (st.deadXmm >> i & 1) ((float*)(a + kXmmOut))[i] = ((float*)(b + kXmmOut))[i];
        same = memcmp(a, b, kObjSize) == 0;
    }
    free(saved);
    CHECK(same, "%s..%s identical to the game's code at 60 fps", first, last);
}

// Runs a patched site on `obj` (state set by the caller) and returns the xmm outputs.
static float* RunNamed(const char* name, uint8_t* obj) {
    const PatchRecord* r = FindPatch(name);
    if (!r) return nullptr;
    RunSite(r->site, r->len, obj, 0);
    return (float*)(obj + kXmmOut);
}
static bool Near(float a, float b) { return fabsf(a - b) <= 1e-5f * fmaxf(1.0f, fabsf(b)); }

static void MiscFinalTests() {
    At60([] {
        for (const char* n : {"POD_LaserBeamHitTimerTick", "POD_LerpRate_RampExact", "PL_17640_LerpExact", "PL_17640_DecayExact",
                              "PL_BuddyPushOut_Exact", "PL_16AF0_LerpExact", "PL_CarryLocalY_LerpExact",
                              "PL_2DOffsetX_DecayExact", "PL_2DOffsetY_DecayExact", "PL_2DOffsetZ_DecayExact",
                              "PL_16C20_IntegrateExact", "PL_16C20_DecayExact", "FISH_850_IntegrateExact", "FISH_850_DecayExact",
                              "W9S_SpinRateExact", "A2_EvadeTurnLerp", "EXBZ_Slot6_LerpExact", "EXBZ_Slot7_LerpExact",
                              "EXBZ_Slot8_LerpExact", "FISH_BobberJitterTick", "ESP10_CollideIntervalTicks",
                              "ESP32_CollideIntervalTicks", "KB_SlideIntegrateExact", "KB_SlideDecayExact"})
            CheckSameAt60(n);
    });

    // 144 fps: each cave against the closed form its analysis derives (normal speed, d = e).
    SimulateFrame(144);
    float e = S->stepUpToOne;
    auto lerpE = [&](float k) { return 1.0f - powf(1.0f - k, e); };
    static uint8_t* o = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* in = (float*)(o + kXmmIn);
    float* src = (float*)(o + kVecRdx);
    float* dst = (float*)(o + kVecRcx);
    auto reset = [&] { FillState(o, 9); *(float*)(o + 0x1470) = e; };

    reset(); in[0] = 2.0f;
    float* x = RunNamed("PL_17640_LerpExact", o);
    CHECK(x && Near(x[0], 2.0f * lerpE(0.05f)), "player 0x17640 lerp exact at 144 fps (%.6f)", x ? x[0] : 0.f);
    reset(); in[0] = 3.0f; in[1] = 3.0f;
    x = RunNamed("PL_17640_DecayExact", o);
    CHECK(x && Near(x[0], 3.0f * lerpE(0.05f)), "player 0x17640 decay exact at 144 fps (%.6f)", x ? x[0] : 0.f);
    reset(); in[0] = 2.0f; in[7] = e;
    x = RunNamed("EXBZ_Slot6_LerpExact", o);
    CHECK(x && Near(x[0], 2.0f * lerpE(0.1f)), "ExBezierPath slot 6 lerp exact at 144 fps (%.6f)", x ? x[0] : 0.f);
    reset(); in[9] = 0.1f; in[1] = 4.0f;  // xmm9 holds the game's 0.1 here
    x = RunNamed("PL_CarryLocalY_LerpExact", o);
    CHECK(x && Near(x[1], 4.0f * lerpE(0.1f)), "carried object height lerp exact at 144 fps (%.6f)", x ? x[1] : 0.f);

    // 9S weapon spin: v' = v + (t-v)F, angle step = d*t - (t-v)*0.9*F/0.1.
    reset(); in[1] = 0.5f; in[6] = -1.0f; in[0] = e;
    x = RunNamed("W9S_SpinRateExact", o);
    {
        float F = lerpE(0.1f), v1 = 0.5f + (-1.0f - 0.5f) * F, step = e * -1.0f - (-1.0f - 0.5f) * 0.9f * F / 0.1f;
        CHECK(x && Near(x[6], v1) && Near(*(float*)(o + 0x3B30), v1) && Near(x[0], step),
              "9S weapon spin exact at 144 fps (rate %.5f/%.5f, step %.5f/%.5f)", x ? x[6] : 0.f, v1, x ? x[0] : 0.f, step);
    }
    // A2 evade: b exact, rate fraction LerpE of b half a tick ahead.
    reset(); in[1] = 0.04f; in[2] = 0.06f; in[7] = 0.1f; in[4] = 1.5f;
    x = RunNamed("A2_EvadeTurnLerp", o);
    {
        float b1 = 0.04f + 0.06f * lerpE(0.1f);
        float lead = 0.1f - (0.1f - b1) * 0.9486833f / powf(0.9486833f, e);
        float want = 1.5f * lerpE(fminf(lead, 1.0f));
        CHECK(x && Near(*(float*)(o + 0x1A88), b1) && Near(x[4], want), "A2 evade turn exact at 144 fps (%.6f/%.6f)",
              x ? x[4] : 0.f, want);
    }
    // 2D offset: x' = Fa*x - v((Fb-Fa)/(kb-ka) - Fb/kb) (the amount subtracted from x).
    reset(); in[1] = 1.0f; in[0] = 1.0f; in[2] = e; *(float*)(o + 0x16C20) = 0.5f;
    x = RunNamed("PL_2DOffsetX_DecayExact", o);
    {
        float Fa = lerpE(0.025f), Fb = lerpE(0.03f);
        float want = Fa * 1.0f - 0.5f * ((Fb - Fa) / (0.03f - 0.025f) - Fb / 0.03f);
        CHECK(x && fabsf(x[0] - want) < 2e-5f, "2D offset decay with velocity coupling at 144 fps (%.6f/%.6f)",
              x ? x[0] : 0.f, want);
    }
    // Knockback slide: decay speed*(1-k)^e, integration weight F/k.
    reset(); in[0] = 2.0f; in[1] = 2.0f * 0.9f - 2.0f; in[6] = e; *(float*)(o + 0xA84) = 0.9f;
    x = RunNamed("KB_SlideDecayExact", o);
    CHECK(x && Near(x[1], 2.0f * powf(0.9f, e)), "knockback slide decay exact at 144 fps (%.6f)", x ? x[1] : 0.f);
    reset(); in[2] = e; *(float*)(o + 0xA84) = 0.9f; src[0] = 1.0f; src[1] = 2.0f; src[2] = 3.0f;
    RunNamed("KB_SlideIntegrateExact", o);
    {
        float W = lerpE(0.1f) / 0.1f;
        CHECK(Near(dst[0], W) && Near(dst[1], 2 * W) && Near(dst[2], 3 * W), "knockback slide step weight at 144 fps (%.6f/%.6f)",
              dst[0], W);
    }
    // Player velocity decay pair.
    reset(); in[0] = 0.03f * e; src[0] = 1.0f;
    RunNamed("PL_16C20_DecayExact", o);
    CHECK(Near(dst[0], powf(0.97f, e)), "player 0x16C20 velocity decay exact at 144 fps (%.6f)", dst[0]);
    reset(); in[2] = e; src[0] = 1.0f;
    RunNamed("PL_16C20_IntegrateExact", o);
    CHECK(Near(dst[0], lerpE(0.03f) / 0.03f), "player 0x16C20 step weight at 144 fps (%.6f)", dst[0]);
    // Fish: vel factor X = 0.05(1-F)/(F*m).
    reset(); in[0] = 0.05f * e; in[6] = 2.0f; src[0] = 1.0f;
    RunNamed("FISH_850_DecayExact", o);
    {
        float F = lerpE(0.05f), X = 0.05f * (1 - F) / (F * 2.0f);
        CHECK(Near(dst[0], X), "fish velocity decay with re-derived velocity at 144 fps (%.6f/%.6f)", dst[0], X);
    }
    // Effect collision interval counts 60 Hz ticks.
    for (int t = 0; t < 2; ++t) {
        reset(); in[1] = 3.0f; S->ticks60 = t; S->ticks60f = (float)t;
        x = RunNamed("ESP10_CollideIntervalTicks", o);
        CHECK(x && x[1] == 3.0f - t, "effect collision interval on %s (%.3f)", t ? "a 60 Hz tick" : "a frame without a tick", x ? x[1] : 0.f);
    }
    // Pod B beam hit timer: on a tick it runs down by the tick's delta (d/e = 1).
    reset(); S->ticks60 = 1; in[1] = e; *(float*)(o + 0xCF8) = 6.0f;
    RunNamed("POD_LaserBeamHitTimerTick", o);
    CHECK(Near(*(float*)(o + 0xCF8), 5.0f), "Pod B beam hit timer counts 60 Hz ticks (%.4f)", *(float*)(o + 0xCF8));
    SimulateFrame(144);
}

static void CameraFinalTests(uint8_t* img) {
    // GetDelta with no node reads the global time object's +0xA4: point it at our own.
    alignas(16) static uint8_t timeObj[0x200];
    void*& timeObjPtr = *(void**)(img + 0x176DEA0);
    void* savedTimeObj = timeObjPtr;
    timeObjPtr = timeObj;
    *(void**)G.deltaHandle = nullptr;
    *(float*)(timeObj + 0xA4) = 1.0f;

    At60([] {
        CheckSameAt60("CAM_PLC1_ANGVEL");
        CheckSameAt60Range("CAM_PLC2_ANGVEL_P", "CAM_PLC2_ANGVEL_Y");
        CheckSameAt60Range("CAM_PLC3_ANGVEL_P", "CAM_PLC3_ANGVEL_Y");
        for (const char* n : {"CAM_PLC1_DECAY_P", "CAM_PLC1_DECAY_Y", "CAM_PLC2_DECAY_P", "CAM_PLC2_DECAY_Y",
                              "CAM_PLC3_DECAY_P", "CAM_PLC3_DECAY_Y", "CAM_GAIN_OFS_Y", "CAM_GAIN_M29_Y", "CAM_GAIN_SIDE_Y",
                              "CAM_GAIN_SIDE_DIST", "CAM_GAIN_M0E_ROLL", "CAM_M2A_3B0_EXACT", "CAM_M2A_3E4_EXACT",
                              "CAM_M29_3B0_EXACT", "CAM_M18_39C_STATE", "CAM_DEF_YAWAUTO_FRAC", "CAM_RECENTER_PITCH",
                              "CAM_RECENTER_YAW", "CAM_DEF_YAWAUTO_CALL"})
            CheckSameAt60(n);
        CheckSameAt60("CAM_PLC1_LINEND", {{{8, 0.0f}}, {}, 1u << 3});  // original compares with xmm8 = 0.0; xmm3 scratch
        CheckSameAt60("CAM_PLC2_LINEND", {{{8, 0.0f}}, {}, 1u << 3});
        CheckSameAt60("CAM_PLC3_LINEND", {{{7, 0.0f}}, {}, 1u << 3});
    });

    SimulateFrame(144);
    float e = S->stepUpToOne;
    *(float*)(timeObj + 0xA4) = e;  // normal speed: d = e
    static uint8_t* o = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* in = (float*)(o + kXmmIn);
    auto F = [&](uint32_t off) -> float& { return *(float*)(o + off); };
    // Angular impulse, exponential phase: angle += W*v, W = (1-0.5^e)/0.5.
    FillState(o, 4);
    F(0x4BC) = 0.0f; F(0x3F0) = 1.0f; F(0x4AC) = 0.2f; F(0x3F4) = 2.0f; F(0x4B0) = -0.4f;
    float* x = RunNamed("CAM_PLC1_ANGVEL", o);
    {
        float W = (1.0f - powf(0.5f, e)) / 0.5f;
        CHECK(x && Near(x[0], 1.0f + W * 0.2f) && Near(x[1], 2.0f - W * 0.4f),
              "camera impulse weight at 144 fps (pitch %.6f/%.6f)", x ? x[0] : 0.f, 1.0f + W * 0.2f);
    }
    // Linear phase: angle += e*v + (1-e)/2*d*a.
    FillState(o, 5);
    F(0x4BC) = 10.0f; F(0x3F0) = 1.0f; F(0x4AC) = 0.2f; F(0x4B4) = 0.01f; F(0x3F4) = 2.0f; F(0x4B0) = -0.4f; F(0x4B8) = 0.02f;
    x = RunNamed("CAM_PLC1_ANGVEL", o);
    {
        float c = (1.0f - e) * 0.5f * e;
        CHECK(x && Near(x[0], 1.0f + 0.01f * c + 0.2f * e) && Near(x[1], 2.0f + 0.02f * c - 0.4f * e),
              "camera impulse linear phase at 144 fps (pitch %.6f)", x ? x[0] : 0.f);
    }
    // Decay: v *= (1 - 0.5)^e with xmm0 = 0.5*d.
    FillState(o, 6);
    in[0] = 0.5f * e; in[9] = 1.0f; F(0x4AC) = 0.3f;
    x = RunNamed("CAM_PLC1_DECAY_P", o);
    CHECK(x && Near(x[1], 0.3f * powf(0.5f, e)), "camera impulse decay at 144 fps (%.6f)", x ? x[1] : 0.f);
    // Gain with the transition blend running: xmm1 *= LerpE(0.3X)/LerpE(X), X = [0x698].
    FillState(o, 7);
    in[1] = 2.0f; F(0x698) = 0.4f; *(int*)(o + 0x224) = 1;
    x = RunNamed("CAM_GAIN_OFS_Y", o);
    {
        float lx = 1 - powf(0.6f, e), lg = 1 - powf(1 - 0.3f * 0.4f, e);
        CHECK(x && Near(x[1], 2.0f * lg / lx), "camera gain composed with the blend at 144 fps (%.6f/%.6f)", x ? x[1] : 0.f,
              2.0f * lg / lx);
    }
    // Gain with the follow factor: X = min(d*[0x210]/e, 1).
    FillState(o, 8);
    in[1] = 2.0f; F(0x210) = 0.1f; *(int*)(o + 0x224) = 0;
    x = RunNamed("CAM_GAIN_OFS_Y", o);
    {
        float lx = 1 - powf(0.9f, e), lg = 1 - powf(1 - 0.03f, e);
        CHECK(x && Near(x[1], 2.0f * lg / lx), "camera gain composed with the follow factor at 144 fps (%.6f/%.6f)",
              x ? x[1] : 0.f, 2.0f * lg / lx);
    }
    // M18 follow factor state: multiplied by (d/e)^e = 1 at normal speed, not by d.
    FillState(o, 9);
    in[0] = e; F(0x39C) = 0.1f;
    x = RunNamed("CAM_M18_39C_STATE", o);
    CHECK(x && Near(x[0], 0.1f), "camera mode M18 follow factor kept at 144 fps (%.6f, was %.6f)", x ? x[0] : 0.f, 0.1f * e);
    // M2A distance lerp.
    FillState(o, 10);
    in[0] = e; in[1] = 3.0f; in[7] = 0.01f;
    x = RunNamed("CAM_M2A_3B0_EXACT", o);
    CHECK(x && Near(x[0], 3.0f * (1 - powf(0.99f, e))), "camera mode M2A distance lerp exact at 144 fps (%.6f)", x ? x[0] : 0.f);

    timeObjPtr = savedTimeObj;
    SimulateFrame(144);
}

static void EnemyFinalTests() {
    At60([] {
        for (const char* n :
             {"EF_Blt_ExactGravityPos_5EDC16", "EF_Blt_RingBombMoveExact_6E4E58", "EF_Blt_SpinRateExact_5ED66E",
              "EF_Blt_SpinDecayExact_5ED68A", "EF_Blt_ScaleLerpExact_6E4C91", "EF_Blt_EB0RotFrac_6B4F88",
              "EF_Blt_M37FrameTurn_675467", "EF_Blt_M43HomingTurn_674732", "EF_Em4000_BeamGrow_53AB57",
              "EF_Em4000_BeamGrowB_53AEB8", "EF_Em4000_BeamTiltLerp_53AC32", "EF_Em0000_SpeedDecay275C0_5429B6",
              "EF_Em0000_SpeedDecay275C0B_53E81C", "EF_Em0000_Decay275BC_5463E0", "EF_Em0000_PitchLerpExact_555DA2",
              "EF_Em0000_PitchLerpExactB_556CEE", "EF_Em0000_HeightLerpExact_566EA1", "EF_Em0000_FallVyExact_566820",
              "EF_Em0000_TiltLerpExact_53F6BE", "EF_Em0000_SlideStepExact_544E38", "EF_Em0000_OrbitAngle_561FB5",
              "EF_EmBase_DriftFrames_689F5C", "EF_Em0030_ApprMax_552B80", "EF_Em5000FF_SpinVelExact_686479",
              "EF_Em5000FF_PartLerpExact_686A25", "EF_Em5000FF_PartLerpExactB_686A61", "EF_Em5000FF_SinkStep_68669D",
              "EF_Em8800_LerpExact_687A92", "EF_Em8800_LerpExactB_687AC6", "EF_Em0080_BoneLerpExact_6C5095",
              "EF_Em1000_BlendExactA_6ABAF2", "EF_Em1000_BlendExactD_6ABD86", "EF_Em1000_BlendExactB_6ABCCC",
              "EF_Em1000_BlendExactC_6ABD19", "EF_EmHover_PullWeight_53BC91", "EF_EmHover_VelDecay_53BCC0",
              "EF_EmHover_VelDecay_53BD07", "EF_EmHover_PullWeight_53CCE6", "EF_EmHover_VelDecay_53CD13",
              "EF_EmHover_VelDecay_53CD57", "EF_EmHover_PullWeight_5423AC", "EF_EmHover_VelDecay_5423D8",
              "EF_EmHover_VelDecay_54241E", "EF_EmHover_PullWeight_5435B8", "EF_EmHover_VelDecay_5435E5",
              "EF_EmHover_VelDecay_543629", "EF_EmHover_PullWeight_558C52", "EF_EmHover_VelDecay_558C81",
              "EF_EmHover_VelDecay_558CC8", "EF_EmHover_PullWeight_5B6297", "EF_EmHover_VelDecay_5B62C3",
              "EF_EmHover_VelDecay_5B6308", "EF_Em2001_HoverPullWeight_5B9DCF", "EF_Em2001_HoverVelDecay_5B9DF6",
              "EF_Em8000_DriftDecayExact_5CC74D", "EF_Em8000_RotorBoneLerpExact_6BABAD",
              "EF_Em8000_RotorBoneLerpExact_6BABF9", "EF_Em5000_DeltaLerpExact_6854F0", "EF_Em9000_DeltaLerpExact_688639",
              "EF_Em9010_DeltaLerpExact_688BF5", "EF_Em7000_HeightLerpExact_6A4E07", "EF_Em2000_DashMoveExact_5B6D98",
              "EF_Em2100Parts_SpinX_5BA332", "EF_Em2100Parts_SpinY_5BA34F", "EF_Em2100Parts_GatherRotFrac_5BA58F",
              "EF_Em2100Parts_GatherRotFrac_5BA653", "EF_Em2100Parts_GravityExact_682E36", "EF_Em4000Core_BobLerp_68431D",
              "EF_Em4000Core_SpinStep_6B8E41", "EF_Em8800Parts_GravityExact_688248", "EF_Em8800Parts_HomeRotFrac_5CE57B",
              "EF_Em8800Parts_HomeRotFrac_5CE63F", "EF_Em8800Parts_HomePosExact_5CE6FB",
              "EF_Em5000FloatRubble_FollowExact_5FA6E8", "EF_Em011e_StopDecayExact_67EBE6", "EF_Em011e_PosStep_67EBFA",
              "EF_Em0030Parts_HeightLerpExact_6BB124", "EF_Em5000_LevelDebounce_685803", "EF_Ba0420_Wait9F0_452F0C",
              "EF_Ba0420_Fade9F0_47393E", "EF_BaAnimal_ContactFrames_425421", "EF_Baa000_StateFrames_468082",
              "EF_Baa010_StateFrames_46D093"})
            CheckSameAt60(n);
        // A register holding the game's constant is replaced by the scaled copy of that constant.
        struct { const char* n; int reg; float v; } withReg[] = {
            {"EF_Em0000_SpeedLerp12_55BBEB", 10, 0.1f}, {"EF_Em0000_SpeedLerp_564B4B", 8, 0.1f},
            {"EF_Em0000_DriftXLerp_53F463", 7, 0.1f},   {"EF_Em0000_DriftXDecay_53F487", 7, 0.1f},
            {"EF_Em0000_HoverPullLerp_555B63", 9, 0.1f},
            {"EF_Em0000_HoverPullLerp_556AB3", 9, 0.1f}, {"EF_Em0000_PullTimer_5475F1", 6, 1.0f},
            {"EF_Em0000_PullTimer_550DA7", 6, 1.0f},    {"EF_Em0000_FadeNear_5EE52E", 6, 0.1f},
            {"EF_Em0000_FadeIn_5EE53C", 6, 0.1f},       {"EF_Baa000_TurnRateRamp_5025F7", 7, 0.01f},
            {"EF_Em1100_CamDistLerp_681045", 7, 0.01f}, {"EF_Em5000_HoverPullLerp_5C3EC7", 6, 0.1f},
            {"EF_Em5000_HoverPullLerp_5C4CEF", 9, 0.1f}, {"EF_Em2000_GlowLerp_6B0D68", 0, 0.1f},
            {"EF_Em2100Parts_ScaleLerp_5BA324", 6, 0.3f}, {"EF_Em0030Parts_YawLerp_6BB072", 6, 0.1f},
        };
        for (auto& w : withReg) CheckSameAt60(w.n, {{{w.reg, w.v}}});
        CheckSameAt60("EF_Em4000_BeamSweepStep_53AF8B", {{}, {}, 1u << 4});  // xmm4 scratch
        // Stores r15d (a pointer in the test), so compared directly: xmm2 = xmm8's 0.1, store kept.
        static uint8_t* m = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        FillState(m, 1);
        ((float*)(m + kXmmIn))[8] = 0.1f;
        float* x = RunNamed("EF_Em0000_MoveByLerp_5654E6", m);
        CHECK(x && x[2] == 0.1f && *(uint32_t*)(m + 0x20000 - 0x75) == (uint32_t)(uintptr_t)m,
              "EF_Em0000_MoveByLerp_5654E6 identical to the game's code at 60 fps");
    });

    // 144 fps: each pattern against its closed form (normal speed, d = e).
    SimulateFrame(144);
    float e = S->stepUpToOne;
    auto lerpE = [&](float k) { return 1.0f - powf(1.0f - k, e); };
    static uint8_t* o = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* in = (float*)(o + kXmmIn);
    auto F = [&](uint32_t off) -> float& { return *(float*)(o + off); };
    auto I = [&](uint32_t off) -> int& { return *(int*)(o + off); };

    FillState(o, 1); in[2] = 2.0f; F(0xC84) = e;
    float* x = RunNamed("EF_Em5000_DeltaLerpExact_6854F0", o);
    CHECK(x && Near(x[2], 2.0f * lerpE(0.1f)), "enemy k*d lerp (constant k) exact at 144 fps (%.6f)", x ? x[2] : 0.f);
    FillState(o, 2); in[0] = 2.0f; in[7] = 0.1f; F(0xC84) = e;
    x = RunNamed("EF_Em0000_TiltLerpExact_53F6BE", o);
    CHECK(x && Near(x[0], 2.0f * lerpE(0.1f)), "enemy k*d lerp (register k) exact at 144 fps (%.6f)", x ? x[0] : 0.f);
    FillState(o, 3); in[0] = e; in[1] = 3.0f;
    x = RunNamed("EF_Em8800_LerpExact_687A92", o);
    CHECK(x && Near(x[0], 3.0f * lerpE(0.025f)), "enemy k*GetDelta lerp exact at 144 fps (%.6f)", x ? x[0] : 0.f);
    FillState(o, 4); in[3] = 2.0f; in[9] = 0.1f; F(0xC84) = e;
    x = RunNamed("EF_Em1000_BlendExactB_6ABCCC", o);
    CHECK(x && Near(x[3] * e, 2.0f * lerpE(0.1f)), "Em1000 blend factor exact at 144 fps (%.6f)", x ? x[3] * e : 0.f);
    FillState(o, 5); in[1] = 1.0f; in[0] = 0.1f * e; F(0x275C0) = 2.0f;
    x = RunNamed("EF_Em0000_SpeedDecay275C0_5429B6", o);
    CHECK(x && Near(x[1], 2.0f * powf(0.9f, e)), "enemy speed decay exact at 144 fps (%.6f)", x ? x[1] : 0.f);
    FillState(o, 6); in[0] = 2.0f; F(0x2AF80) = 1.0f; in[6] = 0.2f;
    x = RunNamed("EF_Em0000_OrbitAngle_561FB5", o);
    CHECK(x && Near(x[0], 1.0f + 0.2f * e), "orbit angle step * e at 144 fps (%.6f)", x ? x[0] : 0.f);
    FillState(o, 7); in[1] = 2.0f;
    x = RunNamed("EF_Em4000_BeamTiltLerp_53AC32", o);
    CHECK(x && Near(2.0f * x[1], 2.0f * lerpE(0.004f)), "Em4000 beam tilt lerp exact at 144 fps (%.6f)", x ? 2 * x[1] : 0.f);

    // Hover pull and decay: steady speed and the approach after 1 s match 60 frames at 60 fps.
    FillState(o, 8); in[7] = 0.1f; F(0xC84) = e;
    x = RunNamed("EF_EmHover_PullWeight_5423AC", o);
    float W = x ? x[2] : 0.f;
    FillState(o, 8); in[6] = 1.0f; in[0] = 0.1f * e;
    x = RunNamed("EF_EmHover_VelDecay_5423D8", o);
    float R = x ? x[2] : 0.f;
    {
        float v144 = 0, v60 = 0;
        for (int i = 0; i < 144; ++i) v144 = R * v144 + W;
        for (int i = 0; i < 60; ++i) v60 = 0.9f * v60 + 0.1f;
        CHECK(Near(W, lerpE(0.1f)) && Near(R, powf(0.9f, e)) && fabsf(v144 - v60) < 1e-4f,
              "hover pull after 1 s at 144 fps: %.5f (60 fps %.5f; weight %.5f, decay %.5f)", v144, v60, W, R);
    }
    // Part gravity: pos += -0.5*g*d*d*(1/e - 1) along M row 1.
    FillState(o, 9); F(0x1344) = e; F(0x1374) = 0.02f; F(0xD0) = 0; F(0xD4) = 1; F(0xD8) = 0;
    F(0x50) = F(0x54) = F(0x58) = 0;
    RunNamed("EF_Em2100Parts_GravityExact_682E36", o);
    CHECK(Near(F(0x54), -0.5f * 0.02f * e * e * (1 / e - 1)) && F(0x50) == 0 && F(0x58) == 0,
          "part gravity correction at 144 fps (%.7f)", F(0x54));
    // Fall state: vy -= g*e; pos step d*vy + 0.5*g*d*(1-e).
    FillState(o, 10); F(0x5FA4) = 0.02f; F(0xC84) = e; in[0] = 1.0f; in[1] = e;
    x = RunNamed("EF_Em0000_FallVyExact_566820", o);
    CHECK(x && Near(x[0], 1.0f - 0.02f * e) && Near(x[1], e + 0.01f * e * (1 - e)), "enemy fall exact at 144 fps (vy %.6f, step %.6f)",
          x ? x[0] : 0.f, x ? x[1] : 0.f);

    // Bullet arc: 1 s at 144 fps lands where 60 frames at 60 fps do (gravity before the move).
    {
        float vy = 1.0f, y = 0, yPlain = 0;
        for (int i = 0; i < 144; ++i) {
            FillState(o, 11);
            I(kRdxFlag) = 1;
            I(0xC84) = 0xA; F(0xDB0) = F(0xDB4) = F(0xDB8) = 0; F(0xE98) = 0.05f; I(0xDC0) = 0; F(0xE58) = 1.0f; F(0xC80) = e;
            F(0xD0) = 0; F(0xD4) = 1; F(0xD8) = 0;
            vy -= 0.05f * e;  // BLT_ArcGravity
            F(0) = 0; F(4) = vy; F(8) = 0; F(0xC) = 0;
            RunNamed("EF_Blt_ExactGravityPos_5EDC16", o);
            y += *(float*)(o + kRdxOut + 4) * e;
            yPlain += vy * e;
        }
        float vy60 = 1.0f, y60 = 0;
        for (int i = 0; i < 60; ++i) vy60 -= 0.05f, y60 += vy60;
        CHECK(fabsf(y - y60) < 1e-4f * fabsf(y60) + 1e-4f, "bullet arc height after 1 s at 144 fps: %.5f (60 fps %.5f, without the fix %.5f)",
              y, y60, yPlain);
        I(kRdxFlag) = 0;
    }
    // Ring bomb: (M*vel + M.row1*c)*e, c = -0.0025*(1-e).
    FillState(o, 12); F(0xDF0) = 1; F(0xDF4) = 2; F(0xDF8) = 3; F(0xD0) = 0; F(0xD4) = 1; F(0xD8) = 0; F(0xE04) = 0;
    RunNamed("EF_Blt_RingBombMoveExact_6E4E58", o);
    CHECK(Near(F(0xDF0), e) && Near(F(0xDF4), (2 - 0.0025f * (1 - e)) * e) && Near(F(0xDF8), 3 * e),
          "ring bomb step exact at 144 fps (%.6f)", F(0xDF4));
    // Bullet spin: rotation by w*(1-(1-kD)^e)/(kD), then w *= (1-kD)^e.
    if (const PatchRecord* r = FindPatch("EF_Blt_SpinRateExact_5ED66E")) {
        const uint8_t* orig = g_pristineImage + (r->site - G.image);
        auto rotY = (void (*)(float*, float))(r->site + 5 + *(const int32_t*)(orig + 1));
        FillState(o, 13); F(0xF00) = 0.05f; F(0xC80) = e; in[1] = 0.2f;
        RunNamed("EF_Blt_SpinRateExact_5ED66E", o);
        alignas(16) float want[16];
        rotY(want, 0.2f * lerpE(0.05f) / 0.05f);
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && Near(((float*)(o + kVecRcx))[i], want[i]);
        CHECK(same, "bullet spin rotation weight at 144 fps");
    }
    FillState(o, 14); F(0xF00) = 0.05f; F(0xC80) = e; in[9] = 1.0f;
    x = RunNamed("EF_Blt_SpinDecayExact_5ED68A", o);
    CHECK(x && Near(x[1], powf(0.95f, e)) && x[0] == 0.0f, "bullet spin decay exact at 144 fps (%.6f)", x ? x[1] : 0.f);
    // Em011e: decay before a per-update move; 1 s of travel equals 60 frames at 60 fps.
    FillState(o, 15); in[0] = 0.1f * e; in[8] = 1.0f; F(0) = 1; F(4) = 0; F(8) = 0; F(0x50) = F(0x54) = F(0x58) = 0;
    x = RunNamed("EF_Em011e_StopDecayExact_67EBE6", o);
    {
        float corr = F(0x50), re = x ? x[2] : 0.f, v = 1, pos = 0, v60 = 1, pos60 = 0;
        for (int i = 0; i < 144; ++i) pos += corr * v + re * v * e, v *= re;
        for (int i = 0; i < 60; ++i) v60 *= 0.9f, pos60 += v60;
        CHECK(x && Near(re, powf(0.9f, e)) && Near(x[8], re) && fabsf(pos - pos60) < 1e-4f,
              "Em011e stopping distance after 1 s at 144 fps: %.5f (60 fps %.5f)", pos, pos60);
    }
    // Frame counters count 60 Hz ticks.
    FillState(o, 16); I(0x2DCC) = 5;
    S->ticks60 = 0;
    RunNamed("EF_Baa000_StateFrames_468082", o);
    int held = I(0x2DCC);
    S->ticks60 = 1;
    RunNamed("EF_Baa000_StateFrames_468082", o);
    CHECK(held == 5 && I(0x2DCC) == 6, "Baa000 state frames count 60 Hz ticks (%d, %d)", held, I(0x2DCC));
    SimulateFrame(144);
}

static void PushBlockTests() {
    At60([] { CheckSameAt60("PB_BlockFallExact"); });
    {
        // Moved flag: the block pre-update keeps a push's flag; the block update clears it once
        // applied, with the flags its `je` reads unchanged (taken only when nothing moved).
        static uint8_t* m = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        const PatchRecord* keep = FindPatch("PB_BlockMovedKeep");
        const PatchRecord* use = FindPatch("PB_BlockMovedConsume");
        bool ok = keep && use;
        if (ok) {
            FillState(m, 4);
            *(int*)(m + kVecRcx + 0x978) = 1;  // rcx = the block in the pre-update
            RunSite(keep->site, keep->len, m, 0);
            ok = *(int*)(m + kVecRcx + 0x978) == 1;
            for (int moved = 0; moved < 2 && ok; ++moved) {
                *(int*)(m + 0x978) = moved;
                RunSite(use->site, use->len, m, 0);
                ok = *(int*)(m + 0x978) == 0;
            }
        }
        CHECK(ok, "push moved flag kept until the block update applies it, then cleared");
    }
    const float* step = ScaledConstant("PB_InteractCooldownStep");
    const float* pull = ScaledConstant("PB_BlockNavPullLerp");
    const float* thr = ScaledConstant("PB_PushMoveThreshold");
    CHECK(step && pull && thr, "pushable block patches present");

    // A block tipping off a ledge: 1 s at 144 fps drops as far as 60 frames at 60 fps.
    SimulateFrame(144);
    float e = S->stepUpToOne;
    static uint8_t* o = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* in = (float*)(o + kXmmIn);
    FillState(o, 1);
    *(float*)(o + 0xB24) = 0.0f;
    *(float*)(o + 0x54) = 10.0f;
    float plain = 10.0f, vPlain = 0.0f;
    for (int i = 0; i < 144; ++i) {
        in[0] = e / 300.0f;                    // g = GetDelta/300 at normal speed
        in[1] = *(float*)(o + 0xB24);          // vy
        RunNamed("PB_BlockFallExact", o);
        vPlain -= e / 300.0f, plain += vPlain;
    }
    // 60 Hz: v -= 1/300, y += v per tick, i.e. y(t) = y0 - t(t+1)/600 after t ticks; compared at
    // the time actually simulated (144 frames of the measured e).
    double t = 144.0 * e;
    float y60 = (float)(10.0 - t * (t + 1.0) / 600.0);
    float y = *(float*)(o + 0x54);
    CHECK(fabsf(y - y60) < 1e-4f, "block fall after 1 s at 144 fps: %.5f (60 fps %.5f, without the fix %.5f)", y, y60, plain);
    CHECK(step && pull && Near(*step, e / 60.0f) && Near(*pull, 1 - powf(0.95f, e)),
          "interaction cooldowns count real time, block area pull exact at 144 fps (%.6f, %.6f)", step ? *step : 0.f,
          pull ? *pull : 0.f);
    CHECK(thr && Near(*thr, 0.0001f * e), "push displacement threshold is the 60 fps speed at 144 fps (%.7f)", thr ? *thr : 0.f);
    SimulateFrame(144);
}

// Auto-ending caption: closes the letterbox it still shows; a closed caption is left alone.
static void CaptionTests(uint8_t* img) {
    const PatchRecord* r = FindPatch("CP_CaptionAutoEndClose");
    CHECK(r != nullptr, "caption auto-end patch present");
    if (!r) return;
    uint32_t& flags = *(uint32_t*)(img + 0x10297C0);
    uint8_t* mgr = img + 0x1416B90;
    uint8_t savedMgr[0x50];
    memcpy(savedMgr, mgr + 0x2D60, sizeof savedMgr);
    uint32_t savedFlags = flags;
    static uint8_t* o = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    FillState(o, 1);
    o[0x49] = 0;
    flags = 0x2200;
    RunSite(r->site, r->len, o, 0);
    bool idle = o[0x49] == 0 && flags == 0x2200;

    FillState(o, 2);
    o[0x49] = 1;
    flags = 0x2200;
    memset(mgr + 0x2D60, 0, sizeof savedMgr);
    *(uint64_t*)(mgr + 0x2D60) = 1;  // a letterbox is open (only tested for non-null on this path)
    *(uint32_t*)(mgr + 0x2D7C) = 1;  // opened by an auto-ending caption
    RunSite(r->site, r->len, o, 0);
    bool closed = o[0x49] == 0 && flags == 0x200 && *(uint32_t*)(mgr + 0x2D90) == 1 && *(uint32_t*)(mgr + 0x2D7C) == 0;

    memcpy(mgr + 0x2D60, savedMgr, sizeof savedMgr);
    flags = savedFlags;
    CHECK(idle && closed, "auto-ending caption closes its letterbox and control lock (closed caption untouched %d, close %d)",
          idle, closed);

    // The stuck order through the game's own letterbox open/close (0x140910E80 / 0x1408A0560):
    // event start, caption opens with a hold, caption closes during the event, event ends.
    const PatchRecord* rel = FindPatch("CP_LetterboxCaptionRelease");
    CHECK(rel != nullptr, "letterbox caption-hold release patch present");
    if (!rel) return;
    uint8_t* closeFn = rel->site - 0x21;
    auto open = (void (*)(uint8_t*, int))(img + 0x910E80);
    auto close = (void (*)(uint8_t*, int, int))closeFn;
    static uint8_t widget[0x100];
    memset(mgr + 0x2D60, 0, sizeof savedMgr);
    *(uint64_t*)(mgr + 0x2D60) = (uint64_t)widget;  // letterbox already shown: opens only flag it
    *(uint32_t*)(mgr + 0x2D74) = 1;   // an event holds the letterbox (as 0x140910A00 leaves it)
    *(uint32_t*)(mgr + 0x2D80) = 1;
    open(mgr, 1);                     // caption opens with a hold
    close(mgr, 0, 0);                 // caption closes while the event runs
    int heldAfterCaption = *(int*)(mgr + 0x2D7C);
    close(mgr, 1, 1);                 // event ends, keeping the letterbox only for open captions
    bool released = heldAfterCaption == 0 && *(uint32_t*)(mgr + 0x2D90) == 1 && *(uint32_t*)(mgr + 0x2D7C) == 0;
    // A caption still open when the event ends keeps it (unchanged).
    memset(mgr + 0x2D60, 0, sizeof savedMgr);
    *(uint64_t*)(mgr + 0x2D60) = (uint64_t)widget;
    *(uint32_t*)(mgr + 0x2D74) = 1;
    open(mgr, 1);
    close(mgr, 1, 1);
    bool kept = *(uint32_t*)(mgr + 0x2D90) == 0 && *(uint32_t*)(mgr + 0x2D7C) == 1;
    memcpy(mgr + 0x2D60, savedMgr, sizeof savedMgr);
    flags = savedFlags;
    CHECK(released && kept, "caption closed during an event releases its letterbox hold (released %d, open caption kept %d)",
          released, kept);
}

static void BulletAndEnemyTests() {
    static uint8_t* obj = (uint8_t*)VirtualAlloc(nullptr, kObjSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* out = (float*)(obj + kXmmOut);
    const PatchRecord* arc = FindPatch("BLT_ArcGravity");
    const PatchRecord* homing = FindPatch("BLT_HomingTurn");
    const PatchRecord* line = FindPatch("BLT_LineCounter");
    const PatchRecord* ctr = FindPatch("EM_TimeOverrideCtr");
    const float* lerp01 = ScaledConstant("enemy movement smoothing 0.1");
    CHECK(arc && homing && line && ctr && lerp01, "bullet and enemy patches present");
    if (!arc || !homing || !line || !ctr || !lerp01) return;

    SimulateFrame(144);
    float e = S->stepUpToOne;
    *(float*)(obj + 0xE04) = 1.0f;
    *(float*)(obj + 0xE98) = 0.05f;
    RunSite(arc->site, arc->len, obj, 0);
    CHECK(fabs(*(float*)(obj + 0xE04) - (1.0f - 0.05f * e)) < 1e-6f, "arc shot gravity at 144 fps: vel.y %.5f (e=%.4f)",
          *(float*)(obj + 0xE04), e);
    *(float*)(obj + 0xE4C) = 0.2f;
    *(float*)(obj + 0xE48) = 0.1f;
    RunSite(homing->site, homing->len, obj, 0);
    CHECK(fabs(out[0] - 0.2f * e) < 1e-6f && fabs(out[1] - (1 - powf(0.9f, e))) < 1e-6f,
          "homing turn at 144 fps: max %.5f, fraction %.5f", out[0], out[1]);
    *(float*)(obj + 0x1530) = 3.0f;
    RunSite(line->site, line->len, obj, 0);
    CHECK(fabs(out[0] - (3.0f + e)) < 1e-6f, "guided-line frame counter at 144 fps: %.5f", out[0]);
    CHECK(fabs(*lerp01 - (1 - powf(0.9f, e))) < 1e-6f, "enemy movement smoothing at 144 fps: %.5f", *lerp01);
    // Shared enemy turn helper: fraction = delta * rate, now exact. One second at 144 fps
    // must leave the same remaining angle as 60 frames at 60 fps (rate 0.2).
    if (const PatchRecord* turn = FindPatch("EX_TurnFE650_Frac")) {
        (void)turn;
        float remain60 = powf(1.0f - 0.2f, 60.0f), remain = 1.0f;
        for (int i = 0; i < 144; ++i) remain *= 1.0f - LerpE(fminf(0.2f * e * *InvE(), 1.0f));
        CHECK(fabs(remain - remain60) < 1e-4f, "exact turn fraction after 1 s at 144 fps: %.6f (60 fps %.6f, linear %.6f)",
              remain, remain60, powf(1.0f - 0.2f * e, 144.0f));
    } else {
        CHECK(false, "exact turn fraction patch present");
    }
    CHECK(LerpE(1.5f) == 1.5f && PowE(-0.5f) == -0.5f && LerpE(1.0f) == 1.0f,
          "out-of-range factors pass through instead of NaN (LerpE(1.5) %.3f, PowE(-0.5) %.3f)", LerpE(1.5f), PowE(-0.5f));

    S->ticks60 = 0;
    int left = RunSite(ctr->site, ctr->len, obj, 10);
    CHECK(left == 10 && *(int*)(obj + 0x5F34) == 10 && *(int*)(obj + 0x5F30) == 1,
          "enemy 10-frame counter holds on a frame without a 60 Hz tick (%d)", left);
    S->ticks60 = 1;
    left = RunSite(ctr->site, ctr->len, obj, 10);
    CHECK(left == 9 && *(int*)(obj + 0x5F34) == 9, "enemy 10-frame counter counts 60 Hz ticks (%d)", left);

    int savedTarget = g_cfg.targetFps;
    g_cfg.targetFps = 60;
    SimulateFrame(61);
    SimulateFrame(61);
    g_cfg.targetFps = savedTarget;
    *(float*)(obj + 0xE04) = 1.0f;
    RunSite(arc->site, arc->len, obj, 0);
    float vy = *(float*)(obj + 0xE04);
    RunSite(homing->site, homing->len, obj, 0);
    CHECK(vy == 0.95f && out[0] == 0.2f && out[1] == 0.1f && *lerp01 == 0.1f,
          "bullets and enemies unchanged at 60 fps (vel.y %.6f, max %.6f, fraction %.6f, smoothing %.6f)", vy, out[0],
          out[1], *lerp01);
    SimulateFrame(144);
}

void RunSubsystemTests(uint8_t* img, int* failures) {
    (void)img;
    g_fail = failures;

    // UI fade: one second at 144 fps must reach the same alpha as 60 frames of vanilla.
    const float* fadeK = ScaledConstant("UI fade (UIAllPurpose)");
    const float* gauge = ScaledConstant("9S hack gauge decay");
    const float* airDecay = ScaledConstant("air control decay");
    const float* landThr = ScaledConstant("landing displacement threshold");
    CHECK(fadeK && gauge && airDecay && landThr, "scaled constants registered");
    if (!fadeK || !gauge || !airDecay || !landThr) return;
    float vanilla = 0, modded = 0;
    for (int i = 0; i < 60; ++i) vanilla += (1.0f - vanilla) * 0.1f;
    for (int i = 0; i < 144; ++i) {
        SimulateFrame(144);
        modded += (1.0f - modded) * *fadeK;
    }
    CHECK(fabs(vanilla - modded) < 0.01f, "UI fade after 1 s: vanilla 60 fps %.4f, mod 144 fps %.4f", vanilla, modded);
    int savedTarget = g_cfg.targetFps;
    g_cfg.targetFps = 60;
    SimulateFrame(61);  // sets the 60 fps limiter target
    SimulateFrame(61);  // a slightly early frame
    CHECK(*fadeK == 0.1f && *airDecay == 0.93f && S->step == 1.0f && S->ticks60 == 1,
          "60 fps limit, early frame: step %.4f, fade %.3f, air decay %.3f, ticks %d (vanilla 1.0, 0.1, 0.93, 1)", S->step,
          *fadeK, *airDecay, S->ticks60);
    g_cfg.targetFps = savedTarget;
    SimulateFrame(144);

    // Per-frame decay and amounts summed over one second equal 60 vanilla frames.
    float drain = 0, speed = 1.0f;
    for (int i = 0; i < 144; ++i) {
        SimulateFrame(144);
        drain += *gauge;
        speed *= *airDecay;
    }
    CHECK(fabs(drain - 60.0f / 360.0f) < 0.005f, "hack gauge drain per second at 144 fps = %.4f (60 fps: %.4f)", drain, 60.0f / 360);
    CHECK(fabs(speed - powf(0.93f, 60)) < 0.002f, "air momentum after 1 s at 144 fps = %.4f (60 fps: %.4f)", speed, powf(0.93f, 60));
    CHECK(fabs(*landThr - 0.0025f * S->step * S->step) < 1e-7f, "landing threshold scales with step^2 (%.6f)", *landThr);

    // Flight unit thunks, run against the game's own vector math.
    SimulateFrame(144);
    float e = S->stepUpToOne;
    if (const PatchRecord* r = FindPatch("R1_AirBezier_disp")) {
        // vec * scalar: the scalar (xmm2) is scaled by e before the original runs.
        alignas(16) float out[4] = {}, v[4] = {1, 2, 3, 0};
        ((void (*)(float*, float*, float))r->cave)(out, v, 2.0f);
        CHECK(fabs(out[0] - 2 * e) < 1e-5f && fabs(out[2] - 6 * e) < 1e-5f, "rail movement scaled: (%.4f, %.4f, %.4f), e=%.4f",
              out[0], out[1], out[2], e);
    } else {
        CHECK(false, "rail movement patch present");
    }
    if (const PatchRecord* r = FindPatch("P1_PlStg_move")) {
        // vec * rotation: the result vector is scaled by e after the original runs.
        alignas(16) float out[4] = {}, v[4] = {1, 2, 3, 0};
        alignas(16) float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        ((void (*)(float*, float*, float*))r->cave)(out, v, m);
        CHECK(fabs(out[0] - e) < 1e-5f && fabs(out[1] - 2 * e) < 1e-5f && fabs(out[2] - 3 * e) < 1e-5f,
              "ship movement scaled: (%.4f, %.4f, %.4f)", out[0], out[1], out[2]);
    } else {
        CHECK(false, "ship movement patch present");
    }

    // Jump arc: one second of a jump (v0 0.3, g 0.014) at 144 fps must land where 60 fps does.
    {
        const float g = 0.014f, v0 = 0.3f;
        float y60 = 0, v = v0;
        for (int i = 0; i < 60; ++i) {
            v -= g;
            y60 += v;
        }
        alignas(16) uint8_t ent[0x100] = {};
        float* pos = (float*)(ent + 0x50);
        ((float*)(ent + 0xD0))[1] = 1.0f;  // up axis
        float savedE = S->stepUpToOne;
        float e = S->stepUpToOne = 60.0f / 144.0f;
        v = v0;
        for (int i = 0; i < 144; ++i) {
            float vb = i == 0 ? v0 : v;  // the jump state sets v0 on the first frame
            v = ExactGravity(ent, vb, vb - g * e, g, e, false);
            pos[1] += v * e;
        }
        float naive = 0;
        v = v0;
        for (int i = 0; i < 144; ++i) {
            v -= g * e;
            naive += v * e;
        }
        CHECK(fabs(pos[1] - y60) < 0.002f, "jump height after 1 s: 60 fps %.4f, mod 144 fps %.4f (unfixed %.4f)", y60, pos[1], naive);
        // Glide: the state machine re-sets vel.y every frame.
        float y60g = 0;
        for (int i = 0; i < 60; ++i) y60g += -0.05f - g;
        alignas(16) uint8_t ent2[0x100] = {};
        ((float*)(ent2 + 0xD0))[1] = 1.0f;
        float& y144g = ((float*)(ent2 + 0x50))[1];
        for (int i = 0; i < 144; ++i) {
            float vb = -0.05f;
            y144g += ExactGravity(ent2, vb, vb - g * e, g, e, false) * e;
        }
        CHECK(fabs(y144g - y60g) < 0.002f, "glide sink after 1 s: 60 fps %.4f, mod 144 fps %.4f", y60g, y144g);
        S->stepUpToOne = savedE;
    }

    // Cloth: a frame-dependent stand-in step (velocity decays 0.9 per call), driven
    // through the patched Cloth::Step entry, must advance exactly in 60 Hz steps and
    // keep values written by other systems between frames.
    if (const PatchRecord* r = FindPatch("cloth step")) {
        static uint8_t cloth[0xC10], nodes[0x130], bone[0x40];
        memset(cloth, 0, sizeof cloth);
        memset(nodes, 0, sizeof nodes);
        memset(bone, 0, sizeof bone);
        *(uint32_t*)(cloth + 0x20) = 1;
        *(uint8_t**)(cloth + 0x28) = nodes;
        *(uint8_t**)(cloth + 0xB10) = bone;  // any non-null model pointer
        *(uint8_t**)(nodes + 0x110) = bone;
        float* vel = (float*)(nodes + 0x40);
        float* pos = (float*)(nodes + 0x50);
        float* input = (float*)(nodes + 0x70);
        vel[0] = 1.0f;
        SetClothStepForTest([](uint8_t* c, float dt, int) {
            uint8_t* n = *(uint8_t**)(c + 0x28);
            float* v = (float*)(n + 0x40);
            ((float*)(n + 0x50))[0] += v[0] * dt;
            v[0] *= 0.9f;
            ((float*)*(uint8_t**)(n + 0x110))[0] += 1.0f;  // the step also rewrites its bone
        });
        auto step = (void (*)(uint8_t*, float, int))r->site;
        int commits = 0;
        bool inputKept = true;
        for (int i = 0; i < 144; ++i) {
            SimulateFrame(144);
            if (i == 0 || S->ticks60 > 0) ++commits;
            *input = (float)i;  // the skeleton update rewrites per-frame inputs every frame
            step(cloth, S->step, 0);
            inputKept &= *input == (float)i;
        }
        g_cfg.targetFps = 60;
        SimulateFrame(60);
        SimulateFrame(60);  // now at vanilla pacing: rolls back the preview, then one plain step
        step(cloth, 1.0f, 0);
        g_cfg.targetFps = savedTarget;
        ++commits;
        float expect = 0, v = 1.0f;
        for (int k = 0; k < commits; ++k) {
            expect += v;
            v *= 0.9f;
        }
        CHECK(fabs(pos[0] - expect) < 1e-4f && fabs(vel[0] - v) < 1e-6f && ((float*)bone)[0] == (float)commits,
              "cloth after %d 60 Hz steps: pos %.5f (expected %.5f), vel %.5f (expected %.5f), bone %g", commits, pos[0], expect,
              vel[0], v, ((float*)bone)[0]);
        CHECK(inputKept, "cloth rollback keeps inputs written between frames");
        SimulateFrame(144);
    } else {
        CHECK(false, "cloth patch present");
    }

    // Input press buffers: 20 frames must last 1/3 s at any frame rate.
    if (const PatchRecord* r = FindPatch("input press buffers")) {
        alignas(16) uint8_t input[0x800] = {};
        int* buf = (int*)(input + 0x770);
        buf[2] = 20;
        int frames = 0;
        while (buf[2] > 0 && frames < 1000) {
            SimulateFrame(144);
            ((void (*)(void*))r->site)(input);
            ++frames;
        }
        CHECK(frames >= 46 && frames <= 50, "jump buffer lasts %d frames at 144 fps (vanilla 20 at 60 fps = 48 at 144)", frames);
    } else {
        CHECK(false, "input press buffer patch present");
    }

    // Tick-gated functions return immediately when no 60 Hz tick elapsed (the
    // invalid `this` proves the original body never runs).
    if (const PatchRecord* r = FindPatch("option text marquee (left)")) {
        S->ticks60 = 0;
        ((void (*)(void*))r->site)((void*)0x10);
        CHECK(true, "marquee update skipped on a frame without a 60 Hz tick");
    } else {
        CHECK(false, "marquee patch present");
    }
    if (const PatchRecord* r = FindPatch("rumble envelope")) {
        int out = 7;
        S->ticks60 = 0;
        ((void (*)(void*, int*, int))r->cave)((void*)0x10, &out, 1);
        CHECK(out == 7, "rumble envelope skipped on a frame without a 60 Hz tick");
    } else {
        CHECK(false, "rumble patch present");
    }
    S->ticks60 = 1;
    BulletAndEnemyTests();
    SameAt60Tests();
    MiscFinalTests();
    CameraFinalTests(img);
    EnemyFinalTests();
    PushBlockTests();
    CaptionTests(img);
}
