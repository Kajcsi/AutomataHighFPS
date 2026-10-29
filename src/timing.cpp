#include "timing.h"
#include "game.h"
#include "memory.h"
#include "display.h"
#include "patchkit.h"
#include <cmath>

SharedState* S;
uint64_t g_frame;

static double g_tickAcc;
static bool g_manualCap;
static bool g_keyWasDown;
static double g_lastFps = -2;

// ---------------------------------------------------------------------------
// Situation detection (addresses from the signature scan; reads are guarded
// because the hacking object pointer can be stale between scenes).

static uint32_t SafeRead32(const void* p) {
    if (!p) return 0;
    __try {
        return *(const volatile uint32_t*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool InMenu() {
    return (G.uiMgr && (SafeRead32(G.uiMgr + 0x1E4) & 1)) || (SafeRead32(G.shortcutMenu) & 1);
}

static bool Loading() { return SafeRead32(G.loadFlag) != 0; }

// ---------------------------------------------------------------------------
// Frame limiter target

static double CapTo(double fps, int cap) { return (fps <= 0 || fps > cap) ? cap : fps; }

static void UpdateLimiter() {
    double fps = g_cfg.targetFps == -1 ? CurrentRefreshHz() : g_cfg.targetFps;
    if (g_manualCap) {
        fps = 60;
    } else if (g_cfg.unlimitedLoading && Loading()) {
        fps = 0;
    } else {
        if (g_cfg.menuFps > 0 && InMenu()) fps = CapTo(fps, g_cfg.menuFps);
    }
    S->targetUnits = fps > 0 ? 3000.0 / fps : 0.0;
    if (fps != g_lastFps) {
        Log("frame limit -> %s%.0f", fps > 0 ? "" : "unlimited ", fps > 0 ? fps : 0.0);
        g_lastFps = fps;
    }
}

static void PollHotkey() {
    if (!g_cfg.toggleKey) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    bool down = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    if (down && !g_keyWasDown) {
        g_manualCap = !g_manualCap;
        Log("manual 60 fps cap %s", g_manualCap ? "ON" : "OFF");
        MessageBeep(g_manualCap ? MB_ICONASTERISK : MB_OK);
    }
    g_keyWasDown = down;
}

// ---------------------------------------------------------------------------
// Per-frame hook: wraps the game's UpdateTimeStep call in the main update.

void OnUpdateTimeStep(void* obj, int variableStep, float fixedStep) {
    G.updateTimeStep(obj, variableStep, fixedStep);
    ++g_frame;
    float& stepRef = *(float*)((uint8_t*)obj + 0xB0);
    // Keep the vanilla lower clamp whenever the game is paced at 60 fps or less
    // (60 fps limit, F11 cap), so frames that finish slightly early behave
    // exactly as in the unmodded game.
    bool vanillaPacing = S->targetUnits >= 50.0 - 1e-6;
    if (stepRef < 1.0f && vanillaPacing) stepRef = 1.0f;
    float step = stepRef;
    S->step = step;
    S->stepOrOne = step < 1.0f ? 1.0f : step;
    S->stepUpToOne = step < 1.0f ? step : 1.0f;
    S->frameSec = step < 1.0f ? step * (1.0f / 60.0f) : 1.0f / 60.0f;
    if (!g_cfg.fixFrameCounters || !(step < 1.0f)) {
        // <= 60 fps: one tick per frame, exactly like vanilla.
        S->ticks60 = 1;
        g_tickAcc = 0.0;
    } else {
        g_tickAcc += step;
        int t = (int)g_tickAcc;
        g_tickAcc -= t;
        S->ticks60 = t;
    }
    S->ticks60f = (float)S->ticks60;
    S->tickPhase = (float)g_tickAcc;
    UpdateScaledConstants(S->stepUpToOne);
    UpdateLimiter();
    PollHotkey();
}

// ---------------------------------------------------------------------------
// Frame-tick getter: vanilla returns (int)(delta + 0.99), i.e. 1 every frame
// above 60 fps. Counters fed by it (warp/transition cooldowns) then ran fps/60
// times too fast.

static int OurFrameTicks() {
    float d = G.getDelta(G.deltaHandle);
    float step = G.Step();
    if (!(step < 1.0f)) return (int)(d + 0.99f);
    if (S->ticks60 <= 0 || step <= 1e-6f) return 0;
    return (int)(d / step + 0.99f);
}

// ---------------------------------------------------------------------------
// Millisecond timers: vanilla truncates dt*1000 every frame. At 60 fps that
// gives 16 ms per 16.67 ms frame (the game's tuned behaviour); above 60 fps the
// loss grows (14% at 144 fps) and past ~1000 fps the timer never advances,
// which blocks scripted progression. Above 60 fps we hand out whole
// milliseconds from an accumulator at the same 16-per-60 Hz-frame rate.

static uint64_t g_msFrame = ~0ull;
static float g_msDt;
static int g_msVal;
static double g_msAcc;

int QuantizedMs(float dtSeconds) {
    if (!g_cfg.fixMsTimers || !(G.Step() < 1.0f) || !(dtSeconds > 0.0f))
        return (int)(dtSeconds * 1000.0f);
    if (g_msFrame == g_frame)
        return dtSeconds == g_msDt ? g_msVal : (int)(dtSeconds * 1000.0f);
    g_msFrame = g_frame;
    g_msDt = dtSeconds;
    g_msAcc += dtSeconds * 960.0;
    int v = (int)g_msAcc;
    g_msAcc -= v;
    g_msVal = v;
    return v;
}

static void OurTimeCountAdd(int* counterMs, float dtSeconds) {
    if (dtSeconds >= 0.0f) *counterMs += QuantizedMs(dtSeconds);
}

static int OurMsFromSeconds(float dtSeconds) { return QuantizedMs(dtSeconds); }

// ---------------------------------------------------------------------------

bool InstallCorePatches() {
    S = (SharedState*)g_arena.Alloc(sizeof(SharedState), 64);
    if (!S) return false;
    S->ticks60 = 1;
    S->ticks60f = 1.0f;
    S->step = S->stepOrOne = S->stepUpToOne = 1.0f;
    S->frameSec = 1.0f / 60.0f;
    S->limiterOverride = 1;
    S->targetUnits = 50.0;

    Log("core patches:");
    bool ok = RedirectCall("per-frame hook", G.timeStepCall, (void*)&OnUpdateTimeStep);

    // Frame limiter: load our target unless the game asked for its 30 fps mode (100 units).
    uint8_t* back = G.limiterLoad + 15;
    ok &= InstallCave("frame limiter", G.limiterLoad, 15,
        {0x8B, 0x83, 0x08, 0x03, 0x00, 0x00, 0x45, 0x0F, 0x57, 0xC0, 0xF2, 0x4C, 0x0F, 0x2A, 0xC0},
        [&](Asm& a) {
            a.raw({0x8B, 0x83, 0x08, 0x03, 0x00, 0x00});         // mov eax, [rbx+0x308]
            a.raw({0x45, 0x0F, 0x57, 0xC0});                     // xorps xmm8, xmm8
            a.raw({0xF2, 0x4C, 0x0F, 0x2A, 0xC0});               // cvtsi2sd xmm8, rax
            a.raw({0x83, 0x3D}).rip(&S->limiterOverride, 1).raw({0x00});  // cmp [override], 0
            a.raw({0x74, 0x10});                                 // je back
            a.raw({0x3D, 0x64, 0x00, 0x00, 0x00});               // cmp eax, 100
            a.raw({0x74, 0x09});                                 // je back
            a.raw({0xF2, 0x44, 0x0F, 0x10, 0x05}).rip(&S->targetUnits);  // movsd xmm8, [targetUnits]
            a.jmp(back);
        });

    // Time step: drop the 1.0 lower clamp so the game advances in real time above 60 fps.
    ok &= PatchBytes("time-step lower clamp", G.stepClampJae, {0x73}, {0xEB});

    if (g_cfg.fixFrameCounters && G.nodeCounters) {
        uint8_t* n = G.nodeCounters;
        // Delay counter: `dec eax` -> `sub eax, ticks60`.
        InstallCave("hit-stop delay counter", n + 7, 12,
            {0xFF, 0xC8, 0xC7, 0x42, 0x14, 0x00, 0x00, 0x80, 0x3F, 0x89, 0x42, 0x28},
            [&](Asm& a) {
                a.raw({0x2B, 0x05}).rip(&S->ticks60);                    // sub eax, [ticks60]
                a.raw({0xC7, 0x42, 0x14, 0x00, 0x00, 0x80, 0x3F});        // mov dword [rdx+0x14], 1.0
                a.raw({0x89, 0x42, 0x28});                               // mov [rdx+0x28], eax
                a.jmp(n + 56);
            });
        // Duration counter: `dec dword [rdx+0x24]` -> subtract ticks60.
        InstallCave("hit-stop duration counter", n + 21, 7,
            {0xFF, 0x4A, 0x24, 0x44, 0x39, 0x52, 0x24},
            [&](Asm& a) {
                a.raw({0x50});                                           // push rax
                a.raw({0x8B, 0x05}).rip(&S->ticks60);                    // mov eax, [ticks60]
                a.raw({0x29, 0x42, 0x24});                               // sub [rdx+0x24], eax
                a.raw({0x58});                                           // pop rax
                a.raw({0x44, 0x39, 0x52, 0x24});                         // cmp [rdx+0x24], r10d
                a.jmp(n + 28);
            });
    }
    if (g_cfg.fixFrameCounters && G.frameTicksFn && G.getDelta)
        RedirectFunction("frame-tick getter", G.frameTicksFn, {0x48, 0x83, 0xEC, 0x28, 0x48}, (void*)&OurFrameTicks);

    if (g_cfg.fixMsTimers) {
        if (G.timeCountAddFn)
            RedirectFunction("TimeCountAction ms timer", G.timeCountAddFn, {0x0F, 0x57, 0xC0, 0x0F, 0x2F},
                             (void*)&OurTimeCountAdd);
        uint8_t* m = G.msCountdown;
        if (m && BytesEqual(m, {0xF3, 0x0F, 0x59, 0x05}) && BytesEqual(m + 8, {0xF3, 0x0F, 0x2C, 0xC0})) {
            // mulss xmm0,[1000.0] ; cvttss2si eax,xmm0  ->  call OurMsFromSeconds (xmm0 in, eax out).
            // Only volatile registers die here; rsp is call-aligned (right after a call returned).
            uint8_t* thunk = g_arena.Thunk((void*)&OurMsFromSeconds);
            uint8_t patch[12] = {0xE8};
            int32_t disp = (int32_t)(thunk - (m + 5));
            memcpy(patch + 1, &disp, 4);
            memset(patch + 5, 0x90, 7);
            WriteProtected(m, patch, 12);
            FlushInstructionCache(GetCurrentProcess(), m, 12);
            RecordPatch("action ms countdown", m, 12, thunk, 14);
            Log("  [ok] action ms countdown at %p", m);
        } else {
            Log("  [skip] action ms countdown: unexpected bytes");
        }
    }
    return ok;
}
