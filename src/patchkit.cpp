#include "patchkit.h"
#include "game.h"
#include "timing.h"
#include <cmath>

// ---------------------------------------------------------------------------
// Scaled constants: per-frame amounts the game reads RIP-relative are redirected
// to floats in the near arena, recomputed every frame.

struct ScaledEntry {
    const char* name;
    Scale kind;
    float base;
    const float* source;
    float* value;
};
static ScaledEntry g_scaled[512];
static size_t g_scaledCount;

static float Evaluate(Scale kind, float c, float e) {
    if (!(e < 1.0f)) return kind == Scale::LerpRatio ? 1.0f : c;  // exactly vanilla at <= 60 fps
    switch (kind) {
    case Scale::Lin: return c * e;
    case Scale::Pow: return powf(c, e);
    case Scale::Lerp: return 1.0f - powf(1.0f - c, e);
    case Scale::Sq: return c * e * e;
    case Scale::Inv: return e > 1e-6f ? c / e : c;
    case Scale::LerpRatio: return (1.0f - powf(1.0f - c, e)) / c;
    }
    return c;
}

static float ReadFloat(const float* p, float fallback) {
    __try {
        return *(const volatile float*)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

static void UpdateScaledDoubles(float e);

void UpdateScaledConstants(float e) {
    UpdateScaledDoubles(e);
    for (size_t i = 0; i < g_scaledCount; ++i) {
        ScaledEntry& s = g_scaled[i];
        float base = s.source ? ReadFloat(s.source, s.base) : s.base;
        *s.value = Evaluate(s.kind, base, e);
    }
}

const float* ScaledConstant(const char* name) {
    for (size_t i = 0; i < g_scaledCount; ++i)
        if (!strcmp(g_scaled[i].name, name)) return g_scaled[i].value;
    return nullptr;
}

float* NewScaled(const char* name, Scale kind, float base, const float* source) {
    if (g_scaledCount >= _countof(g_scaled)) {
        Log("  scaled constant table full (%s)", name);
        return nullptr;
    }
    float* v = (float*)g_arena.Alloc(4, 4);
    if (!v) return nullptr;
    *v = Evaluate(kind, base, 1.0f);
    g_scaled[g_scaledCount++] = {name, kind, base, source, v};
    return v;
}

bool ScaleOperand(const char* name, uint8_t* insn, const uint8_t* opcode, size_t opLen, size_t dispOff, size_t len,
                  Scale kind, float expected) {
    if (!insn || memcmp(insn, opcode, opLen) != 0) {
        Log("  [skip] %s: unexpected bytes", name);
        return false;
    }
    float original = *(float*)RipTarget(insn, dispOff, len);
    if (fabsf(original - expected) > fabsf(expected) * 1e-4f + 1e-9f) {
        Log("  [skip] %s: constant is %g, expected %g", name, original, expected);
        return false;
    }
    float* v = NewScaled(name, kind, original);
    if (!v) return false;
    int64_t disp = (uint8_t*)v - (insn + len);
    int32_t d = (int32_t)disp;
    bool ok = disp == d && WriteProtected(insn + dispOff, &d, 4);
    FlushInstructionCache(GetCurrentProcess(), insn, len);
    if (ok) RecordPatch(name, insn, len);
    Log("  [%s] %s at %p", ok ? "ok" : "FAIL", name, insn);
    return ok;
}

void ScaleConst(const char* name, const char* sig, size_t off, std::initializer_list<uint8_t> opcode, Scale kind,
                float expected, size_t dispOff, size_t len) {
    if (uint8_t* m = FindUnique(name, sig)) ScaleOperand(name, m + off, opcode.begin(), opcode.size(), dispOff, len, kind, expected);
}

void RetargetConst(const char* name, const char* sig, size_t off, std::initializer_list<uint8_t> opcode, size_t dispOff,
                   size_t len, const void* target) {
    if (uint8_t* m = FindUnique(name, sig)) RetargetRipOperand(name, m + off, dispOff, len, opcode, target);
}

// ---------------------------------------------------------------------------
// Tick gates and counters

bool GateOnTick(const char* name, uint8_t* fn, std::initializer_list<uint8_t> prologue) {
    size_t len = prologue.size();
    return InstallCave(name, fn, len, prologue, [&](Asm& a) {
        a.raw({0x83, 0x3D}).rip(&S->ticks60, 1).raw({0x00});  // cmp dword [ticks60], 0
        a.raw({0x7F, 0x01});                                   // jg run
        a.raw({0xC3});                                         // ret (skip this frame)
        for (uint8_t b : prologue) a.raw({b});                 // run: original prologue
        a.jmp(fn + len);
    });
}

bool GateCallOnTick(const char* name, uint8_t* site) {
    if (!site || site[0] != 0xE8) {
        Log("  [skip] %s: no call at site", name);
        return false;
    }
    uint8_t* callee = RipTarget(site, 1, 5);
    return InstallCaveUnchecked(name, site, 5, true, [&](Asm& a) {
        a.raw({0x83, 0x3D}).rip(&S->ticks60, 1).raw({0x00});  // cmp dword [ticks60], 0
        a.raw({0x7F, 0x01});                                   // jg run
        a.raw({0xC3});                                         // ret to the call site
        a.jmp(callee);                                         // run: tail-call, returns to the site
    });
}

void IncByTicks(const char* name, uint8_t* site, uint8_t modrm) {
    if (!site || site[0] != 0xFF || site[1] != modrm) {
        Log("  [skip] %s: unexpected bytes", name);
        return;
    }
    uint8_t disp[4];
    memcpy(disp, site + 2, 4);
    InstallCaveUnchecked(name, site, 6, false, [&](Asm& a) {
        a.raw({0x50});                                          // push rax
        a.raw({0x8B, 0x05}).rip(&S->ticks60);                   // mov eax, [ticks60]
        a.raw({0x01, modrm});                                   // add [reg+disp32], eax (inc's ModRM has reg field 0 too)
        a.raw({disp[0], disp[1], disp[2], disp[3]});
        a.raw({0x58});                                          // pop rax
        a.jmp(site + 6);
    });
}

void DecEaxByTicks(const char* name, uint8_t* site, uint8_t disp8) {
    InstallCave(name, site, 5, {0xFF, 0xC8, 0x89, 0x47, disp8}, [&](Asm& a) {
        a.raw({0x2B, 0x05}).rip(&S->ticks60);  // sub eax, [ticks60]
        a.raw({0x89, 0x47, disp8});            // mov [rdi+disp8], eax
        a.jmp(site + 5);
    });
}

// Saves flags and every volatile register (xmm0-5 at [rsp+0x20..0x80)); leaves rsp 16-byte aligned.
static void EmitSaveVolatiles(Asm& a, bool viaCall) {
    a.raw({0x9C});                                                                 // pushfq
    a.raw({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // push rax rcx rdx r8-r11
    a.raw({0x48, 0x81, 0xEC, (uint8_t)(viaCall ? 0x88 : 0x80), 0x00, 0x00, 0x00});  // sub rsp, frame
    for (uint8_t i = 0; i < 6; ++i) a.raw({0x0F, 0x11, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + i * 16)});  // movups [rsp+..], xmm0-5
}

static void EmitRestoreXmm(Asm& a, int keepXmm) {
    for (uint8_t i = 0; i < 6; ++i) {
        if (i == keepXmm) continue;
        a.raw({0x0F, 0x10, (uint8_t)(0x44 | i << 3), 0x24, (uint8_t)(0x20 + i * 16)});  // movups xmm0-5, [rsp+..]
    }
}

static void EmitPopVolatiles(Asm& a, bool viaCall) {
    a.raw({0x48, 0x81, 0xC4, (uint8_t)(viaCall ? 0x88 : 0x80), 0x00, 0x00, 0x00});  // add rsp, frame
    a.raw({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r11-r8 rdx rcx rax
    a.raw({0x9D});                                                                 // popfq
}

static void EmitRestoreVolatiles(Asm& a, bool viaCall, int keepXmm) {
    EmitRestoreXmm(a, keepXmm);
    EmitPopVolatiles(a, viaCall);
}

static void EmitCallAbs(Asm& a, const void* fn) {
    a.raw({0x48, 0xB8});  // mov rax, fn
    uint64_t f = (uint64_t)fn;
    for (int i = 0; i < 8; ++i) a.raw({(uint8_t)(f >> (8 * i))});
    a.raw({0xFF, 0xD0});  // call rax
}

void EmitFloatCall(Asm& a, float (*fn)(float), uint8_t in, uint8_t out, bool viaCall) {
    EmitSaveVolatiles(a, viaCall);
    a.raw({0x0F, 0x28, (uint8_t)(0xC0 | in)});        // movaps xmm0, xmm<in>
    EmitCallAbs(a, (const void*)fn);
    a.raw({0x0F, 0x28, (uint8_t)(0xC0 | out << 3)});  // movaps xmm<out>, xmm0
    EmitRestoreVolatiles(a, viaCall, out);
}

void EmitLerpMulssMem(Asm& a, const uint8_t* orig, size_t len) {
    // orig: F3 [REX] 0F 59 ModRM disp8/disp32 (no SIB; base register is not rsp).
    bool rex = (orig[1] & 0xF0) == 0x40;
    uint8_t rexByte = rex ? orig[1] : 0;
    const uint8_t* op = orig + (rex ? 2 : 1);  // 0F 59 ModRM ...
    uint8_t modrm = op[2];
    uint8_t dst = (uint8_t)(((rexByte & 4) ? 8 : 0) | ((modrm >> 3) & 7));
    size_t memLen = len - (size_t)(op + 3 - orig);  // displacement bytes
    EmitSaveVolatiles(a, false);
    // movss xmm0, [reg+disp] (same base/displacement, destination xmm0)
    a.raw({0xF3});
    if (rexByte & 0x0B) a.raw({(uint8_t)(0x40 | (rexByte & 0x0B))});
    a.raw({0x0F, 0x10, (uint8_t)(modrm & 0xC7)});
    for (size_t i = 0; i < memLen; ++i) a.raw({op[3 + i]});
    EmitCallAbs(a, (const void*)&LerpE);
    a.raw({0xF3, 0x0F, 0x11, 0x44, 0x24, 0x18});  // movss [rsp+0x18], xmm0 (rescaled factor)
    EmitRestoreXmm(a, -1);
    a.raw({0xF3});
    if (dst >= 8) a.raw({0x44});
    a.raw({0x0F, 0x59, (uint8_t)(0x44 | (dst & 7) << 3), 0x24, 0x18});  // mulss xD, [rsp+0x18] (flags untouched)
    EmitPopVolatiles(a, false);
}

const float* NearOne() {
    static float* one;
    if (!one && (one = (float*)g_arena.Alloc(4, 4))) *one = 1.0f;
    return one;
}

uint8_t* FindEither(const char* name, const char* sig, const char* sigAlt) {
    if (uint8_t* m = FindUnique(name, sig)) return m;
    return sigAlt ? FindUnique(name, sigAlt) : nullptr;
}

float PowE(float x) {
    float e = S->stepUpToOne;
    return e < 1.0f ? powf(x, e) : x;
}

float LerpE(float x) {
    float e = S->stepUpToOne;
    return e < 1.0f ? 1.0f - powf(1.0f - x, e) : x;
}

// ---------------------------------------------------------------------------
// Generated tables

void InstallTable(const ScaledConstPatch* rows, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const ScaledConstPatch& r = rows[i];
        if (uint8_t* m = FindEither(r.name, r.sig, r.sigAlt))
            ScaleOperand(r.name, m + r.off, r.opcode, r.opLen, r.dispOff, r.len, r.kind, r.base);
    }
}

// Double-precision constants (a separate small registry; same formulas).
struct ScaledDoubleEntry {
    Scale kind;
    double base;
    double* value;
};
static ScaledDoubleEntry g_doubles[16];
static size_t g_doubleCount;

static void UpdateScaledDoubles(float e) {
    for (size_t i = 0; i < g_doubleCount; ++i) {
        ScaledDoubleEntry& d = g_doubles[i];
        double v = d.base;
        if (e < 1.0f) {
            if (d.kind == Scale::Pow) v = pow(d.base, (double)e);
            else if (d.kind == Scale::Lin) v = d.base * e;
            else if (d.kind == Scale::Lerp) v = 1.0 - pow(1.0 - d.base, (double)e);
        }
        *d.value = v;
    }
}

void InstallTable(const ScaledDoublePatch* rows, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const ScaledDoublePatch& r = rows[i];
        uint8_t* m = FindEither(r.name, r.sig, r.sigAlt);
        if (!m) continue;
        uint8_t* insn = m + r.off;
        if (memcmp(insn, r.opcode, r.opLen) != 0 || g_doubleCount >= _countof(g_doubles)) {
            Log("  [skip] %s: unexpected bytes", r.name);
            continue;
        }
        double original = *(double*)RipTarget(insn, r.dispOff, r.len);
        if (fabs(original - r.base) > fabs(r.base) * 1e-6) {
            Log("  [skip] %s: constant is %g, expected %g", r.name, original, r.base);
            continue;
        }
        double* v = (double*)g_arena.Alloc(8, 8);
        if (!v) continue;
        *v = original;
        g_doubles[g_doubleCount++] = {r.kind, original, v};
        int32_t d = (int32_t)((uint8_t*)v - (insn + r.len));
        bool ok = WriteProtected(insn + r.dispOff, &d, 4);
        FlushInstructionCache(GetCurrentProcess(), insn, r.len);
        if (ok) RecordPatch(r.name, insn, r.len);
        Log("  [%s] %s at %p", ok ? "ok" : "FAIL", r.name, insn);
    }
}

void InstallTable(const ArgCallPatch* rows, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const ArgCallPatch& r = rows[i];
        uint8_t* m = FindEither(r.name, r.sig, r.sigAlt);
        if (!m) continue;
        uint8_t* site = m + r.off;
        if (site[0] != 0xE8 || r.reg > 7) {
            Log("  [skip] %s: no call at site", r.name);
            continue;
        }
        uint8_t* callee = RipTarget(site, 1, 5);
        if (r.kind == Scale::Lerp) {
            // The argument is a per-frame smoothing factor: x -> 1-(1-x)^e.
            InstallCaveUnchecked(r.name, site, 5, true, [&](Asm& a) {
                EmitFloatCall(a, &LerpE, r.reg, r.reg, true);
                a.jmp(callee);
            });
            continue;
        }
        float* k = NewScaled(r.name, r.kind, r.base);
        InstallCaveUnchecked(r.name, site, 5, true, [&](Asm& a) {
            a.raw({0xF3, 0x0F, 0x59, (uint8_t)(0x05 | r.reg << 3)}).rip(k);  // mulss xmm<reg>, [k]
            a.jmp(callee);                                                  // tail-call the original
        });
    }
}

void InstallTable(const VecCallPatch* rows, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const VecCallPatch& r = rows[i];
        uint8_t* m = FindEither(r.name, r.sig, r.sigAlt);
        if (!m) continue;
        uint8_t* site = m + r.off;
        if (site[0] != 0xE8) {
            Log("  [skip] %s: no call at site", r.name);
            continue;
        }
        uint8_t* callee = RipTarget(site, 1, 5);
        InstallCaveUnchecked(r.name, site, 5, true, [&](Asm& a) {
            a.raw({0x48, 0x83, 0xEC, 0x28});                      // sub rsp, 0x28
            a.raw({0x48, 0x89, 0x4C, 0x24, 0x20});                // mov [rsp+0x20], rcx
            a.call(callee);                                       // original: [rcx] = vector * matrix
            a.raw({0x48, 0x8B, 0x4C, 0x24, 0x20});                // mov rcx, [rsp+0x20]
            a.raw({0x0F, 0x10, 0x01});                            // movups xmm0, [rcx]
            a.raw({0x44, 0x8B, 0x51, 0x0C});                      // mov r10d, [rcx+12] (w as the game set it)
            a.raw({0xF3, 0x0F, 0x10, 0x0D}).rip(&S->stepUpToOne);  // movss xmm1, [e]
            a.raw({0x0F, 0xC6, 0xC9, 0x00});                      // shufps xmm1, xmm1, 0
            a.raw({0x0F, 0x59, 0xC1});                            // mulps xmm0, xmm1
            a.raw({0x0F, 0x11, 0x01});                            // movups [rcx], xmm0
            a.raw({0x44, 0x89, 0x51, 0x0C});                      // mov [rcx+12], r10d
            a.raw({0x48, 0x83, 0xC4, 0x28});                      // add rsp, 0x28
            a.raw({0xC3});                                        // ret
        });
    }
}

void DecEaxByTicks(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0x2B, 0x05}).rip(&S->ticks60);  // sub eax, [ticks60]
        a.copy(site + 2, s.len - 2);
        a.jmp(site + s.len);
    });
}

uint8_t* SiteOf(const SigSite& s) {
    uint8_t* m = FindEither(s.name, s.sig, s.sigAlt);
    if (!m) return nullptr;
    uint8_t* site = m + s.off;
    for (int i = 0; i < s.len; ++i) {
        if ((s.cmpMask >> i & 1ull) && site[i] != s.orig[i]) {
            Log("  [skip] %s: unexpected bytes", s.name);
            return nullptr;
        }
    }
    return site;
}
