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

// Runs the patched code from `site` up to `site + len` with rbx = rsi = rdi = obj and
// eax = eaxIn in a function-like frame (rsp 16-byte aligned, 0x100 bytes of locals),
// then returns eax. xmm0 and xmm1 are stored to obj+0x1F00 / obj+0x1F04.
static int RunSite(uint8_t* site, size_t len, uint8_t* obj, int eaxIn) {
    static uint8_t* thunk;
    static uint8_t* back;
    if (!thunk) {
        thunk = g_arena.Alloc(128);
        Asm a(thunk);
        a.raw({0x53, 0x56, 0x57, 0x55});                          // push rbx, rsi, rdi, rbp
        a.raw({0x48, 0x81, 0xEC}).i32(0x108);                     // sub rsp, 0x108
        a.raw({0x48, 0x89, 0xD3, 0x48, 0x89, 0xD6, 0x48, 0x89, 0xD7});  // mov rbx/rsi/rdi, rdx
        a.raw({0x44, 0x89, 0xC0});                                // mov eax, r8d
        a.raw({0xFF, 0xE1});                                      // jmp rcx
        back = a.cur();
        a.raw({0xF3, 0x0F, 0x11, 0x83}).i32(0x1F00);              // movss [rbx+0x1F00], xmm0
        a.raw({0xF3, 0x0F, 0x11, 0x8B}).i32(0x1F04);              // movss [rbx+0x1F04], xmm1
        a.raw({0x48, 0x81, 0xC4}).i32(0x108);                     // add rsp, 0x108
        a.raw({0x5D, 0x5F, 0x5E, 0x5B, 0xC3});                    // pop rbp, rdi, rsi, rbx; ret
    }
    uint8_t saved[5], jmp[5] = {0xE9};
    memcpy(saved, site + len, 5);
    *(int32_t*)(jmp + 1) = (int32_t)(back - (site + len + 5));
    WriteProtected(site + len, jmp, 5);
    int r = ((int (*)(uint8_t*, uint8_t*, int))thunk)(site, obj, eaxIn);
    WriteProtected(site + len, saved, 5);
    return r;
}

static void BulletAndEnemyTests() {
    alignas(16) static uint8_t obj[0x6000];  // fields up to +0x5F34
    float* out = (float*)(obj + 0x1F00);
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
}
