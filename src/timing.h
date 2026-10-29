#pragma once
#include "common.h"

// Per-frame values that patched game code reads directly (RIP-relative), so the
// block lives in the near arena.
struct SharedState {
    int32_t ticks60;          // 60 Hz ticks elapsed this frame: 1 at <= 60 fps, else 0 or 1
    int32_t limiterOverride;  // 1 = use targetUnits instead of the game's 60/30 fps target
    double targetUnits;       // limiter frame time in 1/3 ms (0 = unlimited)
    float step;               // global time step in 60 Hz frames (< 1 above 60 fps)
    float ticks60f;           // ticks60 as float, for caves that need it in an xmm register
    float stepOrOne;          // max(step, 1): what per-frame code assumed it was advancing
    float stepUpToOne;        // e = min(step, 1): scale for amounts vanilla applied once per frame
    float frameSec;           // seconds per update for code that assumed 1/60: e / 60
};
extern SharedState* S;
extern uint64_t g_frame;

bool InstallCorePatches();
// Replaces the game's UpdateTimeStep call each frame (exposed for the test harness).
void OnUpdateTimeStep(void* obj, int variableStep, float fixedStep);
// Accurate integer milliseconds for this frame's dt (shared by all ms timers).
int QuantizedMs(float dtSeconds);
