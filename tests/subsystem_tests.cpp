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
}
