// Inputs to the animation system. Playback itself advances by the entity's frame
// delta (seconds), blends progress linearly in time and events fire on time
// crossings, so animations play at the right speed at any frame rate. What was
// frame-dependent is code that smooths or ramps the playback speed, blend
// parameters and root-motion scale once per update: player run/turn states and an
// escort blend ramp, the Baa000 mount's speed ramp, and the shared enemy turn
// states' blend, speed and root-scale smoothing.
// Sites come from analysis/anim_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "game.h"

#include "generated/anim.inc"

// The site starts with `<op>ss xD, xS` (F3 [REX] 0F op ModRM) where xS holds a per-update
// constant also used elsewhere: that one instruction takes the scaled constant instead.
static void SwapFactor(const SigSite& s, const float* k) {
    bool rex = (s.orig[1] & 0xF0) == 0x40;
    const uint8_t* op = s.orig + (rex ? 2 : 1);  // 0F op ModRM
    if (s.orig[0] != 0xF3 || op[0] != 0x0F || op[2] < 0xC0) {
        Log("  [skip] %s: not a register SSE operation", s.name);
        return;
    }
    uint8_t dst = (uint8_t)(((rex && (s.orig[1] & 4)) ? 8 : 0) | ((op[2] >> 3) & 7));
    RewriteSite(s, {{0, (uint8_t)(rex ? 5 : 4), op[1], dst, k}});
}

// ---------------------------------------------------------------------------
// Diagnostics ([Debug] JumpLog=1): every frame the player is in the jump (5) or fall (6)
// state, log the time, step, state, phase (it advances when a motion ends) and height.

static uint8_t** g_jumper;  // arena slot, set by the motion-update cave while in state 5/6
static uint8_t* g_lastJumper;
static LARGE_INTEGER g_jumpStart;

void DebugJumpFrame() {
    if (!g_jumper) return;
    uint8_t* p = *g_jumper;
    *g_jumper = nullptr;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    __try {
        if (p) {
            if (!g_lastJumper) g_jumpStart = now;
            g_lastJumper = p;
        } else if (g_lastJumper) {
            p = g_lastJumper;  // first frame after the jump/fall states: log once more, then stop
            g_lastJumper = nullptr;
        } else {
            return;
        }
        double t = (double)(now.QuadPart - g_jumpStart.QuadPart) / (double)freq.QuadPart;
        Log("jump t=%.4f step=%.4f state=%d phase=%d y=%.4f timer1A78=%.4f", t, S->step, *(int*)(p + 0x670),
            *(int*)(p + 0x674), *(float*)(p + 0x54), *(float*)(p + 0x1A78));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastJumper = nullptr;
    }
}

static void InstallJumpLogger() {
    uint8_t* fn = FindUnique("player motion update",
                             "48 8B C4 53 41 54 48 81 EC A8 00 00 00 0F 29 78 C8 48 8B D9 44 0F 29 40 B8 0F 28 FA");
    if (!fn || !(g_jumper = (uint8_t**)g_arena.Alloc(8, 8))) return;
    InstallCaveUnchecked("jump logger", fn, 13, false, [&](Asm& a) {
        a.raw({0x81, 0xB9, 0x70, 0x06, 0x00, 0x00}).i32(5);         // cmp dword [rcx+0x670], 5
        a.raw({0x74, 0x0C});                                        // je store
        a.raw({0x81, 0xB9, 0x70, 0x06, 0x00, 0x00}).i32(6);         // cmp dword [rcx+0x670], 6
        a.raw({0x75, 0x07});                                        // jne skip
        a.raw({0x48, 0x89, 0x0D}).rip(g_jumper);                   // store: mov [jumper], rcx
        a.copy(fn, 13);                                             // skip: mov rax, rsp; push rbx; push r12; sub rsp, 0xA8
        a.jmp(fn + 13);
    });
}

void FixAnimationInputs() {
    InstallJumpLogger();  // cheap; logging itself follows [Debug] JumpLog at runtime
    Log("animation inputs:");
    InstallTable(kAnScaledConsts, _countof(kAnScaledConsts));
    InstallTable(kAnScaledDoubles, _countof(kAnScaledDoubles));

    const float* lerp01 = NewScaled("animation smoothing 0.1", Scale::Lerp, 0.1f);
    const float* lerp001 = NewScaled("animation smoothing 0.01", Scale::Lerp, 0.01f);
    const float* step01 = NewScaled("animation blend ramp 0.1", Scale::Lin, 0.1f);
    for (const SigSite* s : {&kAn_AN_Pl_St02_BlendX1480_Lerp, &kAn_AN_Pl_St02_Speed1A90_Lerp, &kAn_AN_Pl_St02_RootXZ1A7C_Lerp,
                             &kAn_AN_Em_TurnBlendC90_A1, &kAn_AN_Em_TurnBlendC90_A2, &kAn_AN_Em_TurnBlendC90_A3,
                             &kAn_AN_Em_TurnBlendC90_A4, &kAn_AN_Em_TurnBlendC90_B1, &kAn_AN_Em_TurnBlendC90_B2,
                             &kAn_AN_Em_TurnBlendC90_B3, &kAn_AN_Em_TurnBlendC90_B4, &kAn_AN_Em_TurnBlendC90_C1,
                             &kAn_AN_Em_BlendC90_X10_A, &kAn_AN_Em_BlendC98_X10_A, &kAn_AN_Em_BlendC90_X10_B,
                             &kAn_AN_Em_BlendC98_X10_B})
        SwapFactor(*s, lerp01);
    SwapFactor(kAn_AN_Pl_St3A_Blend1488_Lerp, lerp001);
    SwapFactor(kAn_AN_Pl_St4A_SpeedMul1A90_Lerp, lerp001);
    for (const SigSite* s : {&kAn_AN_Pl_EscortBlend1A74_DecA, &kAn_AN_Pl_EscortBlend1A74_IncA,
                             &kAn_AN_Pl_EscortBlend1A74_DecB, &kAn_AN_Pl_EscortBlend1A74_IncB})
        SwapFactor(*s, step01);
    // Player state 0x3A playback speed: v += (t - v) * xmm9, where xmm9 is 0.01, 0.02 or a
    // value derived from the blend; xmm1 is free here.
    PatchSite(kAn_AN_Pl_St3A_Speed1A7C_Lerp, [](Asm& a, uint8_t* site) {
        a.raw({0x41, 0x0F, 0x28, 0xC9});                            // movaps xmm1, xmm9
        EmitFloatCall(a, &LerpE, 1, 1);
        a.raw({0xF3, 0x0F, 0x59, 0xF9});                            // mulss xmm7, xmm1
        a.jmp(site + 5);
    });
}
