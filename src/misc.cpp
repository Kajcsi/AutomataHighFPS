// Remaining exactness fixes outside camera and enemies: smoothing the game scales by the
// frame delta only linearly (k*d), velocity decays paired with a linear position step
// (both made exact together, so the time profile and the total travel match 60 FPS),
// Pod B laser hit timers, the ramped pod lerp, the 9S thrown-weapon spin, the A2 evade
// turn, ExBezierPath, the fish and fishing bobber, effect collision intervals and the
// shared knockback slide. Every cave runs the original instructions at e = 1.
// Sites come from analysis/misc_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/misc.inc"

namespace {

const float* E() { return &S->stepUpToOne; }
const uint8_t kDivss = 0x5E;
// Base register numbers for SseMem.
const uint8_t kRcx = 1, kRbx = 3, kRsi = 6, kRdi = 7;

// `<op>ss xD, xS` (f3) or `<op>ps xD, xS` (movaps 0x28, xorps 0x57, comiss 0x2F).
void SseRR(Asm& a, uint8_t op, uint8_t d, uint8_t s, bool f3 = true) {
    if (f3) a.raw({0xF3});
    uint8_t rex = (uint8_t)(0x40 | (d >= 8 ? 4 : 0) | (s >= 8 ? 1 : 0));
    if (rex != 0x40) a.raw({rex});
    a.raw({0x0F, op, (uint8_t)(0xC0 | (d & 7) << 3 | (s & 7))});
}
void Movaps(Asm& a, uint8_t d, uint8_t s) { SseRR(a, 0x28, d, s, false); }
// `<op>ss xD, [base + disp32]` (base not rsp/r12).
void SseMem(Asm& a, uint8_t op, uint8_t d, uint8_t base, int32_t disp) {
    a.raw({0xF3});
    if (d >= 8) a.raw({0x44});
    a.raw({0x0F, op, (uint8_t)(0x80 | (d & 7) << 3 | base)}).i32(disp);
}
void CmpE1(Asm& a) { a.raw({0x81, 0x3D}).rip(E(), 4).i32(0x3F800000); }  // cmp dword [e], 1.0
void CmpTicks0(Asm& a) { a.raw({0x83, 0x3D}).rip(&S->ticks60, 1).raw({0x00}); }  // cmp dword [ticks60], 0
// xR holds k*d: xR = 1-(1-min(k*d/e, 1))^e.
void ExactFrac(Asm& a, uint8_t r, bool viaCall = false) {
    EmitSseRip(a, kMulss, r, InvE());
    EmitSseRip(a, kMinss, r, NearOne());
    EmitFloatCall(a, &LerpE, r, r, viaCall);
}
bool Bytes(const uint8_t* p, std::initializer_list<uint8_t> b) { return memcmp(p, b.begin(), b.size()) == 0; }

// `mulss xA, [rip k]; mulss xA, <d>` (k*d lerp/decay factor on xA) with F computed in xF:
// at e < 1 xA *= 1-(1-min(k*d/e,1))^e. `emitD` emits `mulss xF, <d>`.
template <class D>
void ExactMulKD(const SigSite& s, uint8_t a_, uint8_t f, D emitD, bool fInA = false) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        const void* k = RipTarget(site, 4, 8);
        CmpE1(a);
        uint8_t* v = a.jccForward(0x4);
        EmitSseRip(a, kMovss, f, k);
        emitD(a, f);
        ExactFrac(a, f);
        if (fInA) SseRR(a, kMulss, a_, 1);  // F is in xA already: xA = F * x1
        else SseRR(a, kMulss, a_, f);
        a.jmp(site + s.len);
        a.bind(v);
        EmitSseRip(a, kMulss, a_, k);
        a.copy(site + 8, s.len - 8);
        a.jmp(site + s.len);
    });
}

void FixPodLaser() {
    // Main beam: the hit list clears when [rdi+0xCF8] (-= delta, reset 6.0) runs out. Count it
    // on 60 Hz ticks by the tick's delta (d/e), so it clears at the 60 FPS instants.
    if (uint8_t* site = SiteOf(kMf_POD_LaserBeamHitTimerTick)) {
        if (!Bytes(site + 20, {0x41, 0x0F, 0x2F, 0xC0, 0x0F, 0x87})) {
            Log("  [skip] %s: unexpected bytes after the site", kMf_POD_LaserBeamHitTimerTick.name);
        } else {
            uint8_t* skipTo = RipTarget(site + 24, 2, 6);  // target of the `ja` (timer still running)
            InstallCaveUnchecked(kMf_POD_LaserBeamHitTimerTick.name, site, 20, false, [&](Asm& a) {
                CmpTicks0(a);
                uint8_t* skip = a.jccForward(0x4);
                EmitSseRip(a, kDivss, 1, E());  // xmm1 = this tick's delta
                a.copy(site, 20);
                a.jmp(site + 20);
                a.bind(skip);
                a.jmp(skipTo);
            });
        }
    }
    // Lv4 reflected segments: `call seconds()` (GetDelta/60 of the pod) -> 0 on frames without
    // a tick, else the tick's seconds rebuilt bit-exactly as (d/e)*(1/60).
    if (uint8_t* site = SiteOf(kMf_POD_LaserSegHitTimerTick)) {
        uint8_t* callee = site[0] == 0xE8 ? RipTarget(site, 1, 5) : nullptr;
        if (!callee || !Bytes(callee + 0x10, {0x48, 0x8D, 0x05}) || callee[0x19] != 0xE8 || callee[0x21] != 0xE8 ||
            !Bytes(callee + 0x26, {0xF3, 0x0F, 0x59, 0x05})) {
            Log("  [skip] %s: unexpected callee", kMf_POD_LaserSegHitTimerTick.name);
        } else {
            const void* global = RipTarget(callee + 0x10, 3, 7);
            const void* nodeOf = RipTarget(callee + 0x19, 1, 5);
            const void* getDelta = RipTarget(callee + 0x21, 1, 5);
            const void* sixtieth = RipTarget(callee + 0x26, 4, 8);
            InstallCaveUnchecked(kMf_POD_LaserSegHitTimerTick.name, site, 5, true, [&](Asm& a) {
                CmpTicks0(a);
                uint8_t* run = a.jccForward(0x5);
                a.raw({0x0F, 0x57, 0xC0, 0xC3});                    // xorps xmm0, xmm0; ret
                a.bind(run);
                CmpE1(a);
                uint8_t* orig = a.jccForward(0x4);
                a.raw({0x48, 0x83, 0xEC, 0x28});                    // sub rsp, 0x28
                a.raw({0x48, 0x8B, 0x89, 0x10, 0x06, 0x00, 0x00});  // mov rcx, [rcx+0x610]
                a.raw({0x48, 0x85, 0xC9, 0x75, 0x09});              // test rcx, rcx; jne h
                a.raw({0x48, 0x8D, 0x05}).rip(global);              // lea rax, [global]
                a.raw({0xEB, 0x05});                                // jmp g
                a.call(nodeOf);                                     // h: call node-of
                a.raw({0x48, 0x8B, 0xC8});                          // g: mov rcx, rax
                a.call(getDelta);
                a.raw({0x48, 0x83, 0xC4, 0x28});                    // add rsp, 0x28
                EmitSseRip(a, kDivss, 0, E());
                EmitSseRip(a, kMulss, 0, sixtieth);
                a.raw({0xC3});                                      // ret
                a.bind(orig);
                a.jmp(callee);
            });
        }
    }
}

void FixPodRamp() {
    // step = (target - pos) * LerpE(0.1) [Pod_Lerp_rate] * (1 - r) [this call, xmm2 = 1 - r]:
    // make the product 1-(1-0.1*(1-r))^e, with r sampled half a 60 Hz tick ahead.
    uint8_t* site = SiteOf(kMf_POD_LerpRate_RampExact);
    if (!site) return;
    if (site[0] != 0xE8 || !Bytes(site - 0x14, {0xF3, 0x0F, 0x5E, 0x05})) {
        Log("  [skip] %s: unexpected bytes", kMf_POD_LerpRate_RampExact.name);
        return;
    }
    uint8_t* callee = RipTarget(site, 1, 5);
    const void* ramp = RipTarget(site - 0x14, 4, 8);  // the ramp length G (seconds)
    const float* k120 = ConstFloat(1.0f / 120.0f);
    const float* k01 = ConstFloat(0.1f);
    const float* lerp01 = NewScaled("pod lerp ramp 0.1", Scale::Lerp, 0.1f);  // the same value Pod_Lerp_rate uses
    InstallCaveUnchecked(kMf_POD_LerpRate_RampExact.name, site, 5, true, [&](Asm& a) {
        CmpE1(a);
        uint8_t* tail = a.jccForward(0x4);
        EmitSseRip(a, kMovss, 1, NearOne());
        EmitSseRip(a, kSubss, 1, E());
        EmitSseRip(a, kMulss, 1, k120);
        EmitSseRip(a, kDivss, 1, ramp);
        SseRR(a, kAddss, 2, 1);
        EmitSseRip(a, kMinss, 2, NearOne());
        EmitSseRip(a, kMulss, 2, k01);
        EmitFloatCall(a, &LerpE, 2, 2, true);
        EmitSseRip(a, kDivss, 2, lerp01);
        a.bind(tail);
        a.jmp(callee);
    });
}

void FixPlayer() {
    auto dPlayer = [](Asm& a, uint8_t f) { SseMem(a, kMulss, f, kRdi, 0x1470); };  // mulss xF, [rdi+0x1470]
    ExactMulKD(kMf_PL_17640_LerpExact, 0, 1, dPlayer);
    ExactMulKD(kMf_PL_17640_DecayExact, 0, 0, dPlayer, true);
    ExactArgCall(kMf_PL_BuddyPushOut_Exact, 0.05f);
    ExactArgCall(kMf_PL_16AF0_LerpExact, 0.05f);
    // Carried entity local y: (10 - y) * xmm9 (0.1) * d.
    PatchSite(kMf_PL_CarryLocalY_LerpExact, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(0x4);
        Movaps(a, 2, 9);
        SseMem(a, kMulss, 2, kRdi, 0x1470);
        ExactFrac(a, 2);
        SseRR(a, kMulss, 1, 2);
        a.jmp(site + 13);
        a.bind(v);
        a.copy(site, 13);
        a.jmp(site + 13);
    });

    // 2D-plane offset: off-plane component x *= 1 - 0.025d, then x += v*d with v *= 1 - 0.03d
    // (velocity [rdi+0x16C20..28]). Exact over a frame: x <- a^e x + v (Fb-Fa)/(kb-ka); the
    // integration row adds v Fb/kb, so this cave subtracts the difference.
    uint8_t* decaySite = SiteOf(kMf_PL_16C20_DecayExact);
    if (decaySite && Bytes(decaySite - 0x16, {0xF3, 0x0F, 0x59, 0x05})) {
        const float* kb = (const float*)RipTarget(decaySite - 0x16, 4, 8);  // the game's 0.03
        struct { const SigSite* s; int32_t v; } axes[] = {{&kMf_PL_2DOffsetX_DecayExact, 0x16C20},
                                                         {&kMf_PL_2DOffsetY_DecayExact, 0x16C24},
                                                         {&kMf_PL_2DOffsetZ_DecayExact, 0x16C28}};
        for (auto& ax : axes) {
            uint8_t* at = SiteOf(*ax.s);
            if (!at) continue;
            const float* ka = (const float*)RipTarget(at, 4, 8);  // the game's 0.025
            const float* dk = ConstFloat(*kb - *ka);
            PatchSite(*ax.s, [&](Asm& a, uint8_t* site) {
                CmpE1(a);
                uint8_t* v = a.jccForward(0x4);
                EmitSseRip(a, kMovss, 0, ka);
                SseRR(a, kMulss, 0, 2);
                ExactFrac(a, 0);                                    // Fa
                EmitSseRip(a, kMovss, 3, kb);
                SseRR(a, kMulss, 3, 2);
                ExactFrac(a, 3);                                    // Fb
                Movaps(a, 4, 3);
                SseRR(a, kSubss, 4, 0);
                EmitSseRip(a, kDivss, 4, dk);
                EmitSseRip(a, kDivss, 3, kb);
                SseRR(a, kSubss, 4, 3);
                SseMem(a, kMulss, 4, kRdi, ax.v);
                SseRR(a, kMulss, 0, 1);
                SseRR(a, kSubss, 0, 4);
                a.jmp(site + 12);
                a.bind(v);
                EmitSseRip(a, kMulss, 0, ka);
                a.copy(site + 8, 4);
                a.jmp(site + 12);
            });
        }
    } else {
        Log("  [skip] 2D offset decays: velocity decay site not found");
    }
    ExactArgCall(kMf_PL_16C20_IntegrateExact, 0.03f);
    if (uint8_t* site = SiteOf(kMf_PL_16C20_DecayExact)) {
        uint8_t* callee = RipTarget(site, 1, 5);
        InstallCaveUnchecked(kMf_PL_16C20_DecayExact.name, site, 5, true, [&](Asm& a) {
            CmpE1(a);
            uint8_t* tail = a.jccForward(0x4);
            ExactFrac(a, 0, true);                                  // xmm0 = 0.03*d on entry
            EmitSseRip(a, kMovss, 2, NearOne());
            SseRR(a, kSubss, 2, 0);
            a.bind(tail);
            a.jmp(callee);
        });
    }

    // A2 evade (replaces the old A2_EvadeTurnLerp cave): blend b += (0.1 - b)*0.1 per frame,
    // then turn rate += (T - w)*b*d. Blend exact as before; the rate's k*d fraction exact, with
    // b sampled half a 60 Hz tick ahead on its own path.
    const float* lerp01 = NewScaled("A2 evade turn", Scale::Lerp, 0.1f);
    const float* half = ConstFloat(0.9486833f);
    const float* halfPow = NewScaled("A2 evade blend lead", Scale::Pow, 0.9486833f);
    PatchSite(kMf_A2_EvadeTurnLerp, [&](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(0x4);
        EmitSseRip(a, kMulss, 2, lerp01);
        SseRR(a, kAddss, 2, 1);
        a.copy(site + 12, 8);                                       // movss [rbx+0x1A88], xmm2
        Movaps(a, 5, 7);
        SseRR(a, kSubss, 5, 2);
        EmitSseRip(a, kMulss, 5, half);
        EmitSseRip(a, kDivss, 5, halfPow);
        Movaps(a, 1, 7);
        SseRR(a, kSubss, 1, 5);
        SseMem(a, kMulss, 1, kRbx, 0x1470);
        ExactFrac(a, 1);
        SseRR(a, kMulss, 4, 1);
        a.jmp(site + 28);
        a.bind(v);
        a.copy(site, 28);
        a.jmp(site + 28);
    });
}

void FixMiscObjects() {
    // 9S thrown weapon: rate v += (t - v)*0.1*d, then angle += d*v (rate first).
    PatchSite(kMf_W9S_SpinRateExact, [](Asm& a, uint8_t* site) {
        const void* k = RipTarget(site + 4, 4, 8);
        CmpE1(a);
        uint8_t* v = a.jccForward(0x4);
        EmitSseRip(a, kMovss, 2, k);
        SseRR(a, kMulss, 2, 0);
        EmitSseRip(a, kMulss, 2, InvE());
        EmitSseRip(a, kMinss, 2, NearOne());
        Movaps(a, 3, 2);                                            // a = 0.1*d/e
        EmitFloatCall(a, &LerpE, 2, 2);                             // F
        Movaps(a, 4, 6);
        SseRR(a, kSubss, 4, 1);                                     // t - v
        Movaps(a, 5, 4);
        SseRR(a, kMulss, 5, 2);
        SseRR(a, kAddss, 5, 1);                                     // v'
        SseRR(a, kMulss, 0, 6);                                     // d*t
        SseRR(a, kMulss, 4, 2);
        EmitSseRip(a, kMovss, 2, NearOne());
        SseRR(a, kSubss, 2, 3);                                     // q = 1 - a
        SseRR(a, kMulss, 4, 2);
        EmitSseRip(a, kDivss, 4, k);
        SseRR(a, kSubss, 0, 4);                                     // angle step
        a.raw({0xF3, 0x0F, 0x11, 0xAB}).i32(0x3B30);               // movss [rbx+0x3B30], xmm5
        Movaps(a, 6, 5);
        a.jmp(site + 32);
        a.bind(v);
        a.copy(site, 4);
        EmitSseRip(a, kMulss, 6, k);
        a.copy(site + 12, 20);
        a.jmp(site + 32);
    });
    // ExBezierPath slots 6-8: [rbx+0x134] += (x - [0x134])*0.1*d, d in xmm7 / xmm8.
    ExactMulKD(kMf_EXBZ_Slot6_LerpExact, 0, 2, [](Asm& a, uint8_t f) { SseRR(a, kMulss, f, 7); });
    ExactMulKD(kMf_EXBZ_Slot7_LerpExact, 0, 2, [](Asm& a, uint8_t f) { SseRR(a, kMulss, f, 8); });
    ExactMulKD(kMf_EXBZ_Slot8_LerpExact, 0, 2, [](Asm& a, uint8_t f) { SseRR(a, kMulss, f, 8); });

    // Fish: pos += vel*d before vel *= 1 - 0.05d, with vel re-derived from the displacement
    // times m = xmm6 in between.
    ExactArgCall(kMf_FISH_850_IntegrateExact, 0.05f);
    if (uint8_t* site = SiteOf(kMf_FISH_850_DecayExact)) {
        if (site[0] != 0xE8 || !Bytes(site - 0x1E, {0xF3, 0x0F, 0x59, 0x05})) {
            Log("  [skip] %s: unexpected bytes", kMf_FISH_850_DecayExact.name);
        } else {
            uint8_t* callee = RipTarget(site, 1, 5);
            const void* k05 = RipTarget(site - 0x1E, 4, 8);
            InstallCaveUnchecked(kMf_FISH_850_DecayExact.name, site, 5, true, [&](Asm& a) {
                CmpE1(a);
                uint8_t* tail = a.jccForward(0x4);
                ExactFrac(a, 0, true);                              // F (xmm0 = 0.05*d on entry)
                EmitSseRip(a, kMovss, 2, NearOne());
                SseRR(a, kSubss, 2, 0);
                EmitSseRip(a, kMulss, 2, k05);
                SseRR(a, kMulss, 0, 6);
                SseRR(a, 0x57, 3, 3, false);                        // xorps xmm3, xmm3
                SseRR(a, 0x2F, 0, 3, false);                        // comiss xmm0, xmm3
                uint8_t* one = a.jccForward(0x6);                   // jbe one
                SseRR(a, kDivss, 2, 0);
                a.bind(tail);
                a.jmp(callee);
                a.bind(one);
                EmitSseRip(a, kMovss, 2, NearOne());
                a.jmp(callee);
            });
        }
    }
    // Fishing bobber: one random jitter step per 60 Hz tick.
    if (uint8_t* site = SiteOf(kMf_FISH_BobberJitterTick)) {
        uint8_t* resume = site + 0x8F;  // `call` that starts the timer update after the jitter block
        if (resume[0] != 0xE8) {
            Log("  [skip] %s: unexpected bytes at the resume point", kMf_FISH_BobberJitterTick.name);
        } else {
            const void* rng = RipTarget(site, 2, 10);
            InstallCaveUnchecked(kMf_FISH_BobberJitterTick.name, site, 10, false, [&](Asm& a) {
                CmpTicks0(a);
                uint8_t* run = a.jccForward(0x5);
                a.raw({0x48, 0x8B, 0xCF});                          // mov rcx, rdi (set by the skipped block)
                a.jmp(resume);
                a.bind(run);
                a.raw({0x69, 0x05}).rip(rng, 4).i32(0x19660D);      // imul eax, [rng], 0x19660D
                a.jmp(site + 10);
            });
        }
    }
    // Effect sparks: collision check every 4 frames -> every 4 60 Hz ticks.
    RewriteSite(kMf_ESP10_CollideIntervalTicks, {{0, 8, kSubss, 1, &S->ticks60f}});
    RewriteSite(kMf_ESP32_CollideIntervalTicks, {{0, 8, kSubss, 1, &S->ticks60f}});

    // Shared knockback slide: pos += dir*speed*d, then speed += (speed*m - speed)*d.
    if (uint8_t* site = SiteOf(kMf_KB_SlideIntegrateExact)) {
        uint8_t* callee = RipTarget(site, 1, 5);
        InstallCaveUnchecked(kMf_KB_SlideIntegrateExact.name, site, 5, true, [&](Asm& a) {
            CmpE1(a);
            uint8_t* tail1 = a.jccForward(0x4);
            EmitSseRip(a, kMovss, 1, NearOne());
            SseMem(a, kSubss, 1, kRbx, 0xA84);                      // k = 1 - m
            SseRR(a, 0x57, 0, 0, false);                            // xorps xmm0, xmm0
            SseRR(a, 0x2F, 1, 0, false);                            // comiss xmm1, xmm0
            uint8_t* tail2 = a.jccForward(0x6);                     // jbe tail
            Movaps(a, 0, 1);
            SseRR(a, kMulss, 0, 2);
            ExactFrac(a, 0, true);
            SseRR(a, kDivss, 0, 1);
            Movaps(a, 2, 0);                                        // W = F/k
            a.bind(tail1);
            a.bind(tail2);
            a.jmp(callee);
        });
    }
    PatchSite(kMf_KB_SlideDecayExact, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v1 = a.jccForward(0x4);
        EmitSseRip(a, kMovss, 2, NearOne());
        SseMem(a, kSubss, 2, kRbx, 0xA84);
        SseRR(a, 0x57, 3, 3, false);                                // xorps xmm3, xmm3
        SseRR(a, 0x2F, 2, 3, false);                                // comiss xmm2, xmm3
        uint8_t* v2 = a.jccForward(0x6);                            // jbe original
        SseRR(a, kMulss, 2, 6);
        ExactFrac(a, 2);
        SseRR(a, kMulss, 2, 0);
        Movaps(a, 1, 0);
        SseRR(a, kSubss, 1, 2);                                     // speed*(1-k*d/e)^e
        a.jmp(site + 8);
        a.bind(v1);
        a.bind(v2);
        a.copy(site, 8);
        a.jmp(site + 8);
    });
}

}  // namespace

void FixRemainingExact() {
    Log("remaining exact fixes:");
    FixPodLaser();
    FixPodRamp();
    FixPlayer();
    FixMiscObjects();
}
