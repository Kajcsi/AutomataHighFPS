// Third-person camera. Each camera mode ramps its follow factors, smooths angles,
// distances and offsets, steps angular impulses and counts frames once per frame.
// Ramps become c*e, per-frame smoothing 1-(1-k)^e, impulses and steps *e, frame
// counters 60 Hz ticks. The transition blend keeps its stored rate raw and converts
// only where the rate is applied. Fractions the camera already multiplies by the frame delta
// (min(delta*k, 1)) are turned into the exact 1-(1-min(delta*k/e, 1))^e.
// Sites come from analysis/camera_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/camera.inc"
#include "generated/camera_blend.inc"

static const float* E() { return &S->stepUpToOne; }

// xmm9 = LerpE(xmm9) through xmm0 (xmm0 must be dead).
static void EmitLerpXmm9(Asm& a) {
    a.raw({0x41, 0x0F, 0x28, 0xC1});  // movaps xmm0, xmm9
    EmitFloatCall(a, &LerpE, 0, 0);
    a.raw({0x44, 0x0F, 0x28, 0xC8});  // movaps xmm9, xmm0
}

// Transition blend (0x14073B8A0): the rate at +0x698 and the fractions derived from it
// stay exactly as vanilla computes and stores them; only where a fraction is applied to
// a camera value (once per frame) does it become 1-(1-f)^e.
static void FixCameraBlend() {
    // +0x698 rate smoothing toward its target.
    LerpObjectFactor(kCamB_CAM_BLEND_694);
    // Path A: xmm7 keeps the raw fraction (stored to +0x698), xmm9 the applied one.
    PatchSite(kCamB_CAM_BLEND_A_FINAL, [](Asm& a, uint8_t* site) {
        a.copy(site, 11);                                           // lea rdx, [rsi+0x140]; movaps xmm7, xmm9
        EmitFloatCall(a, &LerpE, 7, 0);
        a.raw({0x44, 0x0F, 0x28, 0xC8});                            // movaps xmm9, xmm0
        a.jmp(site + 11);
    });
    PatchSite(kCamB_CAM_BLEND_A_STORE, [](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x11, 0xBE}).copy(site + 5, 4);          // movss [rsi+0x698], xmm7 (raw)
        a.jmp(site + 9);
    });
    // Path B: blended fractions p*k + (1-p)*rate are final here and never stored.
    PatchSite(kCamB_CAM_BLEND_B1_FINAL, [](Asm& a, uint8_t* site) {
        a.copy(site, 5);                                            // addss xmm6, xmm8
        EmitFloatCall(a, &LerpE, 2, 2);
        EmitFloatCall(a, &LerpE, 3, 3);
        EmitFloatCall(a, &LerpE, 6, 6);
        a.jmp(site + 5);
    });
    PatchSite(kCamB_CAM_BLEND_B2_FINAL, [](Asm& a, uint8_t* site) {
        a.copy(site, 5);                                            // addss xmm9, xmm0
        EmitLerpXmm9(a);
        a.jmp(site + 5);
    });
    // Path C: xmm9 is a temporary copy of the fraction (or its max with [0x508]).
    for (const SigSite* s : {&kCamB_CAM_BLEND_C_F0, &kCamB_CAM_BLEND_C_100}) {
        PatchSite(*s, [&](Asm& a, uint8_t* site) {
            EmitLerpXmm9(a);
            a.copy(site, s->len);                                   // lea r14/rdi, [rsi+...]
            a.jmp(site + s->len);
        });
    }
    // Path C scalar and vector lerps by xmm7, which stays raw for the store.
    for (const SigSite* s : {&kCamB_CAM_BLEND_C_138, &kCamB_CAM_BLEND_C_130}) {
        PatchSite(*s, [&](Asm& a, uint8_t* site) {
            a.copy(site, 4);                                        // subss xmm1, xmm0
            EmitFloatCall(a, &LerpE, 7, 2);
            a.raw({0xF3, 0x0F, 0x59, 0xCA});                        // mulss xmm1, xmm2
            a.jmp(site + 8);
        });
    }
    PatchSite(kCamB_CAM_BLEND_C_120, [](Asm& a, uint8_t* site) {
        EmitFloatCall(a, &LerpE, 7, 2);                             // movaps xmm2, xmm7 -> LerpE
        a.copy(site + 3, 4);                                        // lea rdx, [rbp-0x19]
        a.jmp(site + 7);
    });
    // Epilogue: the orientation slerp toward the target by xmm7 each frame.
    PatchSite(kCamB_CAM_BLEND_EPI_SLERP, [](Asm& a, uint8_t* site) {
        EmitFloatCall(a, &LerpE, 7, 1);                             // movaps xmm1, xmm7 -> LerpE
        a.copy(site + 3, 3);                                        // mov rcx, rsi
        a.jmp(site + 6);
    });
}

static const float* Constant(float v) {
    float* p = (float*)g_arena.Alloc(4, 4);
    if (p) *p = v;
    return p;
}

// `movss xR, [state]; addss xR, [velocity]` (8-byte instructions at `movAt`/`addAt`, R < 8):
// state + velocity applied once per frame -> state + velocity * e.
static void EmitScaledAdd(Asm& a, const uint8_t* site, int movAt, int addAt, uint8_t reg) {
    a.raw({0xF3, 0x0F, 0x10}).copy(site + addAt + 3, 5);  // movss xR, [velocity]
    EmitSseRip(a, kMulss, reg, E());
    a.raw({0xF3, 0x0F, 0x58}).copy(site + movAt + 3, 5);  // addss xR, [state]
}

static void ScaledAdd(const SigSite& s, uint8_t reg) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        EmitScaledAdd(a, site, 0, 8, reg);
        a.jmp(site + s.len);
    });
}

// The original fraction f = delta * k (computed by `prefix` bytes of the site, in xmm<reg>)
// -> 1-(1-min(f/e, 1))^e, then the rest of the site.
static void ExactFraction(const SigSite& s, size_t prefix, uint8_t reg, const float* inv) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.copy(site, prefix);
        EmitSseRip(a, kMulss, reg, inv);
        if (prefix == s.len) EmitSseRip(a, kMinss, reg, NearOne());
        else a.copy(site + prefix, s.len - prefix);  // the original minss with 1.0
        EmitFloatCall(a, &LerpE, reg, reg);
        a.jmp(site + s.len);
    });
}

void FixCamera() {
    Log("camera:");
    InstallTable(kCamScaledConsts, _countof(kCamScaledConsts));
    InstallTable(kCamScaledDoubles, _countof(kCamScaledDoubles));
    InstallTable(kCamArgCalls, _countof(kCamArgCalls));

    // Frame counters.
    for (const SigSite* s : {&kCam_CAM_8394_RECENTER_LOCK, &kCam_CAM_LAG_540_SNAPFRAMES, &kCam_CAM_85C8_FRAMES,
                             &kCam_CAM_APPLY_20C_SNAP_A, &kCam_CAM_APPLY_20C_SNAP_B})
        DecEaxByTicks(*s);
    RewriteSite(kCam_CAM_TRK_8404_FRAMES, {{0, 5, kSubss, 0, E()}});  // subss xmm0, xmm10 (1.0)
    RewriteSite(kCam_CAM_M0A_3A8_FRAMES, {{0, 4, kSubss, 0, E()}});   // subss xmm0, xmm6 (1.0)

    // Per-object smoothing factors.
    for (const SigSite* s : {&kCam_CAM_PLC1_OFS_A, &kCam_CAM_PLC1_OFS_B, &kCam_CAM_PLC2_OFS_A, &kCam_CAM_PLC2_OFS_B,
                             &kCam_CAM_LOOKOFS_830C, &kCam_CAM_OFSELEM_LERP, &kCam_CAM_M17_8388, &kCam_CAM_M08_8388,
                             &kCam_CAM_M08_808, &kCam_CAM_M1B_8388, &kCam_CAM_DEF_8388, &kCam_CAM_DEF_8398})
        LerpObjectFactor(*s);
    // Factors already loaded into a register: smooth them before use.
    LerpAround(kCam_CAM_M17_837C, 1, false);
    LerpAround(kCam_CAM_M1B_837C, 1, false);
    LerpAround(kCam_CAM_DEF_837C, 2, false);
    LerpAround(kCam_CAM_APPLY_22C, 3, true);
    LerpAround(kCam_CAM_APPLY_230, 3, true);
    LerpAround(kCam_CAM_APPLY_240_USE, 2, true);
    LerpAround(kCam_CAM_APPLY_238_USE, 2, true);
    // Override fraction + its frame counter.
    PatchSite(kCam_CAM_APPLY_214_OVERRIDE, [](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm6, [rdi+0x218]
        EmitFloatCall(a, &LerpE, 6, 6);
        a.raw({0x2B, 0x05}).rip(&S->ticks60);                       // sub eax, [ticks60]
        a.copy(site + 10, 6);                                       // mov [rdi+0x214], eax
        a.jmp(site + 16);
    });
    PatchSite(kCam_CAM_APPLY_21C_OVERRIDE, [](Asm& a, uint8_t* site) {
        a.raw({0xF3, 0x0F, 0x10, 0x87}).copy(site + 5, 4);          // movss xmm0, [rdi+0x220] (xmm0 reloaded after)
        EmitFloatCall(a, &LerpE, 0, 0);
        a.raw({0x44, 0x0F, 0x28, 0xD8});                            // movaps xmm11, xmm0
        a.raw({0x2B, 0x05}).rip(&S->ticks60);                       // sub eax, [ticks60]
        a.copy(site + 11, 6);                                       // mov [rdi+0x21c], eax
        a.jmp(site + 17);
    });

    // Smoothing constants kept in a register that is also used for caps, rates or
    // compares: replace each smoothing use instead of the constant.
    const float* lerp01 = NewScaled("camera smoothing 0.1", Scale::Lerp, 0.1f);
    const float* lerp02 = NewScaled("camera smoothing 0.2", Scale::Lerp, 0.2f);
    const float* step05deg = NewScaled("camera approach step 0.5 deg", Scale::Lin, 0.00872665f);
    for (const SigSite* s : {&kCam_CAM_PLC1_DIST_550, &kCam_CAM_DEF_DIST_3B0, &kCam_CAM_DEF_8B08, &kCam_CAM_M03_DECAY_A,
                             &kCam_CAM_M03_DECAY_B, &kCam_CAM_M0E_3C0, &kCam_CAM_M0E_3B0}) {
        const uint8_t* op = s->orig + 1;  // REX
        uint8_t dst = (uint8_t)(((op[0] & 4) ? 8 : 0) | ((op[3] >> 3) & 7));
        RewriteSite(*s, {{0, 5, kMulss, dst, lerp01}});
    }
    RewriteSite(kCam_CAM_M1B_APPROACH_FRAC, {{8, 4, kMovss, 2, lerp02}});  // movaps xmm2, xmm9
    RewriteSite(kCam_CAM_M1C_3C0_FRAC, {{0, 4, kMovss, 2, lerp01}});
    RewriteSite(kCam_CAM_M1C_3C4_FRAC, {{0, 4, kMovss, 2, lerp01}});
    RewriteSite(kCam_CAM_M18_3C0_APPROACH, {{0, 4, kMovss, 3, step05deg}, {4, 4, kMovss, 2, lerp01}});
    RewriteSite(kCam_CAM_M18_3C4_APPROACH, {{0, 4, kMovss, 3, step05deg}, {12, 4, kMovss, 2, lerp01}});
    // Apply: xmm9 (0.1) is now the scaled smoothing factor; its clamp compare keeps 0.1.
    RewriteSite(kCam_CAM_APPLY_RATE_CMP, {{0, 4, kComiss, 6, Constant(0.1f)}});

    // Roll: xmm7 *= [rdi+0x698] * 0.1 (a per-frame fraction) -> LerpE of it.
    PatchSite(kCam_CAM_ROLL_EXT_LERP, [](Asm& a, uint8_t* site) {
        const void* k = RipTarget(site + 8, 4, 8);                  // the game's 0.1
        a.raw({0xF3, 0x0F, 0x10, 0x8F}).copy(site + 4, 4);          // movss xmm1, [rdi+0x698]
        EmitSseRip(a, kMulss, 1, k);
        EmitFloatCall(a, &LerpE, 1, 1);
        a.raw({0xF3, 0x0F, 0x59, 0xF9});                            // mulss xmm7, xmm1
        a.jmp(site + 16);
    });

    // Angular impulses: angle += impulse velocity once per frame.
    PatchSite(kCam_CAM_PLC1_ANGVEL, [](Asm& a, uint8_t* site) {
        EmitScaledAdd(a, site, 0, 16, 0);                           // xmm0 = [0x3f0] + [0x4ac]*e
        a.copy(site + 8, 8);                                        // movss xmm1, [0x4b0] (velocity)
        EmitSseRip(a, kMulss, 1, E());
        a.copy(site + 24, 8);                                       // addss xmm1, [0x3f4]
        a.jmp(site + 32);
    });
    for (const SigSite* s : {&kCam_CAM_PLC2_ANGVEL_P, &kCam_CAM_PLC2_ANGVEL_Y, &kCam_CAM_PLC3_ANGVEL_P, &kCam_CAM_PLC3_ANGVEL_Y})
        ScaledAdd(*s, 0);
    // Per-frame ramps of +0x8504.
    ScaleThenReplay(kCam_CAM_DEF_8504_UP, 0);
    ScaleThenReplay(kCam_CAM_DEF_8504_DN, 0);

    // Target lag: the Y speed is added once per frame. The register holding it differs
    // between the builds; read it from the `movss xR, [rbx+0x54c]` just before the site.
    if (uint8_t* site = SiteOf(kCam_CAM_LAG_STEP_Y)) {
        const uint8_t* prev = site - 8;
        if (prev[0] == 0xF3 && prev[1] == 0x0F && prev[2] == 0x10 && (prev[3] & 0xC7) == 0x83 &&
            *(const int32_t*)(prev + 4) == 0x54C) {
            uint8_t reg = (prev[3] >> 3) & 7;
            InstallCaveUnchecked(kCam_CAM_LAG_STEP_Y.name, site, 8, false, [&](Asm& a) {
                EmitSseRip(a, kMulss, reg, E());
                a.copy(site, 8);                                    // movss xmm0, [rbx+0x534]
                a.jmp(site + 8);
            });
        } else {
            Log("  [skip] %s: unexpected bytes before the site", kCam_CAM_LAG_STEP_Y.name);
        }
    }

    // Fractions already multiplied by the frame delta: exact 60 Hz equivalent.
    const float* inv = NewScaled("1/e", Scale::Inv, 1.0f);
    ExactFraction(kCam_CAM_TRK_YAW_FRAC_EXACT, 5, 0, inv);
    ExactFraction(kCam_CAM_TRK_PITCH_FRAC_EXACT, 5, 0, inv);
    ExactFraction(kCam_CAM_APPLY_FRAC_EXACT, 8, 6, inv);
    ExactFraction(kCam_HELPER_66ED60_EXACT, 5, 0, inv);

    FixCameraBlend();
}
