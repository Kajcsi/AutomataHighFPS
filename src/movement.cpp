// Player, enemy and pod movement beyond the basic fixes in subsystems.cpp:
// exact jump/fall/glide arcs, remaining per-frame counters, the vertical
// free-move mode, pod follow smoothing and A2's evade turn.
// Sites come from analysis/movement_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"
#include <cmath>
#include <cstring>

#include "generated/movement.inc"

static float& F(uint8_t* p, int off) { return *(float*)(p + off); }
static uint32_t Bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

// ---------------------------------------------------------------------------
// Exact gravity. Both integrators do `v -= g*d; pos += v*d` (semi-implicit
// Euler), so the arc depends on the step size: at 60 fps the sampled height is
// v0*t - g*t^2/2 - g*t/2 (t in 60 Hz frames). Two cases reproduce it exactly:
//  - free fall, including the first frame after a one-time set such as a jump
//    start: keep the vanilla v, and move the position by the missing
//    -0.5*g*(d60-d)*d along the up axis;
//  - vel.y re-set to the same value every frame (jump hold, glide, clamps):
//    vanilla loses a whole 60 Hz frame of gravity per set, so apply g*d60.
// The first frame of a repeated set is taken as a one-time set (error < one step).

struct GravSlot {
    const void* entity;
    uint32_t prevBits;  // vel.y we stored last frame
    uint32_t setBits;   // value the state machine set last frame
    bool valid;
    bool lastWasSet;
};
static GravSlot g_gravSlots[32];
static unsigned g_gravNext;
static SRWLOCK g_gravLock = SRWLOCK_INIT;

static GravSlot& SlotFor(const void* p) {
    for (GravSlot& s : g_gravSlots)
        if (s.entity == p) return s;
    GravSlot& s = g_gravSlots[g_gravNext++ % _countof(g_gravSlots)];
    s = {p, 0, 0, false, false};
    return s;
}

// vb: vel.y before this frame's gravity, vanillaV: vb - g*d. Returns the vel.y to store.
float ExactGravity(uint8_t* p, float vb, float vanillaV, float g, float d, bool grounded) {
    float e = S->stepUpToOne;
    float v = vanillaV;
    AcquireSRWLockExclusive(&g_gravLock);
    GravSlot& s = SlotFor(p);
    bool wasSet = false;
    if (!(grounded && vb <= 0.0f) && e < 1.0f && e > 0.0f) {
        float d60 = d / e;
        wasSet = !s.valid || Bits(vb) != s.prevBits;
        if (wasSet && s.lastWasSet && Bits(vb) == s.setBits) {
            v = vb - g * d60;  // repeated set: a whole 60 Hz frame of gravity per set
        } else {
            float c = -0.5f * g * (d60 - d) * d;  // free fall or one-time set
            float* pos = (float*)(p + 0x50);
            const float* up = (const float*)(p + 0xD0);
            pos[0] += c * up[0];
            pos[1] += c * up[1];
            pos[2] += c * up[2];
        }
    }
    s.setBits = Bits(vb);
    s.lastWasSet = wasSet;
    s.prevBits = Bits(v);
    s.valid = true;
    ReleaseSRWLockExclusive(&g_gravLock);
    return v;
}

// Player (Pl0000): vel +0x1490 (y +0x1494), vel.y before gravity +0x14A0,
// gravity +0x10484, delta +0x1470, grounded +0x14A4.
static void PlayerGravity(uint8_t* p) {
    F(p, 0x1494) = ExactGravity(p, F(p, 0x14A0), F(p, 0x1494), F(p, 0x10484), F(p, 0x1470), *(int*)(p + 0x14A4) != 0);
}

// Enemies: vel.y +0xCA4, gravity +0x5FA4, delta +0xC84, grounded +0xCB0.
static void EnemyGravity(uint8_t* p, float vb) {
    float g = F(p, 0x5FA4), d = F(p, 0xC84);
    F(p, 0xCA4) = ExactGravity(p, vb, vb - g * d, g, d, *(int*)(p + 0xCB0) != 0);
}

static void EmitCallC(Asm& a, const void* fn) {
    a.raw({0x48, 0xB8});  // mov rax, fn
    uint64_t f = (uint64_t)fn;
    for (int i = 0; i < 8; ++i) a.raw({(uint8_t)(f >> (8 * i))});
    a.raw({0xFF, 0xD0});  // call rax
}

static void FixGravity() {
    // Player: before `lea rdx, [rdi+0x1490]; jmp integrate` (volatile registers and
    // flags are dead there, rsp is call-aligned).
    PatchSite(kMove_PlGravCave, [&](Asm& a, uint8_t* site) {
        uint8_t* back = site + 9 + (int8_t)site[8];  // target of the replaced `jmp short`
        a.raw({0xF3, 0x0F, 0x10, 0x05}).rip(&S->stepUpToOne);  // movss xmm0, [e]
        a.raw({0x0F, 0x2F, 0x05}).rip(NearOne());               // comiss xmm0, [1.0]
        a.raw({0x73, 0x00});                                    // jae done
        uint8_t* jae = a.cur() - 1;
        a.raw({0x48, 0x8B, 0xCF});                              // mov rcx, rdi
        EmitCallC(a, (const void*)&PlayerGravity);
        *jae = (uint8_t)(a.cur() - (jae + 1));
        a.raw({0x48, 0x8D, 0x97, 0x90, 0x14, 0x00, 0x00});      // done: lea rdx, [rdi+0x1490]
        a.jmp(back);
    });
    // Enemies: replaces `vel.y -= g*d` (36 bytes); xmm1 must hold vel.y afterwards.
    PatchSite(kMove_EmGravCave, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x8B, 0xA4, 0x0C, 0x00, 0x00});  // movss xmm1, [rbx+0xCA4]
        a.raw({0xF3, 0x0F, 0x10, 0x05}).rip(&S->stepUpToOne);     // movss xmm0, [e]
        a.raw({0x0F, 0x2F, 0x05}).rip(NearOne());                  // comiss xmm0, [1.0]
        a.raw({0x73, 0x00});                                       // jae vanilla
        uint8_t* jae = a.cur() - 1;
        a.raw({0x48, 0x8B, 0xCB});                                 // mov rcx, rbx (xmm1 = vel.y)
        EmitCallC(a, (const void*)&EnemyGravity);
        a.raw({0xF3, 0x0F, 0x10, 0x8B, 0xA4, 0x0C, 0x00, 0x00});  // movss xmm1, [rbx+0xCA4]
        a.jmp(site + 36);
        *jae = (uint8_t)(a.cur() - (jae + 1));
        for (int i = 8; i < 36; ++i) a.raw({site[i]});             // vanilla: vel.y -= g*d; store
        a.jmp(site + 36);
    });
}

// ---------------------------------------------------------------------------
// Per-frame int counters

// `dec eax; mov [rdi+disp32], eax` -> `sub eax, ticks60; mov [rdi+disp32], eax`.
static void DecRegByTicks(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0x2B, 0x05}).rip(&S->ticks60);                                // sub eax, [ticks60]
        a.raw({0x89, 0x87, site[4], site[5], site[6], site[7]});             // mov [rdi+disp32], eax
        a.jmp(site + 8);
    });
}

void FixMovementExtra() {
    Log("movement (extra):");
    InstallTable(kMoveScaledConsts, _countof(kMoveScaledConsts));
    InstallTable(kMoveArgCalls, _countof(kMoveArgCalls));
    FixGravity();

    // Player hit-stop delay/length, timed flag, heal effect cooldown, input windows.
    for (const SigSite* s : {&kMove_IC_HitStopDelay_1063C, &kMove_IC_HitStopLen_10638, &kMove_IC_TimedFlag_10350,
                             &kMove_IC_HealFxCD_10888, &kMove_IC_Window_175A0, &kMove_IC_Hold_126D4})
        DecRegByTicks(*s);
    // Buddy HP drain while separated, 2-frame delay, pod sonar ping interval.
    // (The +0x128FC waits are the hacking respawn counters, patched in hacking.cpp.)
    for (const SigSite* s : {&kMove_IC_BuddyHpDrain_10668, &kMove_IC_Delay_164F0, &kMove_Pod_SonarPing})
        DecMemByTicks(*s);

    // A2 evade turn: `mulss xmm2, xmm7` (xmm7 = 0.1, also the lerp target) -> scaled 0.1.
    float* k = NewScaled("A2 evade turn", Scale::Lerp, 0.1f);
    PatchSite(kMove_A2_EvadeTurnLerp, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x59, 0x15}).rip(k);  // mulss xmm2, [k]
        a.raw({0xF3, 0x0F, 0x58, 0xD1});         // addss xmm2, xmm1
        a.jmp(site + 8);
    });
}
