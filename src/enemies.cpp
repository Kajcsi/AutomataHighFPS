// Bullets and enemy AI. The shared bullet update (arc gravity and speed-up, spin,
// homing, speed decay, wobble, guided lines, gravity wells), bullet types that move
// without the frame delta, the common enemy movement smoothing and steps, boss and
// NPC motion, and enemy frame counters (hit windows, cooldowns, waits).
// Sites come from analysis/bullet_enemy_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/bullet_enemy.inc"

static const float* E() { return &S->stepUpToOne; }

static void FixBullets() {
    // Arc shots: vel.y -= gravity once per frame -> gravity * e.
    PatchSite(kBlt_BLT_ArcGravity, [](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm0, [rsi+0xE04]
        a.raw({0xF3, 0x0F, 0x10, 0x8E}).copy(site + 12, 4);         // movss xmm1, [rsi+0xE98]
        EmitSseRip(a, kMulss, 1, E());
        a.raw({0xF3, 0x0F, 0x5C, 0xC1});                            // subss xmm0, xmm1
        a.copy(site + 16, 8);                                       // movss [rsi+0xE04], xmm0
        a.jmp(site + 24);
    });
    // Homing: rotate toward the target by fraction [0xE48] (smoothing), at most [0xE4C] (per-frame angle).
    PatchSite(kBlt_BLT_HomingTurn, [](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm0, [rsi+0xE4C]
        EmitSseRip(a, kMulss, 0, E());
        a.copy(site + 13, 8);                                       // movss xmm1, [rsi+0xE48]
        EmitFloatCall(a, &LerpE, 1, 1);
        a.copy(site + 8, 5);                                        // lea r8, [rsp+0x70]
        a.jmp(site + 21);
    });
    // Speed decay: speed *= 1 - [0x11B8] per frame -> (1 - k)^e.
    PatchSite(kBlt_BLT_SpeedDecay, [](Asm& a, uint8_t* site) {
        a.copy(site, 12);                                           // xmm0 = 1 - [rsi+0x11B8]
        EmitFloatCall(a, &PowE, 0, 0);
        a.copy(site + 12, 8);                                       // mulss xmm0, [rsi+0x11BC]
        a.jmp(site + 20);
    });
    // Guided-line shots: float frame counter += 1.0 -> += e.
    PatchSite(kBlt_BLT_LineCounter, [](Asm& a, uint8_t* site) {
        a.copy(site, 13);                                           // movss xmm0, [rsi+0x1530]; lea r8, [rsp+0x30]
        EmitSseRip(a, kAddss, 0, E());
        a.jmp(site + 18);
    });
    // Gravity well: bullet position += pull vector [rsp+0x30] per frame -> pull * e
    // (scaled copy in the dead temporary at [rsp+0x40]; w unchanged).
    PatchSite(kBlt_BLT_GravityWellPull, [](Asm& a, uint8_t* site) {
        a.raw({0x0F, 0x10, 0x44, 0x24, 0x30});                     // movups xmm0, [rsp+0x30]
        a.raw({0x0F, 0x11, 0x44, 0x24, 0x40});                     // movups [rsp+0x40], xmm0
        for (uint8_t off : {0x40, 0x44, 0x48}) {
            a.raw({0xF3, 0x0F, 0x10, 0x44, 0x24, off});             // movss xmm0, [rsp+off]
            EmitSseRip(a, kMulss, 0, E());
            a.raw({0xF3, 0x0F, 0x11, 0x44, 0x24, off});             // movss [rsp+off], xmm0
        }
        a.raw({0x4C, 0x8D, 0x44, 0x24, 0x40});                     // lea r8, [rsp+0x40]
        a.copy(site + 5, 4);                                        // lea rdx, [rsi+0x50]
        a.jmp(site + 9);
    });
}

static void FixEnemies() {
    // Common enemy movement (about 20 enemy classes): yaw, angle and speed smoothing
    // multiply by a register holding 0.1 that the delta-aware turn helper also uses,
    // so each use is patched instead of the constant.
    const float* lerp01 = NewScaled("enemy movement smoothing 0.1", Scale::Lerp, 0.1f);
    RewriteSite(kBlt_EM_Move_YawLerp_275C4, {{0, 5, kMulss, 0, lerp01}});    // mulss xmm0, xmm10
    RewriteSite(kBlt_EM_Move_AngLerp_275BC, {{0, 5, kMulss, 8, lerp01}});    // mulss xmm8, xmm10
    RewriteSite(kBlt_EM_Move_SpeedLerp_275C0, {{0, 5, kMulss, 6, lerp01}});  // mulss xmm6, xmm10
    RewriteSite(kBlt_EM_KeepDist_Lerp, {{0, 4, kMovss, 2, lerp01}});         // movaps xmm2, xmm8
    RewriteSite(kBlt_EM_Approach_Step, {{0, 3, kMovss, 2, NewScaled("enemy approach step 0.1", Scale::Lin, 0.1f)}});
    // Drift velocity: vel += (target - vel) * k (k varies) -> LerpE(k); position += vel -> vel * e.
    PatchSite(kBlt_EM_DriftVelLerp, [](Asm& a, uint8_t* site) {
        a.copy(site, 3);                                            // movaps xmm2, xmm7
        EmitFloatCall(a, &LerpE, 2, 2);
        a.copy(site + 3, 5);                                        // lea rdx, [rsp+0x50]
        a.jmp(site + 8);
    });
    PatchSite(kBlt_EM_DriftVelMove, [](Asm& a, uint8_t* site) {
        a.raw({0x0F, 0x10, 0x87}).copy(site + 3, 4);                // movups xmm0, [rdi+0x2C340]
        EmitSseRip(a, kMovss, 1, E());
        a.raw({0x0F, 0xC6, 0xC9, 0x00});                            // shufps xmm1, xmm1, 0
        a.raw({0x0F, 0x59, 0xC1});                                  // mulps xmm0, xmm1
        a.raw({0x0F, 0x11, 0x44, 0x24, 0x60});                     // movups [rsp+0x60], xmm0 (dead temporary)
        a.raw({0x48, 0x8D, 0x54, 0x24, 0x60});                     // lea rdx, [rsp+0x60]
        a.copy(site + 7, 3);                                        // mov rcx, rdi
        a.jmp(site + 10);
    });
    // NPC "come out" states: rise by a smoothed per-frame velocity.
    ScaleThenReplay(kBlt_SW_JetComeOut_PosY, 1);
    ScaleThenReplay(kBlt_SW_ComeOut_PosY, 1);

    for (const SigSite* s : {&kBlt_EM_Win2C328, &kBlt_EM_Cd27E64, &kBlt_EM_Win27E5C, &kBlt_EM_TimeOverrideCtr,
                             &kBlt_POD_Ctr181C, &kBlt_EM_Wait275DC, &kBlt_EM_Wait275E0, &kBlt_EM_FxInterval2C378,
                             &kBlt_EM_Wait275DC_b, &kBlt_EM_TimeReqDelay6178, &kBlt_EM_TimeReqDur6174})
        DecEaxByTicks(*s);
}

void FixBulletsAndEnemies() {
    Log("bullets and enemies:");
    InstallTable(kBltScaledConsts, _countof(kBltScaledConsts));
    InstallTable(kBltArgCalls, _countof(kBltArgCalls));
    InstallTable(kBltVecCalls, _countof(kBltVecCalls));
    FixBullets();
    FixEnemies();
}
