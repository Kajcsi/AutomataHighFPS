// Flight unit: shooter sections, behind-view flight battles, free flight and
// robot mode. Their movement, smoothing and turn rates are applied once per
// frame; every one of them is rescaled to the frame's share of a 60 Hz frame.
// Sites come from analysis/flight_unit_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/flight_unit.inc"

// `mulss xmmD, xmmS` where xmmS holds a 0.1 smoothing factor that is also used for
// other purposes -> multiply by a frame-scaled 0.1 instead.
static void ScaledMulssReg(const SigSite& s) {
    float* k = NewScaled(s.name, Scale::Lerp, 0.1f);
    uint8_t dst = (s.orig[4] >> 3) & 7;  // ModRM reg field of F3 41 0F 59 /r
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x59, (uint8_t)(0x05 | dst << 3)}).rip(k);  // mulss xmmD, [k]
        a.jmp(site + s.len);
    });
}

// `movss [rsp+0x20], xmmN` passing a 0.1 slerp rate on the stack -> pass a scaled rate (xmm0 is free).
static void ScaledStackRate(const SigSite& s) {
    float* k = NewScaled(s.name, Scale::Lerp, 0.1f);
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x05}).rip(k);          // movss xmm0, [k]
        a.raw({0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20});     // movss [rsp+0x20], xmm0
        a.jmp(site + s.len);
    });
}

// `movss xmm2, [rdi+0x275C0]` (curve-following enemy speed) -> speed * e.
static void ScaledEnemySpeed(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x97, 0xC0, 0x75, 0x02, 0x00});  // movss xmm2, [rdi+0x275C0]
        a.raw({0xF3, 0x0F, 0x59, 0x15}).rip(&S->stepUpToOne);     // mulss xmm2, [e]
        a.jmp(site + s.len);
    });
}

// `call cameraRate(…, xmm3 = rate)` -> rate = 1 - (1-rate)^e.
static void ScaledCameraRate(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        uint8_t* callee = RipTarget(site, 1, 5);
        EmitFloatCall(a, &LerpE, 3, 3);
        a.call(callee);
        a.jmp(site + 5);
    });
}

// `addss xmm0, [rdi+disp8]` (laser beam length += growth) -> growth * e.
static void ScaledLaserGrowth(const SigSite& s) {
    uint8_t disp8 = s.orig[4];
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x59, 0x05}).rip(&S->stepUpToOne);  // mulss xmm0, [e]
        a.raw({0xF3, 0x0F, 0x58, 0x47, disp8});                // addss xmm0, [rdi+disp8]
        a.jmp(site + s.len);
    });
}

void FixFlightUnit() {
    Log("flight unit:");
    InstallTable(kFlightScaledConsts, _countof(kFlightScaledConsts));
    InstallTable(kFlightArgCalls, _countof(kFlightArgCalls));
    InstallTable(kFlightVecCalls, _countof(kFlightVecCalls));

    ScaledCameraRate(kFlight_R8_AirBezier_camRate);
    ScaledCameraRate(kFlight_R9_AirCurve_camRate);
    ScaledMulssReg(kFlight_P9_PlTilt_15c14);
    ScaledMulssReg(kFlight_E6_EmApproach_acc);
    ScaledMulssReg(kFlight_E7_EmExit_acc);
    ScaledMulssReg(kFlight_E10_EmShotAim1);
    ScaledMulssReg(kFlight_E11_EmShotAim2);
    ScaledStackRate(kFlight_P12_PlSlerpRate);
    ScaledStackRate(kFlight_P14_PlTurnRate1);
    ScaledStackRate(kFlight_P15_PlTurnRate2);
    ScaledStackRate(kFlight_E8_EmFaceRate);
    ScaledEnemySpeed(kFlight_E1_EmCurve_vel);
    ScaledEnemySpeed(kFlight_E2_EmCurve_vel);
    ScaledEnemySpeed(kFlight_E3_EmApproach_vel);
    ScaledEnemySpeed(kFlight_E4_EmExit_vel);

    // Shooter bullets and missiles: travelled distance += |v| * scale per frame.
    PatchSite(kFlight_B1_StgBullet_dist, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x59, 0xC6});                       // mulss xmm0, xmm6
        a.raw({0xF3, 0x0F, 0x59, 0x05}).rip(&S->stepUpToOne);  // mulss xmm0, [e]
        a.raw({0xF3, 0x0F, 0x58, 0x87, 0xE0, 0x13, 0x00, 0x00});  // addss xmm0, [rdi+0x13E0]
        a.jmp(site + 12);
    });
    // Homing strength decays by a per-bullet factor every frame: strength *= factor^e.
    PatchSite(kFlight_B2_Homing_decay, [&](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x87, 0x0C, 0x15, 0x00, 0x00});  // movss xmm0, [rdi+0x150C]
        EmitFloatCall(a, &PowE, 0, 0);                            // xmm0 = factor^e
        a.raw({0xF3, 0x0F, 0x59, 0x87, 0x04, 0x15, 0x00, 0x00});  // mulss xmm0, [rdi+0x1504]
        a.jmp(site + 16);
    });

    // Lasers: spawn interval and phase counters in frames, beam growth per frame.
    PatchSite(kFlight_L1_LaserSpawnCtr, [&](Asm& a, uint8_t* site) {
        a.raw({0x2B, 0x05}).rip(&S->ticks60);                     // sub eax, [ticks60]
        a.raw({0x41, 0x89, 0x85, 0xD0, 0x00, 0x00, 0x00});        // mov [r13+0xD0], eax
        a.jmp(site + 9);
    });
    PatchSite(kFlight_L2_LaserPhaseCtr1, [&](Asm& a, uint8_t* site) {
        a.raw({0x50});                                            // push rax
        a.raw({0x8B, 0x05}).rip(&S->ticks60);                     // mov eax, [ticks60]
        a.raw({0x29, 0x47, 0x50});                                // sub [rdi+0x50], eax
        a.raw({0x58});                                            // pop rax
        a.raw({0x83, 0x7F, 0x50, 0x00});                          // cmp dword [rdi+0x50], 0
        a.jmp(site + 7);
    });
    PatchSite(kFlight_L3_LaserPhaseCtr2, [&](Asm& a, uint8_t* site) {
        a.raw({0x50});                                            // push rax
        a.raw({0x8B, 0x05}).rip(&S->ticks60);                     // mov eax, [ticks60]
        a.raw({0x29, 0x47, 0x50});                                // sub [rdi+0x50], eax
        a.raw({0x58});                                            // pop rax
        a.raw({0x39, 0x77, 0x50});                                // cmp [rdi+0x50], esi
        a.jmp(site + 6);
    });
    ScaledLaserGrowth(kFlight_L4_LaserGrow1);
    ScaledLaserGrowth(kFlight_L5_LaserGrow2);

    // Scroll world-speed change: speed += (target - speed) * R per frame, R set by script.
    if (uint8_t* data = SiteOf(kFlight_DATA_ScrollWorldRate)) {
        const float* rate = (const float*)RipTarget(data, 4, 8);
        float* k = NewScaled("scroll world-speed rate", Scale::Lerp, 0.0f, rate);
        PatchSite(kFlight_S1_ScrollWorldSpd, [&](Asm& a, uint8_t* site) {
            a.raw({0xF3, 0x0F, 0x59, 0x0D}).rip(k);  // mulss xmm1, [k]
            a.jmp(site + 8);
        });
    }
}
