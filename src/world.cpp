// Per-frame constants found by sweeping the whole executable for frame-step idioms
// (lerps, decays, ramps and countdowns without the frame delta), outside the
// systems that have their own file: ambient audio smoothing, effect particles,
// foot IK, map gimmick timers and fades, item lifetimes, creature NPC states,
// player animation blends and scripted states, companion NPC motion, fish, the
// 9S thrown weapon, environment and light transitions, phase-flow and manager
// timers, scroll movement, and HUD/menu gauges, popups and counters.
// Sites come from analysis/sweep1_patches.tsv and analysis/sweep2_patches.tsv
// via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/sweep1.inc"
#include "generated/sweep2.inc"
#include "generated/sweep3.inc"
#include "generated/pod.inc"

static const float* E() { return &S->stepUpToOne; }

// Boss HP gauge: v = (target + v) * 0.5 per frame -> target*(1-0.5^e) + v*0.5^e
// (no target: v *= 0.5^e). Both paths continue at the store after the original *0.5.
static void FixBossGauge() {
    static const uint8_t kStore[] = {0xF3, 0x0F, 0x11, 0x87, 0x98, 0x00, 0x00, 0x00};  // movss [rdi+0x98], xmm0
    uint8_t* gauge = SiteOf(kSw2_SW2_BossGaugeSmoothA);
    if (!gauge || memcmp(gauge + 0x1A, kStore, sizeof kStore) != 0) {
        Log("  [skip] boss gauge: store not found");
        return;
    }
    const float* half = NewScaled("boss gauge smoothing", Scale::Lerp, 0.5f);
    const float* keep = NewScaled("boss gauge decay", Scale::Pow, 0.5f);
    PatchSite(kSw2_SW2_BossGaugeSmoothA, [&](Asm& a, uint8_t* site) {
        EmitSseRip(a, kMulss, 0, half);                         // xmm0 = target * LerpE(0.5)
        a.raw({0xF3, 0x0F, 0x10, 0x8F}).copy(site + 4, 4);      // movss xmm1, [rdi+0x98]
        EmitSseRip(a, kMulss, 1, keep);
        a.raw({0xF3, 0x0F, 0x58, 0xC1});                        // addss xmm0, xmm1
        a.jmp(site + 0x1A);                                     // movss [rdi+0x98], xmm0
    });
    PatchSite(kSw2_SW2_BossGaugeSmoothB, [&](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                        // movss xmm0, [rdi+0x98]
        EmitSseRip(a, kMulss, 0, keep);
        a.jmp(site + 0x10);                                     // movss [rdi+0x98], xmm0
    });
}

// Remaining counters and steps (analysis/sweep3_patches.tsv): player press buffers and
// scripted-state waits, hit/phase windows of the combat manager, the camera's
// skip-update counter, enemy and NPC pulls, Baa020 creature motion, ambient audio rays.
static void FixRemaining() {
    InstallTable(kSw3ScaledConsts, _countof(kSw3ScaledConsts));
    InstallTable(kSw3ArgCalls, _countof(kSw3ArgCalls));
    InstallTable(kSw3VecCalls, _countof(kSw3VecCalls));
    for (const SigSite* s : {&kSw3_LO_PlPressBuf118, &kSw3_LO_PlPressBuf11C, &kSw3_LO_PlPressBuf120, &kSw3_LO_PlPressBuf124,
                             &kSw3_LO_PlPressBuf128, &kSw3_LO_PlPressBuf12C, &kSw3_LO_PlPressBuf130, &kSw3_LO_PlPressBuf134,
                             &kSw3_LO_PlPressBuf138, &kSw3_LO_CamSkipUpdate8C08, &kSw3_LO_HitWindowMgr134,
                             &kSw3_LO_PhaseFlagTailMgr14CC})
        DecEaxByTicks(*s);
    // Inlined copy of the camera skip counter: `dec eax; mov [rip+disp32], eax`.
    PatchSite(kSw3_LO_CamSkipUpdate8C08_B, [](Asm& a, uint8_t* site) {
        const void* counter = RipTarget(site + 2, 2, 6);
        a.raw({0x2B, 0x05}).rip(&S->ticks60);                       // sub eax, [ticks60]
        a.raw({0x89, 0x05}).rip(counter);                           // mov [counter], eax
        a.jmp(site + 8);
    });
    for (const SigSite* s : {&kSw3_LO_PlFallReturnWait, &kSw3_LO_PlStgBeamWait, &kSw3_LO_PlStgBeamWait300,
                             &kSw3_LO_PlGraceFrame1A94})
        DecMemByTicks(*s);
    // Scripted hover/shoot states: sway velocity smoothing and decay by a register that
    // holds 0.1 and is reused later.
    const float* lerp01 = NewScaled("player sway smoothing 0.1", Scale::Lerp, 0.1f);
    const float* pow01 = NewScaled("player sway decay 0.1", Scale::Pow, 0.1f);
    RewriteSite(kSw3_LO_PlSwayVelLerpA, {{4, 4, kMulss, 1, lerp01}});  // mulss xmm1, xmm7
    RewriteSite(kSw3_LO_PlSwayVelLerpB, {{4, 5, kMulss, 1, lerp01}});  // mulss xmm1, xmm8
    RewriteSite(kSw3_LO_PlSwayVelDecayA, {{0, 4, kMulss, 0, pow01}});  // mulss xmm0, xmm7
    RewriteSite(kSw3_LO_PlSwayVelDecayB, {{0, 5, kMulss, 0, pow01}});  // mulss xmm0, xmm8
    LerpObjectFactor(kSw3_LO_EmHitPullY);
    // Baa020: speed ramps up/down by a per-object step each frame, phased by a frame
    // counter; its state duration counter feeds an exactly-zero branch.
    PatchSite(kSw3_LO_Baa020SpeedRampUp, [](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm0, [rbx+0x2D74]
        EmitSseRip(a, kMulss, 0, E());
        a.jmp(site + 8);
    });
    PatchSite(kSw3_LO_Baa020SpeedRampDown, [](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x8B}).copy(site + 12, 4);         // movss xmm1, [rbx+0x2D74]
        EmitSseRip(a, kMulss, 1, E());
        a.copy(site, 8);                                            // movss xmm0, [rbx+0x2D70]
        a.raw({0xF3, 0x0F, 0x5C, 0xC1});                            // subss xmm0, xmm1
        a.jmp(site + 16);
    });
    PatchSite(kSw3_LO_Baa020RampFrameCtr, [](Asm& a, uint8_t* site) {
        a.raw({0x03, 0x05}).rip(&S->ticks60);                       // add eax, [ticks60] (was inc eax)
        a.copy(site + 2, 8);                                        // xor ecx, ecx; mov [rbx+0x2D78], eax
        a.jmp(site + 10);
    });
    ZeroBranchOnTick(kSw3_LO_Baa020StateFrames, 8);
    // Ambient audio (sound worker, kicked once per frame): rays live 64 frames and 8 are
    // spawned per frame -> 64 ticks and 8 per tick, so the openness average spans the same time.
    ZeroBranchOnTick(kSw3_LO_AmbRayLifetime, 4);
    if (uint8_t* site = SiteOf(kSw3_LO_AmbRaySpawnGate)) GateCallOnTick(kSw3_LO_AmbRaySpawnGate.name, site);
}

// Pods (analysis/pod_patches.tsv): orbit angle, follow-factor ramp and target smoothing,
// bone and arm aim, program spins and beam growth, Pod B laser burst, keep-alive and
// beam lengths, skill model lifetimes.
static void FixPods() {
    InstallTable(kPodScaledConsts, _countof(kPodScaledConsts));
    InstallTable(kPodArgCalls, _countof(kPodArgCalls));
    // [0x1810] += (target - [0x1810]) * 0.05 per frame; xmm11 (0.05) is also the default follow factor.
    const float* lerp005 = NewScaled("pod follow ramp 0.05", Scale::Lerp, 0.05f);
    RewriteSite(kPod_POD_Ramp1810_Bv10, {{0, 5, kMulss, 1, lerp005}});
    RewriteSite(kPod_POD_Ramp1810_Fight, {{0, 5, kMulss, 0, lerp005}});
    RewriteSite(kPod_POD_Ramp1810_180C, {{0, 5, kMulss, 0, lerp005}});
    // Pod B laser lv3/lv4 follow-up burst: one shot per frame -> one per 60 Hz tick.
    if (uint8_t* site = SiteOf(kPod_POD_LaserBurstGate)) GateCallOnTick(kPod_POD_LaserBurstGate.name, site);
    DecMemByTicks(kPod_POD_LaserKeepAlive);
    // Beam length += 1 per frame (xmm8 = 1.0 is also used as w).
    RewriteSite(kPod_POD_LaserLength, {{0, 5, kAddss, 0, E()}});
    // Reflected segment length += 1 per frame (xmm6 = 1.0 is also w), then its cap compare.
    if (uint8_t* site = SiteOf(kPod_POD_LaserSegLength)) {
        const void* cap = RipTarget(site + 4, 3, 7);
        RewriteSite(kPod_POD_LaserSegLength, {{0, 4, kAddss, 0, E()}, {4, 7, kComiss, 0, cap}});
    }
    DecMemByTicks(kPod_POD_SpearModelLife);
    DecEaxByTicks(kPod_POD_HandModelLife40);
    DecEaxByTicks(kPod_POD_HandModelLife300);
}

void FixWorld() {
    Log("world:");
    InstallTable(kSw1ScaledConsts, _countof(kSw1ScaledConsts));
    InstallTable(kSw1ArgCalls, _countof(kSw1ArgCalls));
    InstallTable(kSw1VecCalls, _countof(kSw1VecCalls));
    // Flight-unit attach state: 5-frame delay counter.
    DecEaxByTicks(kSw1_SW1_PlFU_DelayCtr);

    InstallTable(kSw2ScaledConsts, _countof(kSw2ScaledConsts));
    InstallTable(kSw2ArgCalls, _countof(kSw2ArgCalls));
    // Weapon pose: 10-frame float counter (xmm6 = 1.0 is also used as w, so patch the use).
    RewriteSite(kSw2_SW2_WeaponPoseFrames, {{0, 4, kSubss, 0, E()}});
    // Light/environment transition: parameters += a precomputed per-frame step while a
    // frame counter runs down.
    if (uint8_t* site = SiteOf(kSw2_SW2_LightTransitionGate)) GateCallOnTick(kSw2_SW2_LightTransitionGate.name, site);
    FixBossGauge();
    // Core menu waits counted in frames.
    PatchSite(kSw2_SW2_CoreMenuCtrA, [](Asm& a, uint8_t* site) {   // dec dword [rdi+0x294]; cmp ...; jg
        a.raw({0x50});                                              // push rax
        a.raw({0x8B, 0x05}).rip(&S->ticks60);                       // mov eax, [ticks60]
        a.raw({0x29, 0x87}).copy(site + 2, 4);                      // sub [rdi+0x294], eax
        a.raw({0x58});                                              // pop rax
        a.jmp(site + 6);
    });
    ZeroBranchOnTick(kSw2_SW2_CoreMenuCtrB, 7);
    ZeroBranchOnTick(kSw2_SW2_CoreMenuCtrC, 7);
    // Operating-ratio display counts up/down by 1 per frame toward an integer target.
    RewriteSite(kSw2_SW2_OpRatioStepDown, {{0, 8, kSubss, 0, &S->ticks60f}});
    RewriteSite(kSw2_SW2_OpRatioStepUp, {{0, 8, kAddss, 0, &S->ticks60f}});
    // Pollution gauge fill: increment per frame -> * e.
    ScaleThenReplay(kSw2_SW2_PollutionFillStep, 0);

    FixRemaining();
    FixPods();
}
