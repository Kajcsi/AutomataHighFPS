#pragma once
// Building blocks shared by the subsystem fixes.
#include "common.h"
#include "memory.h"

// How a per-frame constant is rescaled for a frame worth e = min(step, 1) 60 Hz frames.
// At e = 1 every formula yields the original value exactly.
enum class Scale {
    Lin,        // c * e          amounts added once per frame
    Pow,        // c ^ e          multiplicative decay applied once per frame (v *= c)
    Lerp,       // 1 - (1-c)^e    smoothing factor of v += (target - v) * c
    Sq,         // c * e^2        thresholds on squared per-frame displacement
    Inv,        // c / e          divisors
    LerpRatio,  // Lerp / c       multiplier turning a per-frame c-lerp argument into Lerp (1 at e = 1)
};

// Recomputes every registered constant; called once per frame by the frame hook.
void UpdateScaledConstants(float e);
// Value currently served for the named constant (tests and diagnostics).
const float* ScaledConstant(const char* name);
// A float in the near arena recomputed every frame. With `source`, the base value is
// re-read from that game address every frame.
float* NewScaled(const char* name, Scale kind, float base, const float* source = nullptr);

// Redirects the RIP-relative float operand of `insn` to a scaled copy of the
// constant it reads; `expected` guards against a different constant.
bool ScaleOperand(const char* name, uint8_t* insn, const uint8_t* opcode, size_t opLen, size_t dispOff, size_t len,
                  Scale kind, float expected);
void ScaleConst(const char* name, const char* sig, size_t off, std::initializer_list<uint8_t> opcode, Scale kind,
                float expected, size_t dispOff = 4, size_t len = 8);
// Points a RIP-relative operand at a shared mod value.
void RetargetConst(const char* name, const char* sig, size_t off, std::initializer_list<uint8_t> opcode, size_t dispOff,
                   size_t len, const void* target);

// Runs a whole function (prologue = its first position-independent instructions) only on 60 Hz ticks.
bool GateOnTick(const char* name, uint8_t* fn, std::initializer_list<uint8_t> prologue);
// Runs the callee of one `call rel32` only on 60 Hz ticks.
bool GateCallOnTick(const char* name, uint8_t* site);
// `inc dword [reg+disp32]` (ModRM `modrm`) -> add ticks60.
void IncByTicks(const char* name, uint8_t* site, uint8_t modrm);
// `dec eax; mov [rdi+disp8], eax` -> subtract ticks60.
void DecEaxByTicks(const char* name, uint8_t* site, uint8_t disp8);
// Emits a call to `float fn(float)` (xmm<in> -> xmm<out>, both < 8) preserving every
// volatile register and the flags. rsp must be 16-byte aligned at that point
// (`viaCall`: the cave was entered by a call, so rsp is 8 off).
void EmitFloatCall(Asm& a, float (*fn)(float), uint8_t in, uint8_t out, bool viaCall = false);
// Replaces `mulss xD, [reg+disp]` (bytes `orig`, length `len`) with xD *= LerpE([reg+disp]):
// the per-object smoothing factor at that address is rescaled for this frame.
void EmitLerpMulssMem(Asm& a, const uint8_t* orig, size_t len);
// A float constant 1.0 within reach of RIP-relative cave code.
const float* NearOne();
// x^e and 1-(1-x)^e for the current frame (x unchanged at e = 1).
float PowE(float x);
float LerpE(float x);

// Generated table rows (src/generated/*.inc). `sigAlt` is used where the Steam and
// Compat builds compiled a site differently (tried when `sig` has no match).
struct ScaledConstPatch {
    const char* name;
    const char* sig;
    const char* sigAlt;
    int off;
    uint8_t opcode[8];  // instruction bytes before the disp32
    uint8_t opLen;
    uint8_t dispOff;
    uint8_t len;
    Scale kind;
    float base;
};
struct ScaledDoublePatch {  // same, for double-precision operands (mulsd)
    const char* name;
    const char* sig;
    const char* sigAlt;
    int off;
    uint8_t opcode[8];
    uint8_t opLen;
    uint8_t dispOff;
    uint8_t len;
    Scale kind;
    double base;
};
// call rel32: rescale float argument xmm<reg>, then tail-jump to the callee.
// kind Lerp means the argument itself is a per-frame smoothing factor: x -> 1-(1-x)^e.
// Any other kind multiplies the argument by the scaled constant (kind, base).
struct ArgCallPatch {
    const char* name;
    const char* sig;
    const char* sigAlt;
    int off;
    uint8_t reg;
    Scale kind;
    float base;
};
struct VecCallPatch {  // call rel32: call the callee, then scale x, y, z of the vector at the original rcx
    const char* name;
    const char* sig;
    const char* sigAlt;
    int off;
};
struct SigSite {  // a hand-written patch site with its original bytes
    const char* name;
    const char* sig;
    const char* sigAlt;
    int off;
    uint8_t orig[48];
    uint8_t len;
    uint64_t cmpMask;  // bit i set: orig[i] must match (clear for relocated displacements)
};

// Unique match of `sig`, else of `sigAlt`; nullptr (logged) if neither matches once.
uint8_t* FindEither(const char* name, const char* sig, const char* sigAlt);
void InstallTable(const ScaledConstPatch* rows, size_t n);
void InstallTable(const ScaledDoublePatch* rows, size_t n);
void InstallTable(const ArgCallPatch* rows, size_t n);
void InstallTable(const VecCallPatch* rows, size_t n);
// Finds a SigSite and checks its original bytes; returns the patch address or nullptr.
uint8_t* SiteOf(const SigSite& s);
// Replaces a SigSite's original bytes with a jmp to a cave built by `emit` (which must jump back).
template <class F>
bool PatchSite(const SigSite& s, F emit) {
    uint8_t* site = SiteOf(s);
    return site && InstallCaveUnchecked(s.name, site, s.len, false, [&](Asm& a) { emit(a, site); });
}
// `dec eax` followed by position-independent stores of eax (a frame counter) -> subtract ticks60.
void DecEaxByTicks(const SigSite& s);
// `[REX] dec dword [reg+disp32]` or `sub dword [reg+disp32], 1` -> subtract ticks60
// (flags as the subtraction leaves them).
void DecMemByTicks(const SigSite& s);
// A frame counter whose result feeds a branch on exactly zero: on a frame without a
// 60 Hz tick the first `counterLen` bytes (the decrement and its store) are skipped
// and the flags say "not zero", so the action can't fire twice while the counter
// holds at 0. The rest of the site runs either way. Clobbers no registers.
bool ZeroBranchOnTick(const SigSite& s, size_t counterLen);
// Emits `<op>ss xmm<reg>, [target]` (op 0x10 movss, 0x58 addss, 0x59 mulss, 0x5C subss,
// 0x5D minss); op 0x2F emits `comiss xmm<reg>, [target]`.
void EmitSseRip(Asm& a, uint8_t op, uint8_t reg, const void* target);
const uint8_t kMovss = 0x10, kAddss = 0x58, kMulss = 0x59, kSubss = 0x5C, kMinss = 0x5D, kComiss = 0x2F;
// One instruction inside a SigSite (`len` bytes at offset `at`) replaced by EmitSseRip(op, reg, target).
struct SseSub {
    uint8_t at, len, op, reg;
    const void* target;
};
// Replays a SigSite's position-independent bytes with the given instructions replaced.
bool RewriteSite(const SigSite& s, std::initializer_list<SseSub> subs);
// xmm<reg> *= e, then the original instructions (a per-frame step added to state).
bool ScaleThenReplay(const SigSite& s, uint8_t reg);
// The original instructions (which compute a per-frame step in xmm<reg>), then xmm<reg> *= e.
bool ReplayThenScale(const SigSite& s, uint8_t reg);
// xmm<reg> = LerpE(xmm<reg>) before the original instructions, or after them (`after`). reg < 8.
bool LerpAround(const SigSite& s, uint8_t reg, bool after);
// `mulss xD, [reg+disp]` with a per-object smoothing factor -> xD *= LerpE([reg+disp]).
bool LerpObjectFactor(const SigSite& s);
// A float constant in the near arena (reachable RIP-relative from caves).
const float* ConstFloat(float v);
// 1/e for the current frame (1.0 at <= 60 FPS).
const float* InvE();
// Smoothing fractions the game already multiplies by the frame delta, f = k*d (linear in
// d, so it drifts above 60 FPS), made exact: f -> 1-(1-min(f/e, 1))^e, identical at e = 1.
// The first `prefix` bytes of the site compute f in xmm<reg> (< 8); the rest of the site
// (if any) is the original min with 1.0.
bool ExactFraction(const SigSite& s, size_t prefix, uint8_t reg);
// Same for the fraction argument xmm2 of a `call rel32`. With `k`, xmm2 holds only the
// delta d and the callee multiplies by k itself: xmm2 -> (1-(1-min(k*d/e, 1))^e) / k,
// and the original call runs unchanged at e = 1.
bool ExactArgCall(const SigSite& s, float k = 0.0f);
