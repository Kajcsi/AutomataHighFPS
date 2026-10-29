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

static const float* E() { return &S->stepUpToOne; }

// `sub dword [mem], 1` followed by a branch on exactly zero. On a frame without a 60 Hz
// tick the counter is left alone and the flags say "not zero", so the action can't
// fire twice while the counter sits at 0.
static void SubOneOnTick(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0x83, 0x3D}).rip(&S->ticks60, 1).raw({0x00});  // cmp dword [ticks60], 0
        a.raw({0x74, (uint8_t)(s.len + 5)});                   // je noTick
        a.copy(site, s.len);                                   // the original sub
        a.jmp(site + s.len);
        a.raw({0x50, 0x31, 0xC0, 0xFF, 0xC0, 0x58});           // noTick: push rax; xor eax, eax; inc eax; pop rax
        a.jmp(site + s.len);
    });
}

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
    SubOneOnTick(kSw2_SW2_CoreMenuCtrB);
    SubOneOnTick(kSw2_SW2_CoreMenuCtrC);
    // Operating-ratio display counts up/down by 1 per frame toward an integer target.
    RewriteSite(kSw2_SW2_OpRatioStepDown, {{0, 8, kSubss, 0, &S->ticks60f}});
    RewriteSite(kSw2_SW2_OpRatioStepUp, {{0, 8, kAddss, 0, &S->ticks60f}});
    // Pollution gauge fill: increment per frame -> * e.
    ScaleThenReplay(kSw2_SW2_PollutionFillStep, 0);
}
