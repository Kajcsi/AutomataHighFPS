// Enemies and bullets, exhaustive pass: bullet arcs integrated exactly (gravity applied
// before the move, the arc speed-up, the ring bomb), bullet spin and speed decay paired
// with the move, homing and frame turns of the remaining move modes, and per-enemy
// smoothing, decays, hover pulls, steps, ramps and frame counters found across the
// Em/Ba classes. Every cave runs the original instructions at e = 1.
// Sites come from analysis/enemy_final_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/enemy_final.inc"

namespace {

const float* E() { return &S->stepUpToOne; }

const uint8_t kR15 = 15, kR14 = 14;
const uint8_t kJe = 0x4, kJne = 0x5, kJae = 0x3, kJa = 0x7, kJbe = 0x6;

// Shared constants (allocated before any cave is written).
struct {
    const float *zero, *half, *mhalf, *lerp01, *lerp001, *lerp03, *lin001, *lin01, *pow01;
} K;

// Destination and source registers of a reg-reg or reg-mem SSE instruction `F3 [REX] 0F op ModRM`.
uint8_t RegOf(const uint8_t* insn) {
    bool rex = (insn[1] & 0xF0) == 0x40;
    return (uint8_t)((rex && (insn[1] & 4) ? 8 : 0) | (insn[rex ? 4 : 3] >> 3 & 7));
}
uint8_t RmOf(const uint8_t* insn) {
    bool rex = (insn[1] & 0xF0) == 0x40;
    return (uint8_t)((rex && (insn[1] & 1) ? 8 : 0) | (insn[rex ? 4 : 3] & 7));
}

// Re-emits the SSE instruction `insn` (F3 [REX] 0F op ModRM ..., not RIP-relative) with
// opcode `op` and register xmm<reg>, keeping its other operand.
void SseAs(Asm& a, const uint8_t* insn, size_t len, uint8_t op, uint8_t reg) {
    size_t i = 1;
    uint8_t rex = (insn[1] & 0xF0) == 0x40 ? insn[i++] : 0x40;
    rex = (uint8_t)((rex & ~4) | (reg >= 8 ? 4 : 0));
    a.raw({0xF3});
    if (rex != 0x40) a.raw({rex});
    a.raw({0x0F, op, (uint8_t)((insn[i + 2] & 0xC7) | (reg & 7) << 3)});
    a.copy(insn + i + 3, len - i - 3);
}

// `<op>ss xD, [base + disp32]` for any base register but rsp/r12.
void SseMemX(Asm& a, uint8_t op, uint8_t d, uint8_t base, int32_t disp) {
    uint8_t rex = (uint8_t)(0x40 | (d >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    a.raw({0xF3});
    if (rex != 0x40) a.raw({rex});
    a.raw({0x0F, op, (uint8_t)(0x80 | (d & 7) << 3 | (base & 7))}).i32(disp);
}

// `movss`/`movss store` with [rsp + disp8] (op 0x10 load, 0x11 store), d < 8.
void SseRsp(Asm& a, uint8_t op, uint8_t d, uint8_t disp8) { a.raw({0xF3, 0x0F, op, (uint8_t)(0x44 | d << 3), 0x24, disp8}); }

// `jmp rel32` to a label bound later with bind().
uint8_t* JmpForward(Asm& a) {
    a.raw({0xE9});
    uint8_t* rel = a.cur();
    a.i32(0);
    return rel;
}

void Xorps(Asm& a, uint8_t r) { SseRR(a, 0x57, r, r, false); }
void Comiss(Asm& a, uint8_t d, uint8_t s) { SseRR(a, 0x2F, d, s, false); }

// --- Delta-scaled smoothing x += (t - x)*k*d (a fraction linear in d) made exact:
// k*d -> 1-(1-min(k*d/e, 1))^e.

// `mulss xA, [rip k]; mulss xA, <d>`: xA *= the exact fraction, built in xF (< 8).
void ExactRipK(const SigSite& s, uint8_t f) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        size_t n = (site[1] & 0xF0) == 0x40 ? 9 : 8;
        uint8_t A = RegOf(site);
        const void* k = RipTarget(site, n - 4, n);
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        SseAs(a, site + n, s.len - n, kMovss, f);
        EmitSseRip(a, kMulss, f, k);
        ExactFrac(a, f);
        SseRR(a, kMulss, A, f);
        a.jmp(site + s.len);
        a.bind(v);
        EmitSseRip(a, kMulss, A, k);
        a.copy(site + n, s.len - n);
        a.jmp(site + s.len);
    });
}

// `mulss xA, xK; mulss xA, <d>`: same with k in a register.
void ExactRegK(const SigSite& s, uint8_t f) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        size_t n = (site[1] & 0xF0) == 0x40 ? 5 : 4;
        uint8_t A = RegOf(site), Kr = RmOf(site);
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        SseAs(a, site + n, s.len - n, kMovss, f);
        SseRR(a, kMulss, f, Kr);
        ExactFrac(a, f);
        SseRR(a, kMulss, A, f);
        a.jmp(site + s.len);
        a.bind(v);
        a.copy(site, s.len);
        a.jmp(site + s.len);
    });
}

// `mulss xT, [rip k]; mulss xmm0, xT` with xmm0 = d (GetDelta) and xT = t - x:
// xmm0 = the exact fraction * xT.
void ExactRipKD0(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        size_t n = (site[1] & 0xF0) == 0x40 ? 9 : 8;
        uint8_t T = RegOf(site);
        const void* k = RipTarget(site, n - 4, n);
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        EmitSseRip(a, kMulss, 0, k);
        ExactFrac(a, 0);
        SseRR(a, kMulss, 0, T);
        a.jmp(site + s.len);
        a.bind(v);
        EmitSseRip(a, kMulss, T, k);
        a.copy(site + n, s.len - n);
        a.jmp(site + s.len);
    });
}

// `mulss xA, xK` with the game multiplying by d = [base+disp] next: xA *= exact fraction / d.
void ExactRegKOverD(const SigSite& s, uint8_t f, uint8_t base, int32_t disp) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        uint8_t A = RegOf(site), Kr = RmOf(site);
        CmpE1(a);
        uint8_t* v1 = a.jccForward(kJe);
        SseMem(a, kMovss, f, base, disp);
        EmitSseRip(a, kComiss, f, K.zero);
        uint8_t* v2 = a.jccForward(kJbe);
        SseRR(a, kMulss, f, Kr);
        ExactFrac(a, f);
        SseMem(a, kDivss, f, base, disp);
        SseRR(a, kMulss, A, f);
        a.jmp(site + s.len);
        a.bind(v1);
        a.bind(v2);
        a.copy(site, s.len);
        a.jmp(site + s.len);
    });
}

// --- Decays v *= 1 - k*d (linear in d) made exact: (1 - k*d/e)^e.

// `subss xR, xK` (4 bytes, xR = 1, xK = k*d), then position-independent instructions.
void ExactDecay(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        uint8_t R = RegOf(site), Kr = RmOf(site);
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        EmitSseRip(a, kMulss, Kr, InvE());
        a.copy(site, 4);
        EmitFloatCall(a, &PowE, R, R);
        a.copy(site + 4, s.len - 4);
        a.jmp(site + s.len);
        a.bind(v);
        a.copy(site, s.len);
        a.jmp(site + s.len);
    });
}

// Hover pull vel = vel*(1 - k*d) + k*pull with k = xK (`movaps xmm2, xK`, then the rest of
// the site) and d = [base+0xC84]: with the decay made (1-c)^e (c = k*d/e), the exact pull
// weight is k*(1-(1-c)^e)/c (k*e when c <= 0).
void ExactPullWeight(const SigSite& s, uint8_t base) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        size_t n = site[0] == 0x41 ? 4 : 3;
        uint8_t Kr = (uint8_t)((site[0] == 0x41 ? 8 : 0) | (site[n - 1] & 7));
        a.copy(site, n);                                            // movaps xmm2, xK
        CmpE1(a);
        uint8_t* done1 = a.jccForward(kJe);
        SseMemX(a, kMovss, 0, base, 0xC84);
        SseRR(a, kMulss, 0, Kr);
        EmitSseRip(a, kMulss, 0, InvE());                           // c
        EmitSseRip(a, kComiss, 0, K.zero);
        uint8_t* zero = a.jccForward(kJbe);
        EmitSseRip(a, kMinss, 0, NearOne());
        SseRR(a, kDivss, 2, 0);
        EmitFloatCall(a, &LerpE, 0, 0);
        SseRR(a, kMulss, 2, 0);
        uint8_t* done2 = JmpForward(a);
        a.bind(zero);
        EmitSseRip(a, kMulss, 2, E());
        a.bind(done1);
        a.bind(done2);
        a.copy(site + n, s.len - n);
        a.jmp(site + s.len);
    });
}

// Semi-implicit gravity (vel.y -= g*d, then pos += M*vel*d) exact over a frame: pos gets
// -0.5*g*d*d*(1/e - 1) along M row 1 ([rdi+0xD0], pos at [rdi+0x50]). Site: the original
// position-independent instructions that follow, replayed after the correction.
void ExactPartGravity(const SigSite& s, int32_t dOff, int32_t gOff) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        SseMem(a, kMovss, 0, kRdi, dOff);
        SseRR(a, kMulss, 0, 0);
        SseMem(a, kMulss, 0, kRdi, gOff);
        EmitSseRip(a, kMovss, 1, InvE());
        EmitSseRip(a, kSubss, 1, NearOne());
        SseRR(a, kMulss, 0, 1);
        EmitSseRip(a, kMulss, 0, K.mhalf);
        for (int32_t off = 0; off < 12; off += 4) {
            SseMem(a, kMovss, 1, kRdi, 0xD0 + off);
            SseRR(a, kMulss, 1, 0);
            SseMem(a, kAddss, 1, kRdi, 0x50 + off);
            SseMem(a, 0x11, 1, kRdi, 0x50 + off);
        }
        a.bind(v);
        a.copy(site, s.len);
        a.jmp(site + s.len);
    });
}

// `inc dword [reg+disp32]` frame counter -> += ticks60.
void IncTicks(const SigSite& s) {
    if (uint8_t* site = SiteOf(s)) IncByTicks(s.name, site, s.orig[1]);
}

// --- Bullets.

void FixBulletMoves() {
    // Shared move: gravity hits vel.y before pos += M*vel*d*s, so steps of e 60 Hz frames
    // leave the arc g60*(1-e)/2 higher per frame. rdx (the integrated vector) is pointed at
    // M*vel*w + M.row1*c, c = -0.5*(1-e)*g60 with g60 the 60 Hz gravity of this update, and
    // w the exact travel weight of mode 0xA's x1.025 speed-up.
    const float* pow1025 = NewScaled("bullet arc speed-up 1.025", Scale::Pow, 1.025f);
    const float* k1025 = ConstFloat(1.025f);
    const float* k25 = ConstFloat(2.5f);
    const float* km0025 = ConstFloat(-0.025f);
    const float* k001 = ConstFloat(0.01f);
    const float* k0005 = ConstFloat(0.005f);
    PatchSite(kEf_EF_Blt_ExactGravityPos_5EDC16, [&](Asm& a, uint8_t* site) {
        a.copy(site + 13, 3);                                       // mov rdx, r15
        CmpE1(a);
        uint8_t* done = a.jccForward(kJe);
        Xorps(a, 0);                                                // g60
        EmitSseRip(a, kMovss, 3, NearOne());                        // w
        a.raw({0x8B, 0x86}).i32(0xC84);                             // mov eax, [rsi+0xC84] (move mode)
        a.raw({0x83, 0xF8, 0x0A});                                  // cmp eax, 0xA
        uint8_t* m47a = a.jccForward(kJne);
        a.raw({0x8B, 0x8E}).i32(0xDB0);                             // ecx = landing point x|y|z bits
        a.raw({0x0B, 0x8E}).i32(0xDB4);
        a.raw({0x0B, 0x8E}).i32(0xDB8);
        a.raw({0x01, 0xC9});                                        // add ecx, ecx (drop the signs)
        uint8_t* acc = a.jccForward(kJne);
        SseMem(a, kAddss, 0, kRsi, 0xE98);                          // arc gravity
        uint8_t* m47b = JmpForward(a);
        a.bind(acc);                                                // speed-up ran iff |vel| < 2.5*1.025^e now
        SseMem(a, kMovss, 1, kRsi, 0xE00);
        SseRR(a, kMulss, 1, 1);
        for (int32_t off : {0xE04, 0xE08}) {
            SseMem(a, kMovss, 2, kRsi, off);
            SseRR(a, kMulss, 2, 2);
            SseRR(a, kAddss, 1, 2);
        }
        EmitSseRip(a, kMovss, 2, pow1025);
        EmitSseRip(a, kMulss, 2, k25);
        SseRR(a, kMulss, 2, 2);
        Comiss(a, 1, 2);
        uint8_t* m47c = a.jccForward(kJae);
        EmitSseRip(a, kMovss, 1, k1025);                            // w = r^(1-e)*(1-r^e)/((1-r)*e)
        EmitSseRip(a, kDivss, 1, pow1025);
        EmitSseRip(a, kMovss, 2, NearOne());
        EmitSseRip(a, kSubss, 2, pow1025);
        SseRR(a, kMulss, 1, 2);
        EmitSseRip(a, kMovss, 2, km0025);
        EmitSseRip(a, kMulss, 2, E());
        SseRR(a, kDivss, 1, 2);
        Movaps(a, 3, 1);
        a.bind(m47a);
        a.bind(m47b);
        a.bind(m47c);
        a.raw({0x8D, 0x48, 0xFC});                                  // lea ecx, [rax-4]
        a.raw({0x83, 0xF9, 0x03});                                  // cmp ecx, 3
        uint8_t* m3 = a.jccForward(kJa);
        EmitSseRip(a, kAddss, 0, k001);                             // modes 4-7: 0.01
        a.bind(m3);
        Xorps(a, 1);                                                // delta-scaled gravities
        a.raw({0x83, 0xF8, 0x03});                                  // cmp eax, 3
        uint8_t* ty = a.jccForward(kJne);
        EmitSseRip(a, kAddss, 1, k001);
        a.bind(ty);
        a.raw({0x8B, 0x8E}).i32(0xDC0);                             // mov ecx, [rsi+0xDC0] (bullet type)
        a.raw({0x83, 0xF9, 0x01});
        uint8_t* t01a = a.jccForward(kJe);
        a.raw({0x83, 0xF9, 0x2E});
        uint8_t* t01b = a.jccForward(kJe);
        a.raw({0x83, 0xF9, 0x40});
        uint8_t* tsum1 = a.jccForward(kJne);
        EmitSseRip(a, kAddss, 1, k0005);
        uint8_t* tsum2 = JmpForward(a);
        a.bind(t01a);
        a.bind(t01b);
        EmitSseRip(a, kAddss, 1, k001);
        a.bind(tsum1);
        a.bind(tsum2);
        SseMem(a, kMulss, 1, kRsi, 0xE58);
        SseMem(a, kMulss, 1, kRsi, 0xC80);
        EmitSseRip(a, kMulss, 1, InvE());
        SseRR(a, kAddss, 0, 1);                                     // g60
        EmitSseRip(a, kMovss, 1, NearOne());
        EmitSseRip(a, kSubss, 1, E());
        EmitSseRip(a, kMulss, 1, K.mhalf);
        SseRR(a, kMulss, 0, 1);                                     // c
        for (uint8_t off = 0; off < 12; off += 4) {
            SseMemX(a, kMovss, 2, kR15, off);
            SseRR(a, kMulss, 2, 3);
            SseMem(a, kMovss, 4, kRsi, 0xD0 + off);
            SseRR(a, kMulss, 4, 0);
            SseRR(a, kAddss, 2, 4);
            SseRsp(a, 0x11, 2, (uint8_t)(0x50 + off));
        }
        a.raw({0x41, 0x8B, 0x47, 0x0C});                            // mov eax, [r15+0xC] (w lane as is)
        a.raw({0x89, 0x44, 0x24, 0x5C});                            // mov [rsp+0x5C], eax
        a.raw({0x48, 0x8D, 0x54, 0x24, 0x50});                      // lea rdx, [rsp+0x50]
        a.bind(done);
        a.copy(site + 8, 5);                                        // lea rcx, [rsp+0x40]
        a.copy(site + 16, 8);                                       // movss xmm2, [rsi+0xC80]
        a.copy(site, 8);                                            // comiss xmm8, [rsi+0xE54] (flags for the jae)
        a.jmp(site + 24);
    });

    // Speed decay before the move: the [0x11BC] factor gets r^(1-e)*(1-r^e)/((1-r)*e), r = 1-[0x11B8].
    if (uint8_t* site = SiteOf(kEf_EF_Blt_SpeedDecayWeight_5EDC92)) {
        uint8_t* target = site + 10 + (int8_t)site[9];              // the `jmp short` at the end of the site
        InstallCaveUnchecked(kEf_EF_Blt_SpeedDecayWeight_5EDC92.name, site, 10, false, [&](Asm& a) {
            a.copy(site, 8);                                        // movss xmm2, [rsi+0x11BC]
            CmpE1(a);
            uint8_t* out1 = a.jccForward(kJe);
            EmitSseRip(a, kMovss, 0, NearOne());
            SseMem(a, kSubss, 0, kRsi, 0x11B8);                     // r
            EmitSseRip(a, kComiss, 0, K.zero);
            uint8_t* out2 = a.jccForward(kJbe);
            EmitSseRip(a, kComiss, 0, NearOne());
            uint8_t* out3 = a.jccForward(kJae);
            Movaps(a, 1, 0);
            EmitFloatCall(a, &PowE, 1, 1);                          // r^e
            Movaps(a, 3, 0);
            SseRR(a, kDivss, 3, 1);
            EmitSseRip(a, kMovss, 4, NearOne());
            SseRR(a, kSubss, 4, 1);
            SseRR(a, kMulss, 3, 4);
            EmitSseRip(a, kMovss, 4, NearOne());
            SseRR(a, kSubss, 4, 0);
            EmitSseRip(a, kMulss, 4, E());
            SseRR(a, kDivss, 3, 4);
            SseRR(a, kMulss, 2, 3);
            a.bind(out1);
            a.bind(out2);
            a.bind(out3);
            a.jmp(target);
        });
    }

    // Spin: vel is rotated by w ([0xEFC], xmm1) and then w *= 1 - k*d (k = [0xF00]). The
    // rotation gets the exact weight (1-(1-kD)^e)/(kD) (e when there is no decay), the
    // decay (1-kD)^e, D = d/e.
    if (uint8_t* site = SiteOf(kEf_EF_Blt_SpinRateExact_5ED66E)) {
        uint8_t* callee = RipTarget(site, 1, 5);
        InstallCaveUnchecked(kEf_EF_Blt_SpinRateExact_5ED66E.name, site, 5, false, [&](Asm& a) {
            CmpE1(a);
            uint8_t* orig1 = a.jccForward(kJe);
            SseMem(a, kMovss, 0, kRsi, 0xF00);
            SseMem(a, kMulss, 0, kRsi, 0xC80);
            EmitSseRip(a, kMulss, 0, InvE());                       // kD
            Xorps(a, 2);
            Comiss(a, 0, 2);
            uint8_t* noDecay = a.jccForward(kJbe);
            Movaps(a, 2, 0);
            EmitFloatCall(a, &LerpE, 2, 2);
            SseRR(a, kDivss, 2, 0);
            SseRR(a, kMulss, 1, 2);
            uint8_t* orig2 = JmpForward(a);
            a.bind(noDecay);
            EmitSseRip(a, kMulss, 1, E());
            a.bind(orig1);
            a.bind(orig2);
            a.call(callee);
            a.jmp(site + 5);
        });
    }
    PatchSite(kEf_EF_Blt_SpinDecayExact_5ED68A, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        SseMem(a, kMovss, 0, kRsi, 0xF00);
        SseMem(a, kMulss, 0, kRsi, 0xC80);
        EmitSseRip(a, kMulss, 0, InvE());
        a.copy(site + 8, 4);                                        // movaps xmm1, xmm9 (1.0)
        SseRR(a, kSubss, 1, 0);
        EmitFloatCall(a, &PowE, 1, 1);
        Xorps(a, 0);                                                // the game's `subss xmm1, xmm0` follows
        a.jmp(site + 20);
        a.bind(v);
        a.copy(site, 20);
        a.jmp(site + 20);
    });

    // Ring bomb (mode 14): vel.y -= 0.005 (already * e) before pos += M*vel once per update.
    // The step becomes (M*vel + M.row1*c)*e, c = -0.0025*(1-e) while the fall is not clamped.
    const float* km00025 = ConstFloat(-0.0025f);
    const float* km01 = ConstFloat(-0.1f);
    PatchSite(kEf_EF_Blt_RingBombMoveExact_6E4E58, [&](Asm& a, uint8_t* site) {
        a.call(RipTarget(site, 1, 5));
        CmpE1(a);
        uint8_t* done = a.jccForward(kJe);
        EmitSseRip(a, kMovss, 2, E());
        EmitSseRip(a, kMovss, 1, NearOne());
        SseRR(a, kSubss, 1, 2);
        EmitSseRip(a, kMulss, 1, km00025);
        SseMem(a, kMovss, 0, kRsi, 0xE04);
        EmitSseRip(a, kComiss, 0, km01);
        uint8_t* apply = a.jccForward(kJa);
        Xorps(a, 1);
        a.bind(apply);
        for (int32_t off = 0; off < 12; off += 4) {
            SseMem(a, kMovss, 0, kRsi, 0xD0 + off);
            SseRR(a, kMulss, 0, 1);
            SseMem(a, kAddss, 0, kRsi, 0xDF0 + off);
            SseRR(a, kMulss, 0, 2);
            SseMem(a, 0x11, 0, kRsi, 0xDF0 + off);
        }
        a.bind(done);
        a.jmp(site + 5);
    });

    // Attached-part scale [0xCCC] += (target - x)*0.05*d.
    ExactRipK(kEf_EF_Blt_ScaleLerpExact_6E4C91, 2);
    // Orientation turned toward the reference frame by fraction f per update -> 1-(1-f)^e.
    LerpAround(kEf_EF_Blt_EB0RotFrac_6B4F88, 7, true);
    // Modes 37-39 frame follow and mode 43 homing: fraction 1-(1-f)^e, max angle * e.
    for (const SigSite* s : {&kEf_EF_Blt_M37FrameTurn_675467, &kEf_EF_Blt_M43HomingTurn_674732}) {
        PatchSite(*s, [&](Asm& a, uint8_t* site) {
            a.copy(site, s->len);
            EmitSseRip(a, kMulss, 0, E());
            EmitFloatCall(a, &LerpE, 1, 1);
            a.jmp(site + s->len);
        });
    }
}

// --- Em0000 (the common flying/ground enemy class) and its states.

void FixEm0000() {
    // Per-update smoothing whose 0.1 lives in a register that has other uses: only this use
    // reads the scaled factor.
    RewriteSite(kEf_EF_Em0000_SpeedLerp12_55BBEB, {{0, 5, kMulss, 1, K.lerp01}});
    RewriteSite(kEf_EF_Em0000_SpeedLerp_564B4B, {{0, 5, kMulss, 1, K.lerp01}});
    RewriteSite(kEf_EF_Em0000_DriftXLerp_53F463, {{0, 4, kMulss, 1, K.lerp01}});
    RewriteSite(kEf_EF_Em0000_DriftXDecay_53F487, {{0, 4, kMulss, 0, K.pow01}});
    RewriteSite(kEf_EF_Em0000_MoveByLerp_5654E6, {{0, 4, kMovss, 2, K.lerp01}});
    RewriteSite(kEf_EF_Em0000_HoverPullLerp_555B63, {{0, 4, kMovss, 2, K.lerp01}});
    RewriteSite(kEf_EF_Em0000_HoverPullLerp_556AB3, {{0, 4, kMovss, 2, K.lerp01}});
    // Float frame timers -= 1.0 per update -> -= e.
    RewriteSite(kEf_EF_Em0000_PullTimer_5475F1, {{0, 4, kSubss, 1, E()}});
    RewriteSite(kEf_EF_Em0000_PullTimer_550DA7, {{0, 4, kSubss, 0, E()}});
    // Camera-near fade -= 0.1 / fade-in += 0.1 per update.
    if (uint8_t* site = SiteOf(kEf_EF_Em0000_FadeNear_5EE52E)) {
        const void* floor = RipTarget(site + 4, 4, 8);
        RewriteSite(kEf_EF_Em0000_FadeNear_5EE52E, {{0, 4, kSubss, 1, K.lin01}, {4, 8, kMovss, 6, floor}});
    }
    RewriteSite(kEf_EF_Em0000_FadeIn_5EE53C, {{8, 4, kAddss, 1, K.lin01}});
    // Orbit angle += step per update -> step * e.
    RewriteSite(kEf_EF_Em0000_OrbitAngle_561FB5, {{3, 0, kMulss, 0, E()}});

    // Delta-scaled lerps and decays.
    ExactRegK(kEf_EF_Em0000_PitchLerpExact_555DA2, 1);
    ExactRegK(kEf_EF_Em0000_PitchLerpExactB_556CEE, 1);
    ExactRegK(kEf_EF_Em0000_TiltLerpExact_53F6BE, 1);
    ExactRipK(kEf_EF_Em0000_HeightLerpExact_566EA1, 2);
    ExactDecay(kEf_EF_Em0000_SpeedDecay275C0_5429B6);
    ExactDecay(kEf_EF_Em0000_SpeedDecay275C0B_53E81C);
    ExactDecay(kEf_EF_Em0000_Decay275BC_5463E0);

    // Fall state: pos.y += d*vy, then vy -= g once per update (explicit Euler). Exact:
    // vy -= g*e and pos.y gets 0.5*g*d*(1-e) on top of d*vy (xmm1).
    PatchSite(kEf_EF_Em0000_FallVyExact_566820, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        SseMem(a, kMovss, 2, kRbx, 0x5FA4);
        EmitSseRip(a, kMulss, 2, E());
        SseRR(a, kSubss, 0, 2);
        EmitSseRip(a, kMovss, 2, NearOne());
        EmitSseRip(a, kSubss, 2, E());
        SseMem(a, kMulss, 2, kRbx, 0x5FA4);
        SseMem(a, kMulss, 2, kRbx, 0xC84);
        EmitSseRip(a, kMulss, 2, K.half);
        SseRR(a, kAddss, 1, 2);
        a.jmp(site + 8);
        a.bind(v);
        a.copy(site, 8);
        a.jmp(site + 8);
    });

    // Slide: s *= 0.96 (already ^e), then pos += R*(0,0,s)*c0 per update. Exact step:
    // s*c0*0.96^(1-e)*(1-0.96^e)/0.04.
    if (uint8_t* site = SiteOf(kEf_EF_Em0000_SlideStepExact_544E38)) {
        const float* c0 = (const float*)RipTarget(site, 4, 8);
        const float* cSlide = ConstFloat(*c0 * 0.96f);
        const float* powInv = NewScaled("Em0000 slide 1/0.96", Scale::Pow, 1.0f / 0.96f);
        const float* ratio = NewScaled("Em0000 slide 0.04", Scale::LerpRatio, 0.04f);
        PatchSite(kEf_EF_Em0000_SlideStepExact_544E38, [&](Asm& a, uint8_t* site) {
            CmpE1(a);
            uint8_t* v = a.jccForward(kJe);
            EmitSseRip(a, kMovss, 2, cSlide);
            EmitSseRip(a, kMulss, 2, powInv);
            EmitSseRip(a, kMulss, 2, ratio);
            a.jmp(site + 8);
            a.bind(v);
            EmitSseRip(a, kMovss, 2, c0);
            a.jmp(site + 8);
        });
    }

    // Hover/fly states: decay paired with an undamped pull.
    struct { const SigSite* w; uint8_t base; const SigSite* d1; const SigSite* d2; } hover[] = {
        {&kEf_EF_EmHover_PullWeight_53BC91, kR14, &kEf_EF_EmHover_VelDecay_53BCC0, &kEf_EF_EmHover_VelDecay_53BD07},
        {&kEf_EF_EmHover_PullWeight_53CCE6, kRsi, &kEf_EF_EmHover_VelDecay_53CD13, &kEf_EF_EmHover_VelDecay_53CD57},
        {&kEf_EF_EmHover_PullWeight_5423AC, kRdi, &kEf_EF_EmHover_VelDecay_5423D8, &kEf_EF_EmHover_VelDecay_54241E},
        {&kEf_EF_EmHover_PullWeight_5435B8, kRsi, &kEf_EF_EmHover_VelDecay_5435E5, &kEf_EF_EmHover_VelDecay_543629},
        {&kEf_EF_EmHover_PullWeight_558C52, kR15, &kEf_EF_EmHover_VelDecay_558C81, &kEf_EF_EmHover_VelDecay_558CC8},
        {&kEf_EF_EmHover_PullWeight_5B6297, kRdi, &kEf_EF_EmHover_VelDecay_5B62C3, &kEf_EF_EmHover_VelDecay_5B6308},
        {&kEf_EF_Em2001_HoverPullWeight_5B9DCF, kRbx, &kEf_EF_Em2001_HoverVelDecay_5B9DF6, nullptr},
    };
    for (auto& h : hover) {
        ExactPullWeight(*h.w, h.base);
        ExactDecay(*h.d1);
        if (h.d2) ExactDecay(*h.d2);
    }

    // Drift duration counter (cleared on exactly zero) counts 60 Hz ticks.
    ZeroBranchOnTick(kEf_EF_EmBase_DriftFrames_689F5C, 7);
    // Turn max step * e (the register also holds a one-time yaw offset).
    RewriteSite(kEf_EF_Em0030_ApprMax_552B80, {{4, 0, kMulss, 3, E()}});
}

// --- Other enemy classes.

void FixEnemyClasses() {
    // Em4000 beam: length += growth per update; tilt lerp k = 0.004 (0.002 doubled by the
    // game); sweep x' = 0.998^e x - 0.02*(1-0.998^e)/0.002 (constant part here).
    ScaleThenReplay(kEf_EF_Em4000_BeamGrow_53AB57, 0);
    ScaleThenReplay(kEf_EF_Em4000_BeamGrowB_53AEB8, 0);
    if (uint8_t* site = SiteOf(kEf_EF_Em4000_BeamTiltLerp_53AC32)) {
        const float* k = (const float*)RipTarget(site, 4, 8);
        const float* lerp = NewScaled("Em4000 beam tilt", Scale::Lerp, 2.0f * *k);
        RewriteSite(kEf_EF_Em4000_BeamTiltLerp_53AC32, {{0, 8, kMulss, 1, lerp}, {8, 0, kMulss, 1, K.half}});
    }
    if (uint8_t* site = SiteOf(kEf_EF_Em4000_BeamSweepStep_53AF8B)) {
        const void* k = RipTarget(site, 4, 8);
        const float* ratio = NewScaled("Em4000 beam sweep 0.002", Scale::LerpRatio, 0.002f);
        PatchSite(kEf_EF_Em4000_BeamSweepStep_53AF8B, [&](Asm& a, uint8_t* site) {
            EmitSseRip(a, kMovss, 4, k);
            EmitSseRip(a, kMulss, 4, ratio);
            SseRR(a, kAddss, 0, 4);
            a.jmp(site + 8);
        });
    }
    // Em4000Core bob: clamp((t - y)*0.1, -m, m) -> lerp 1-(1-0.1)^e, clamp m*e; spin += rate*e.
    if (uint8_t* site = SiteOf(kEf_EF_Em4000Core_BobLerp_68431D)) {
        const float* k = (const float*)RipTarget(site + 4, 4, 8);
        const float* lerp = NewScaled("Em4000Core bob", Scale::Lerp, *k);
        RewriteSite(kEf_EF_Em4000Core_BobLerp_68431D, {{4, 8, kMulss, 0, lerp}, {12, 0, kMulss, 1, E()}});
    }
    RewriteSite(kEf_EF_Em4000Core_SpinStep_6B8E41, {{8, 0, kMulss, 0, E()}});

    // Baa000 (mount) turn-rate ramp += 0.01 per update.
    RewriteSite(kEf_EF_Baa000_TurnRateRamp_5025F7, {{0, 4, kAddss, 2, K.lin001}});

    // Em5000FloatFloor: spin velocity and part values (v += (t-v)*k*d with d in xmm0).
    ExactRipKD0(kEf_EF_Em5000FF_SpinVelExact_686479);
    ExactRipKD0(kEf_EF_Em5000FF_PartLerpExact_686A25);
    ExactRipKD0(kEf_EF_Em5000FF_PartLerpExactB_686A61);
    ExactRipKD0(kEf_EF_Em8800_LerpExact_687A92);
    ExactRipKD0(kEf_EF_Em8800_LerpExactB_687AC6);
    ExactRipKD0(kEf_EF_Em0080_BoneLerpExact_6C5095);
    // Sink: velocity -= 0.01*d, then each part y += velocity once per update (semi-implicit):
    // step e*(v - 0.005*(1-e)).
    const float* km0005 = ConstFloat(-0.005f);
    PatchSite(kEf_EF_Em5000FF_SinkStep_68669D, [&](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm0, [rsi+0x1778]
        CmpE1(a);
        uint8_t* done = a.jccForward(kJe);
        EmitSseRip(a, kMovss, 1, NearOne());
        EmitSseRip(a, kSubss, 1, E());
        EmitSseRip(a, kMulss, 1, km0005);
        SseRR(a, kAddss, 0, 1);
        EmitSseRip(a, kMulss, 0, E());
        a.bind(done);
        a.jmp(site + 8);
    });

    // Em1000 blend parameters and Em1100 camera distance.
    ExactRipK(kEf_EF_Em1000_BlendExactA_6ABAF2, 2);
    ExactRipK(kEf_EF_Em1000_BlendExactD_6ABD86, 1);
    ExactRegKOverD(kEf_EF_Em1000_BlendExactB_6ABCCC, 4, kRbx, 0xC84);
    ExactRegKOverD(kEf_EF_Em1000_BlendExactC_6ABD19, 2, kRbx, 0xC84);
    RewriteSite(kEf_EF_Em1100_CamDistLerp_681045, {{0, 4, kMulss, 1, K.lerp001}});

    // Em5000 hover pulls (paired with pow 0.9 decays in the table).
    RewriteSite(kEf_EF_Em5000_HoverPullLerp_5C3EC7, {{0, 3, kMovss, 2, K.lerp01}});
    RewriteSite(kEf_EF_Em5000_HoverPullLerp_5C4CEF, {{0, 4, kMovss, 2, K.lerp01}});

    // Em8000 drift: s = vel*0.1; pos += s*d; vel -= s*d. The shared 0.1 -> (1-(1-0.1D)^e)/d.
    if (uint8_t* site = SiteOf(kEf_EF_Em8000_DriftDecayExact_5CC74D)) {
        const void* k = RipTarget(site, 4, 8);
        PatchSite(kEf_EF_Em8000_DriftDecayExact_5CC74D, [&](Asm& a, uint8_t* site) {
            CmpE1(a);
            uint8_t* v1 = a.jccForward(kJe);
            SseMem(a, kMovss, 2, kRdi, 0xC84);
            EmitSseRip(a, kComiss, 2, K.zero);
            uint8_t* v2 = a.jccForward(kJbe);
            EmitSseRip(a, kMulss, 2, k);
            ExactFrac(a, 2);
            SseMem(a, kDivss, 2, kRdi, 0xC84);
            a.jmp(site + 8);
            a.bind(v1);
            a.bind(v2);
            EmitSseRip(a, kMovss, 2, k);
            a.jmp(site + 8);
        });
    }
    ExactRipK(kEf_EF_Em8000_RotorBoneLerpExact_6BABAD, 0);
    ExactRipK(kEf_EF_Em8000_RotorBoneLerpExact_6BABF9, 0);
    ExactRipK(kEf_EF_Em5000_DeltaLerpExact_6854F0, 4);
    ExactRipK(kEf_EF_Em9000_DeltaLerpExact_688639, 4);
    ExactRipK(kEf_EF_Em9010_DeltaLerpExact_688BF5, 4);

    // Em2000 dash: the move uses the speed before the per-update decrement 0.01 -> d*(v + 0.005*(1-e)).
    const float* k0005 = ConstFloat(0.005f);
    PatchSite(kEf_EF_Em2000_DashMoveExact_5B6D98, [&](Asm& a, uint8_t* site) {
        a.copy(site, 8);                                            // movss xmm3, [rbx+0x275D0]
        CmpE1(a);
        uint8_t* done = a.jccForward(kJe);
        EmitSseRip(a, kMovss, 4, NearOne());
        EmitSseRip(a, kSubss, 4, E());
        EmitSseRip(a, kMulss, 4, k0005);
        SseRR(a, kAddss, 3, 4);
        a.bind(done);
        a.jmp(site + 8);
    });
    RewriteSite(kEf_EF_Em2000_GlowLerp_6B0D68, {{0, 4, kMulss, 1, K.lerp01}});

    // Em7000 height: pos.y += clamp((t-y)*0.05)*d with d in xmm3 -> factor exact/d.
    if (uint8_t* site = SiteOf(kEf_EF_Em7000_HeightLerpExact_6A4E07)) {
        const void* k = RipTarget(site, 4, 8);
        PatchSite(kEf_EF_Em7000_HeightLerpExact_6A4E07, [&](Asm& a, uint8_t* site) {
            CmpE1(a);
            uint8_t* v1 = a.jccForward(kJe);
            EmitSseRip(a, kComiss, 3, K.zero);
            uint8_t* v2 = a.jccForward(kJbe);
            Movaps(a, 4, 3);
            EmitSseRip(a, kMulss, 4, k);
            ExactFrac(a, 4);
            SseRR(a, kDivss, 4, 3);
            SseRR(a, kMulss, 2, 4);
            a.jmp(site + 8);
            a.bind(v1);
            a.bind(v2);
            EmitSseRip(a, kMulss, 2, k);
            a.jmp(site + 8);
        });
    }

    // Em2100Parts: scale lerp 0.3, spin += rate*e, gather-turn fraction, gravity.
    RewriteSite(kEf_EF_Em2100Parts_ScaleLerp_5BA324, {{0, 4, kMulss, 1, K.lerp03}});
    RewriteSite(kEf_EF_Em2100Parts_SpinX_5BA332, {{8, 0, kMulss, 0, E()}});
    RewriteSite(kEf_EF_Em2100Parts_SpinY_5BA34F, {{8, 0, kMulss, 0, E()}});
    LerpAround(kEf_EF_Em2100Parts_GatherRotFrac_5BA58F, 0, true);
    LerpAround(kEf_EF_Em2100Parts_GatherRotFrac_5BA653, 0, true);
    ExactPartGravity(kEf_EF_Em2100Parts_GravityExact_682E36, 0x1344, 0x1374);
    // Em8800Parts: gravity, return-turn fraction, position lerps whose k ramps to 1.
    ExactPartGravity(kEf_EF_Em8800Parts_GravityExact_688248, 0x13D4, 0x1404);
    LerpAround(kEf_EF_Em8800Parts_HomeRotFrac_5CE57B, 0, true);
    LerpAround(kEf_EF_Em8800Parts_HomeRotFrac_5CE63F, 0, true);
    PatchSite(kEf_EF_Em8800Parts_HomePosExact_5CE6FB, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(kJe);
        for (uint8_t r : {2, 3}) {
            EmitSseRip(a, kMulss, r, InvE());
            SseRR(a, kMinss, r, 7);                                 // xmm7 = 1.0
            EmitFloatCall(a, &LerpE, r, r);
        }
        a.jmp(site + 8);
        a.bind(v);
        a.copy(site, 8);
        a.jmp(site + 8);
    });

    // Em5000FloatRubble: piece pos += (t - pos)*0.1*d (vscale by d in xmm2, k = xmm6).
    PatchSite(kEf_EF_Em5000FloatRubble_FollowExact_5FA6E8, [](Asm& a, uint8_t* site) {
        a.copy(site, 3);                                            // movaps xmm2, xmm0 (d)
        CmpE1(a);
        uint8_t* done = a.jccForward(kJe);
        SseRR(a, kMulss, 2, 6);
        ExactFrac(a, 2);
        SseRR(a, kDivss, 2, 6);
        a.bind(done);
        a.copy(site + 3, 5);                                        // lea rdx, [rsp+0x60]
        a.jmp(site + 8);
    });

    // Em011e: vel *= 1 - 0.1d, then pos += vel once per update. Exact: vel *= r^e and pos +=
    // vel_old*r*(1-r^e)/(1-r); the position step adds vel_new*e, this cave the difference.
    PatchSite(kEf_EF_Em011e_StopDecayExact_67EBE6, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* orig1 = a.jccForward(kJe);
        EmitSseRip(a, kMulss, 0, InvE());                           // c = 1 - r
        EmitSseRip(a, kComiss, 0, K.zero);
        uint8_t* orig2 = a.jccForward(kJbe);
        Movaps(a, 1, 8);                                            // xmm8 = 1.0
        SseRR(a, kSubss, 1, 0);
        EmitSseRip(a, kMaxss, 1, K.zero);                           // r
        Movaps(a, 3, 1);
        EmitFloatCall(a, &PowE, 1, 1);                              // r^e
        Movaps(a, 4, 8);
        SseRR(a, kSubss, 4, 1);
        SseRR(a, kMulss, 4, 3);
        SseRR(a, kDivss, 4, 0);
        EmitSseRip(a, kMovss, 5, E());
        SseRR(a, kMulss, 5, 1);
        SseRR(a, kSubss, 4, 5);                                     // r(1-r^e)/c - e*r^e
        for (int32_t off = 0; off < 12; off += 4) {
            SseMem(a, kMovss, 5, kRbx, off);                        // vel_old (rbx = vel)
            SseRR(a, kMulss, 5, 4);
            SseMem(a, kAddss, 5, kRdi, 0x50 + off);
            SseMem(a, 0x11, 5, kRdi, 0x50 + off);
        }
        Movaps(a, 8, 1);
        Movaps(a, 2, 1);
        a.jmp(site + 9);
        a.bind(orig1);
        a.bind(orig2);
        a.copy(site, 9);
        a.jmp(site + 9);
    });
    // pos += vel (vector add call) -> pos.xyz += vel.xyz*e lane by lane (x last, as the
    // callee leaves it in xmm0), w as is.
    PatchSite(kEf_EF_Em011e_PosStep_67EBFA, [](Asm& a, uint8_t* site) {
        a.copy(site, 11);                                           // the call's arguments
        for (int32_t off : {12, 8, 4, 0}) {
            SseMem(a, kMovss, 0, kRbx, off);
            if (off != 12) EmitSseRip(a, kMulss, 0, E());
            SseMem(a, kAddss, 0, kRdi, 0x50 + off);
            SseMem(a, 0x11, 0, kRdi, 0x50 + off);
        }
        a.jmp(site + 16);
    });

    // Em0030Parts: yaw lerp 0.1; height y += dy*g*d, scaled to the exact fraction.
    RewriteSite(kEf_EF_Em0030Parts_YawLerp_6BB072, {{0, 4, kMulss, 0, K.lerp01}});
    PatchSite(kEf_EF_Em0030Parts_HeightLerpExact_6BB124, [](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v1 = a.jccForward(kJe);
        EmitSseRip(a, kComiss, 0, K.zero);
        uint8_t* v2 = a.jccForward(kJbe);
        Movaps(a, 1, 6);
        SseRR(a, kMulss, 1, 0);
        ExactFrac(a, 1);
        Movaps(a, 3, 6);
        SseRR(a, kMulss, 3, 0);
        SseRR(a, kDivss, 1, 3);
        SseRsp(a, 0x10, 3, 0x54);
        SseRR(a, kMulss, 3, 1);
        SseRsp(a, 0x11, 3, 0x54);
        a.bind(v1);
        a.bind(v2);
        a.copy(site, 8);
        a.jmp(site + 8);
    });

    // Frame counters on 60 Hz ticks.
    for (const SigSite* s : {&kEf_EF_Em5000_LevelDebounce_685803, &kEf_EF_Ba0420_Wait9F0_452F0C, &kEf_EF_Ba0420_Fade9F0_47393E,
                             &kEf_EF_BaAnimal_ContactFrames_425421, &kEf_EF_Baa000_StateFrames_468082,
                             &kEf_EF_Baa010_StateFrames_46D093})
        IncTicks(*s);
    DecMemByTicks(kEf_EF_Em5000Block_StateDelay_58BC67);
}

}  // namespace

void FixEnemyFinal() {
    Log("enemy and bullet final pass:");
    InvE();
    NearOne();
    K.zero = ConstFloat(0.0f);
    K.half = ConstFloat(0.5f);
    K.mhalf = ConstFloat(-0.5f);
    K.lerp01 = NewScaled("enemy final smoothing 0.1", Scale::Lerp, 0.1f);
    K.lerp001 = NewScaled("enemy final smoothing 0.01", Scale::Lerp, 0.01f);
    K.lerp03 = NewScaled("enemy final smoothing 0.3", Scale::Lerp, 0.3f);
    K.lin001 = NewScaled("enemy final step 0.01", Scale::Lin, 0.01f);
    K.lin01 = NewScaled("enemy final step 0.1", Scale::Lin, 0.1f);
    K.pow01 = NewScaled("enemy final decay 0.1", Scale::Pow, 0.1f);
    InstallTable(kEfScaledConsts, _countof(kEfScaledConsts));
    InstallTable(kEfArgCalls, _countof(kEfArgCalls));
    FixBulletMoves();
    FixEm0000();
    FixEnemyClasses();
}
