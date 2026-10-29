// Camera exactness: angular impulses (decay and integration together, including the
// linear-deceleration phase), gains that compose with the follow/blend fraction, and the
// remaining smoothing in modes M2A, M29, M18 and the default auto-yaw. Every cave runs the
// original instructions (or an identity) at e = 1.
// Sites come from analysis/camera_final_patches.tsv; the five angular-impulse sites are the
// existing CAM_PLC*_ANGVEL rows of analysis/camera_patches.tsv with new cave bodies.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"
#include "game.h"

#include "generated/camera_final.inc"

namespace {

const float* E() { return &S->stepUpToOne; }
const float* g_half;
const float* g_zero;

void CallGetDelta(Asm& a) {
    a.raw({0x48, 0x8D, 0x0D}).rip(G.deltaHandle);  // lea rcx, [handle]
    a.call((const void*)G.getDelta);
}

// xmm0 = W (the per-frame weight); angle += W*v for pitch (xmm0 out) and yaw (xmm1 out,
// also stored when `yawHere`).
void EmitApplyWeight(Asm& a, uint8_t base, bool yawHere, uint8_t* exit) {
    SseMem(a, kMovss, 1, base, 0x4B0);
    SseRR(a, kMulss, 1, 0);
    SseMem(a, kAddss, 1, base, 0x3F4);
    if (yawHere) SseMem(a, 0x11, 1, base, 0x3F4);
    SseMem(a, kMulss, 0, base, 0x4AC);
    SseMem(a, kAddss, 0, base, 0x3F0);
    a.jmp(exit);
}

// Angular impulse: vanilla adds the velocity to the angle, then decays it (v *= 1 - 0.5*d,
// or v -= d*a while the +0x4BC timer runs). Read this frame's delta, predict vanilla's next
// branch and add the exact 60 Hz amount: W*v with W = (1-(1-f)^e)/f, f = 0.5*d/e, or
// e*v + (1-e)/2*d*a in the linear phase.
void EmitAngVel(Asm& a, uint8_t* site, size_t len, uint8_t base, bool yawHere, bool keepRcx) {
    uint8_t* exit = site + len;
    CmpE1(a);
    uint8_t* s = a.jccForward(0x5);  // jne S
    a.copy(site, len);
    if (yawHere) {                   // vanilla's yaw step, moved up from the Y site
        SseMem(a, kMovss, 1, base, 0x3F4);
        SseMem(a, kAddss, 1, base, 0x4B0);
        SseMem(a, 0x11, 1, base, 0x3F4);
    }
    a.jmp(exit);
    a.bind(s);
    if (keepRcx) a.raw({0x51});                                       // push rcx
    a.raw({0x48, 0x83, 0xEC, (uint8_t)(keepRcx ? 0x28 : 0x20)});
    CallGetDelta(a);                                                  // xmm0 = d
    a.raw({0x48, 0x83, 0xC4, (uint8_t)(keepRcx ? 0x28 : 0x20)});
    if (keepRcx) a.raw({0x59});                                       // pop rcx
    SseRR(a, 0x57, 3, 3, false);                                      // xorps xmm3, xmm3
    SseMem(a, kMovss, 2, base, 0x4BC);
    SseRR(a, kComiss, 2, 3, false);
    uint8_t* e1 = a.jccForward(0x6);                                  // timer <= 0: exponential
    SseRR(a, kSubss, 2, 0);
    Movaps(a, 4, 0);
    EmitSseRip(a, kMulss, 4, InvE());
    SseRR(a, kSubss, 4, 0);
    SseRR(a, kComiss, 2, 4, false);
    uint8_t* e2 = a.jccForward(0x6);                                  // timer - d <= s - d: exponential
    EmitSseRip(a, kMovss, 2, NearOne());                              // linear: c = (1-e)/2*d
    EmitSseRip(a, kSubss, 2, E());
    EmitSseRip(a, kMulss, 2, g_half);
    SseRR(a, kMulss, 2, 0);
    SseMem(a, kMovss, 0, base, 0x4B4);
    SseRR(a, kMulss, 0, 2);
    SseMem(a, kMovss, 3, base, 0x4AC);
    EmitSseRip(a, kMulss, 3, E());
    SseRR(a, kAddss, 0, 3);
    SseMem(a, kAddss, 0, base, 0x3F0);
    SseMem(a, kMovss, 1, base, 0x4B8);
    SseRR(a, kMulss, 1, 2);
    SseMem(a, kMovss, 3, base, 0x4B0);
    EmitSseRip(a, kMulss, 3, E());
    SseRR(a, kAddss, 1, 3);
    SseMem(a, kAddss, 1, base, 0x3F4);
    if (yawHere) SseMem(a, 0x11, 1, base, 0x3F4);
    a.jmp(exit);
    a.bind(e1);
    a.bind(e2);
    EmitSseRip(a, kMulss, 0, g_half);                                 // f = 0.5*d/e
    EmitSseRip(a, kMulss, 0, InvE());
    Movaps(a, 2, 0);
    EmitSseRip(a, kMovss, 1, NearOne());
    SseRR(a, kSubss, 1, 0);
    EmitFloatCall(a, &PowE, 1, 1);                                    // (1-f)^e
    SseRR(a, 0x57, 3, 3, false);
    SseRR(a, kComiss, 2, 3, false);
    uint8_t* we = a.jccForward(0x6);                                  // f <= 0: W = e
    EmitSseRip(a, kMovss, 0, NearOne());
    SseRR(a, kSubss, 0, 1);
    SseRR(a, kDivss, 0, 2);                                           // W = (1-(1-f)^e)/f
    EmitApplyWeight(a, base, yawHere, exit);
    a.bind(we);
    EmitSseRip(a, kMovss, 0, E());
    EmitApplyWeight(a, base, yawHere, exit);
}

// v *= 1 - 0.5*d (xmm0 = 0.5*d on entry; site: movaps xmm1, 1.0; subss xmm1, xmm0; mulss
// xmm1, [v]) -> v *= (1 - 0.5*d/e)^e.
void Decay(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        EmitSseRip(a, kMulss, 0, InvE());
        a.copy(site, 8);
        EmitFloatCall(a, &PowE, 1, 1);
        a.copy(site + 8, 8);
        a.jmp(site + 16);
    });
}

// Linear phase continues while timer - d > s - d (s = d/e) instead of > 0, so it lasts the
// same 60 Hz time as at 60 FPS. `reg` is the timer register, `cmpLen` the original comiss.
void LinEnd(const SigSite& s, uint8_t reg, size_t cmpLen) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        Movaps(a, 3, 0);
        EmitSseRip(a, kMulss, 3, InvE());
        SseRR(a, kSubss, 3, 0);
        SseRR(a, kComiss, reg, 3, false);
        a.copy(site + cmpLen, s.len - cmpLen);
        a.jmp(site + s.len);
    });
}

// xmm0 = X, the 60 Hz fraction the camera closer applies this frame: the raw transition
// blend fraction [base+0x698] while a blend runs ([base+0x224]), else min(d*[base+0x210]/e, 1).
// `save`/`restore` keep live registers across the GetDelta call (rsp must end 16-aligned).
template <class Save, class Restore>
void EmitCloserFraction(Asm& a, uint8_t base, uint8_t frame, Save save, Restore restore) {
    SseMem(a, kMovss, 0, base, 0x698);
    EmitSseRip(a, kMaxss, 0, g_zero);
    EmitSseRip(a, kMinss, 0, NearOne());
    a.raw({0x83, (uint8_t)(0xB8 | base)}).i32(0x224).raw({0x00});    // cmp dword [base+0x224], 0
    uint8_t* have = a.jccForward(0x5);
    save(a);
    a.raw({0x48, 0x83, 0xEC, frame});
    CallGetDelta(a);
    a.raw({0x48, 0x83, 0xC4, frame});
    restore(a);
    SseMem(a, kMulss, 0, base, 0x210);
    EmitSseRip(a, kMulss, 0, InvE());
    EmitSseRip(a, kMinss, 0, NearOne());
    a.bind(have);
}

// A gain g applied to the error the closer then reduces by X per 60 Hz frame composes to
// g*X; the mod's closer applies LerpE(X), so use g' = LerpE(g*X)/LerpE(X).
void GainMul(const SigSite& s, uint8_t base, bool pushRax) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        const void* k = RipTarget(site, 4, 8);                        // mulss xmm1, [g]
        CmpE1(a);
        uint8_t* sPath = a.jccForward(0x5);
        EmitSseRip(a, kMulss, 1, k);
        a.jmp(site + 8);
        a.bind(sPath);
        Movaps(a, 5, 0);
        EmitCloserFraction(a, base, (uint8_t)(pushRax ? 0x28 : 0x20),
                           [&](Asm& x) { if (pushRax) x.raw({0x50}); }, [&](Asm& x) { if (pushRax) x.raw({0x58}); });
        Movaps(a, 3, 0);
        EmitFloatCall(a, &LerpE, 0, 0);
        Movaps(a, 2, 3);
        EmitSseRip(a, kMulss, 2, k);
        EmitFloatCall(a, &LerpE, 2, 2);
        EmitSseRip(a, kComiss, 0, g_zero);
        uint8_t* keep = a.jccForward(0x6);
        SseRR(a, kDivss, 2, 0);
        SseRR(a, kMulss, 1, 2);
        Movaps(a, 0, 5);
        a.jmp(site + 8);
        a.bind(keep);
        EmitSseRip(a, kMulss, 1, k);
        Movaps(a, 0, 5);
        a.jmp(site + 8);
    });
}

void FixGains() {
    GainMul(kCamF_CAM_GAIN_OFS_Y, kRsi, true);
    GainMul(kCamF_CAM_GAIN_M29_Y, kRbx, false);
    // Side-view Y lead: target - cur = (1+g)(raw - cur) -> g' = LerpE((1+g)X)/LerpE(X) - 1.
    if (uint8_t* site = SiteOf(kCamF_CAM_GAIN_SIDE_Y)) {
        uint8_t* vscale = RipTarget(site, 1, 5);
        InstallCaveUnchecked(kCamF_CAM_GAIN_SIDE_Y.name, site, 5, true, [&](Asm& a) {
            CmpE1(a);
            uint8_t* t1 = a.jccForward(0x4);
            EmitCloserFraction(a, kRdi, 0x28, [](Asm& x) { x.raw({0x51, 0x52}); }, [](Asm& x) { x.raw({0x5A, 0x59}); });
            Movaps(a, 3, 0);
            EmitFloatCall(a, &LerpE, 0, 0, true);
            Movaps(a, 1, 2);
            EmitSseRip(a, kAddss, 1, NearOne());
            SseRR(a, kMulss, 1, 3);
            EmitFloatCall(a, &LerpE, 1, 1, true);
            EmitSseRip(a, kComiss, 0, g_zero);
            uint8_t* t2 = a.jccForward(0x6);
            SseRR(a, kDivss, 1, 0);
            EmitSseRip(a, kSubss, 1, NearOne());
            Movaps(a, 2, 1);
            a.bind(t1);
            a.bind(t2);
            a.jmp(vscale);
        });
    }
    // Side-view distance factor gamma (xmm9) stored as an argument: gamma' = LerpE(gamma X)/LerpE(X).
    PatchSite(kCamF_CAM_GAIN_SIDE_DIST, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* sPath = a.jccForward(0x5);
        a.copy(site, 7);
        a.jmp(site + 7);
        a.bind(sPath);
        Movaps(a, 5, 0);
        EmitCloserFraction(a, kRdi, 0x28, [](Asm& x) { x.raw({0x41, 0x50}); }, [](Asm& x) { x.raw({0x41, 0x58}); });
        Movaps(a, 3, 0);
        EmitFloatCall(a, &LerpE, 0, 0);
        Movaps(a, 1, 9);
        SseRR(a, kMulss, 1, 3);
        EmitFloatCall(a, &LerpE, 1, 1);
        Movaps(a, 2, 9);
        EmitSseRip(a, kComiss, 0, g_zero);
        uint8_t* st = a.jccForward(0x6);
        SseRR(a, kDivss, 1, 0);
        Movaps(a, 2, 1);
        a.bind(st);
        a.raw({0xF3, 0x0F, 0x11, 0x54, 0x24, 0x30});                 // movss [rsp+0x30], xmm2
        Movaps(a, 0, 5);
        a.jmp(site + 7);
    });
    // Mode M0E roll: Approach(cur, diff, f, m) -> f' = LerpE(fX)/LerpE(X), m' = m*e*X/LerpE(X).
    if (uint8_t* site = SiteOf(kCamF_CAM_GAIN_M0E_ROLL)) {
        uint8_t* approach = RipTarget(site, 1, 5);
        InstallCaveUnchecked(kCamF_CAM_GAIN_M0E_ROLL.name, site, 5, true, [&](Asm& a) {
            CmpE1(a);
            uint8_t* t1 = a.jccForward(0x4);
            Movaps(a, 4, 0);
            Movaps(a, 5, 1);
            EmitCloserFraction(a, kRdi, 0x28, [](Asm&) {}, [](Asm&) {});
            Movaps(a, 1, 0);
            EmitFloatCall(a, &LerpE, 0, 0, true);
            EmitSseRip(a, kComiss, 0, g_zero);
            uint8_t* rst = a.jccForward(0x6);
            EmitSseRip(a, kMulss, 3, E());
            SseRR(a, kMulss, 3, 1);
            SseRR(a, kDivss, 3, 0);
            SseRR(a, kMulss, 1, 2);
            EmitFloatCall(a, &LerpE, 1, 1, true);
            SseRR(a, kDivss, 1, 0);
            Movaps(a, 2, 1);
            Movaps(a, 0, 4);
            Movaps(a, 1, 5);
            a.jmp(approach);
            a.bind(rst);
            Movaps(a, 0, 4);
            Movaps(a, 1, 5);
            a.bind(t1);
            a.jmp(approach);
        });
    }
}

void FixModes() {
    // M2A +0x3B0: += d*(500 - x)*k -> LerpE(min(k*d/e, 1))*(500 - x) (xmm0 = d, xmm1 = 500 - x, xmm7 = k).
    PatchSite(kCamF_CAM_M2A_3B0_EXACT, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* sPath = a.jccForward(0x5);
        a.copy(site, 8);
        a.jmp(site + 8);
        a.bind(sPath);
        SseRR(a, kMulss, 0, 7);
        ExactFrac(a, 0);
        SseRR(a, kMulss, 0, 1);
        a.jmp(site + 8);
    });
    // M2A +0x3E4: += d*(t - x)*k (xmm0 = t - x, xmm3 = d; the result in xmm3 is also an argument).
    PatchSite(kCamF_CAM_M2A_3E4_EXACT, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* sPath = a.jccForward(0x5);
        a.copy(site, 12);
        a.jmp(site + 12);
        a.bind(sPath);
        Movaps(a, 1, 3);
        SseMem(a, kMulss, 1, kRbx, 0x39C);
        ExactFrac(a, 1);
        SseRR(a, kMulss, 0, 1);
        Movaps(a, 3, 0);
        a.jmp(site + 12);
    });
    // M29 +0x3B0: += d*min(0.01*(22 - x), 0.01) -> min(LerpE(min(0.01*d/e, 1))*(22 - x), 0.01*d).
    if (uint8_t* at = SiteOf(kCamF_CAM_M29_3B0_EXACT)) {
        const void* handle = RipTarget(at + 4, 3, 7);
        const float* k001 = ConstFloat(0.01f);
        const float* k22 = ConstFloat(22.0f);
        PatchSite(kCamF_CAM_M29_3B0_EXACT, [&](Asm& a, uint8_t* site) {
            CmpE1(a);
            uint8_t* sPath = a.jccForward(0x5);
            SseRR(a, kMulss, 0, 6);
            a.raw({0x48, 0x8D, 0x0D}).rip(handle);                    // lea rcx, [handle]
            a.jmp(site + 11);
            a.bind(sPath);
            Movaps(a, 1, 0);
            EmitSseRip(a, kMulss, 1, k001);
            ExactFrac(a, 1);
            EmitSseRip(a, kMovss, 2, k22);
            SseMem(a, kSubss, 2, kRbx, 0x3B0);
            SseRR(a, kMulss, 1, 2);
            EmitSseRip(a, kMulss, 0, k001);
            SseRR(a, kMinss, 0, 1);
            a.raw({0x48, 0x8D, 0x0D}).rip(handle);
            a.jmp(site + 11);
        });
    }
    // M18 stores d * follow factor back every frame -> multiply by (d/e)^e instead.
    PatchSite(kCamF_CAM_M18_39C_STATE, [](Asm& a, uint8_t* site) {
        EmitSseRip(a, kMulss, 0, InvE());
        EmitFloatCall(a, &PowE, 0, 0);
        a.copy(site, 8);
        a.jmp(site + 8);
    });
    // Default auto-yaw: the d*k part of the fraction -> k*d/e (the Approach call then applies LerpE).
    PatchSite(kCamF_CAM_DEF_YAWAUTO_FRAC, [](Asm& a, uint8_t* site) {
        a.copy(site, 8);
        EmitSseRip(a, kMulss, 6, InvE());
        a.jmp(site + 8);
    });
    InstallTable(kCamFArgCalls, _countof(kCamFArgCalls));
}

}  // namespace

// The angular-impulse sites (existing SigSites from camera_patches.tsv).
void FixCameraImpulses(const SigSite& plc1, const SigSite& plc2P, const SigSite& plc2Y, const SigSite& plc3P,
                       const SigSite& plc3Y) {
    if (!g_half) {
        g_half = ConstFloat(0.5f);
        g_zero = ConstFloat(0.0f);
    }
    PatchSite(plc1, [](Asm& a, uint8_t* site) { EmitAngVel(a, site, 32, kRbx, false, false); });
    PatchSite(plc2P, [](Asm& a, uint8_t* site) { EmitAngVel(a, site, 16, kRcx, true, true); });
    PatchSite(plc3P, [](Asm& a, uint8_t* site) { EmitAngVel(a, site, 16, kRcx, true, true); });
    for (const SigSite* y : {&plc2Y, &plc3Y}) {                      // yaw already integrated: reload it
        PatchSite(*y, [](Asm& a, uint8_t* site) {
            a.copy(site, 8);
            a.jmp(site + 16);
        });
    }
    for (const SigSite* s : {&kCamF_CAM_PLC1_DECAY_P, &kCamF_CAM_PLC1_DECAY_Y, &kCamF_CAM_PLC2_DECAY_P,
                             &kCamF_CAM_PLC2_DECAY_Y, &kCamF_CAM_PLC3_DECAY_P, &kCamF_CAM_PLC3_DECAY_Y})
        Decay(*s);
    LinEnd(kCamF_CAM_PLC1_LINEND, 2, 4);
    LinEnd(kCamF_CAM_PLC2_LINEND, 1, 4);
    LinEnd(kCamF_CAM_PLC3_LINEND, 1, 3);
}

void FixCameraFinal() {
    if (!g_half) {
        g_half = ConstFloat(0.5f);
        g_zero = ConstFloat(0.0f);
    }
    FixGains();
    FixModes();
}
