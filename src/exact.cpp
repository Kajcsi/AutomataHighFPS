// Smoothing the game already scales by the frame delta, but only linearly: a fraction
// f = k*d (d = this frame's delta in 60 Hz frames) applied once per frame converges
// faster than at 60 FPS when d < 1 (k = 0.2: about 7% at 240 FPS). These sites get
// the exact 1-(1-min(k*d/e, 1))^e, which is identical at 60 FPS and keeps slow motion
// (d/e is the time scale above 60 FPS). Covered: the shared enemy and NPC turn helpers,
// the turn-to-point helper and many Approach callers (pods, animals, player, enemies,
// camera modes), and two scalar position lerps.
// Sites come from analysis/exact_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/exact.inc"

// `mulss xmm0, xmm1; ...` where xmm1 = (target - x) * k (from `mulss xmm1, [k]` just
// before the site) and xmm0 = the delta: xmm0 -> (1-(1-k*d/e)^e) / k. At e = 1 the
// original instructions run unchanged.
static void ExactScalarStep(const SigSite& s, float expectedK) {
    uint8_t* site = SiteOf(s);
    if (!site) return;
    const uint8_t* prev = site - 8;
    const float* k = (const float*)RipTarget((uint8_t*)prev, 4, 8);
    if (prev[0] != 0xF3 || prev[1] != 0x0F || prev[2] != 0x59 || prev[3] != 0x0D || *k != expectedK) {
        Log("  [skip] %s: unexpected bytes before the site", s.name);
        return;
    }
    InstallCaveUnchecked(s.name, site, s.len, false, [&](Asm& a) {
        a.raw({0x81, 0x3D}).rip(&S->stepUpToOne, 4).i32(0x3F800000);  // cmp dword [e], 1.0
        uint8_t* je = a.jccForward(0x4);                                // je original
        EmitSseRip(a, kMulss, 0, k);
        EmitSseRip(a, kMulss, 0, InvE());
        EmitFloatCall(a, &LerpE, 0, 0);
        EmitSseRip(a, 0x5E, 0, k);                                      // divss xmm0, [k]
        a.bind(je);
        a.copy(site, s.len);
        a.jmp(site + s.len);
    });
}

void FixExactSmoothing() {
    Log("exact delta-scaled smoothing:");
    // Turn helpers: fraction = delta * rate, computed by `movaps xR, xmm0; mulss xR, rate`.
    ExactFraction(kEx_EX_TurnFE650_Frac, 8, 6);
    ExactFraction(kEx_EX_TurnFE400_Frac, 8, 6);
    ExactFraction(kEx_EX_TurnFE4E0_Frac, 8, 7);
    ExactFraction(kEx_EX_TurnNPC414DD0_Frac, 8, 6);
    // Pod orbit position: pos += ((target - pos) * 0.2) * podDelta in two vector scales.
    ExactArgCall(kEx_EX_PodOrbitPosA, 0.2f);
    ExactArgCall(kEx_EX_PodOrbitPosB, 0.2f);
    ExactScalarStep(kEx_EX_Em0110Parts_C98Lerp, 0.3f);
    ExactScalarStep(kEx_EX_Funnel_54Lerp, 0.1f);
    // Approach / turn-to-point calls whose fraction argument (xmm2) is delta * rate.
    for (const SigSite* s :
         {&kEx_EX_TurnFE7B0_Approach, &kEx_EX_PodFollowMain, &kEx_EX_PodOrbitYaw, &kEx_EX_PodFaceYaw,
          &kEx_EX_PodOwnerFollow, &kEx_EX_Ba0110_YawApproach, &kEx_EX_Baa_YawApproach_6557A7,
          &kEx_EX_Baa_YawApproach_6662CE, &kEx_EX_Baa_YawApproach_66642A, &kEx_EX_Baa_YawApproach_666D5C,
          &kEx_EX_Baa_YawApproach_66B032, &kEx_EX_Baa_YawApproach_66B186, &kEx_EX_Baa_YawApproach_66B3B3,
          &kEx_EX_Baa_YawApproach_66BCCE, &kEx_EX_Baa_YawApproach_66FFB2, &kEx_EX_PlFlight_RollApproach,
          &kEx_EX_PlFlight_ShipApproach, &kEx_EX_Pl_1A74_Approach, &kEx_EX_Pl_16A7C_Approach,
          &kEx_EX_Pl_16A80_Approach, &kEx_EX_Em_5B7E10_YawApproach, &kEx_EX_Em_5B82C0_YawApproach,
          &kEx_EX_Shared_5ECC60_ApproachX, &kEx_EX_Shared_5ECC60_ApproachY, &kEx_EX_Em2000_YawApproach,
          &kEx_EX_Em4000Core_Approach, &kEx_EX_Turn_6CA040_Approach, &kEx_EX_Em3000_Approach,
          &kEx_EX_Funnel_YawApproachA, &kEx_EX_Funnel_YawApproachB, &kEx_EX_Funnel_YawApproachC,
          &kEx_EX_CAM_M08_PitchApproach, &kEx_EX_CAM_M08_YawApproach, &kEx_EX_CAM_M08_YawApproach2,
          &kEx_EX_CAM_M1A_YawApproach, &kEx_EX_CAM_M1B_YawApproach, &kEx_EX_CAM_M1C_Approach,
          &kEx_EX_CAM_M18_Approach, &kEx_EX_CAM_M19_YawApproach, &kEx_EX_CAM_M17_PitchApproach,
          &kEx_EX_CAM_M17_YawApproach, &kEx_EX_CAM_M17_YawApproach2, &kEx_EX_CAM_DEF_PitchAuto,
          &kEx_EX_CAM_DEF_PitchStick, &kEx_EX_CAM_DEF_YawStick, &kEx_EX_CAM_DEF_YawFollow,
          &kEx_EX_CAM_DEF_YawFollow2, &kEx_EX_CAM_DEF_PitchFollow, &kEx_EX_CAM_7330C0_YawApproach,
          &kEx_EX_CAM_7330C0_YawApproach2, &kEx_EX_CAM_7330C0_PitchApproach, &kEx_EX_CAM_M12_YawApproach,
          &kEx_EX_CAM_M13_YawApproach, &kEx_EX_CAM_M14_YawApproach, &kEx_EX_CAM_M16_YawApproach,
          &kEx_EX_CAM_M0F_YawApproach, &kEx_EX_CAM_M11_YawApproach, &kEx_EX_CAM_M10_YawApproach,
          &kEx_EX_CAM_M15_YawApproach})
        ExactArgCall(*s);
}
