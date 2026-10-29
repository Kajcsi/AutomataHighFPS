// Hacking minigame and the Ending E staff-roll shooter (same engine). Ship and
// enemy speed smoothing, movement, spins, wave patterns, gun sweeps and several
// per-frame cooldowns are rescaled to the frame's share of a 60 Hz frame.
// Sites come from analysis/hacking_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/hacking.inc"

// `mulss xD, [reg+disp]` with a per-object smoothing factor -> xD *= 1-(1-factor)^e.
static void LerpObjectFactor(const SigSite& s) {
    const uint8_t* op = s.orig + ((s.orig[1] & 0xF0) == 0x40 ? 2 : 1);
    uint8_t modrm = op[2];
    if ((modrm & 7) == 4 || (modrm >> 6) == 0) {
        Log("  [skip] %s: unsupported operand", s.name);
        return;
    }
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        EmitLerpMulssMem(a, s.orig, s.len);
        a.jmp(site + s.len);
    });
}

// `dec dword [rdi+0x128FC]` (respawn / restart wait in frames) -> subtract ticks60.
static void RespawnWait(const SigSite& s) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        a.raw({0x50});                                        // push rax
        a.raw({0x8B, 0x05}).rip(&S->ticks60);                 // mov eax, [ticks60]
        a.raw({0x29, 0x87, 0xFC, 0x28, 0x01, 0x00});          // sub [rdi+0x128FC], eax
        a.raw({0x58});                                        // pop rax
        a.jmp(site + 6);
    });
}

// Replays the original instructions, then multiplies xmm<reg> by e (flags untouched).
static void ThenScale(const SigSite& s, uint8_t reg) {
    PatchSite(s, [&](Asm& a, uint8_t* site) {
        for (int i = 0; i < s.len; ++i) a.raw({site[i]});
        a.raw({0xF3, 0x0F, 0x59, (uint8_t)(0x05 | reg << 3)}).rip(&S->stepUpToOne);  // mulss xmm<reg>, [e]
        a.jmp(site + s.len);
    });
}

void FixHacking() {
    Log("hacking:");
    InstallTable(kHackScaledConsts, _countof(kHackScaledConsts));
    InstallTable(kHackScaledDoubles, _countof(kHackScaledDoubles));
    InstallTable(kHackArgCalls, _countof(kHackArgCalls));

    for (const SigSite* s : {&kHack_SHIP_LERP_A, &kHack_SHIP_LERP_B, &kHack_ROLL_LERP, &kHack_EM_SPDLERP_E1,
                             &kHack_EM_SPDLERP_E2, &kHack_EM_SPDLERP_E3, &kHack_EM_SPDLERP_E4, &kHack_EM_SPDLERP_E5,
                             &kHack_EM_SPDLERP_E6, &kHack_EM_SPDLERP_E7, &kHack_EM_SPDLERP_E8, &kHack_EM_SPDLERP_E9,
                             &kHack_EM_SPDLERP_E10})
        LerpObjectFactor(*s);
    RespawnWait(kHack_SHIP_RESPAWN1);
    RespawnWait(kHack_SHIP_RESPAWN2);
    RespawnWait(kHack_ROLL_RESPAWN);
    // Et0024 rotation: angle += speed * rate per frame (flags are live after the site).
    ThenScale(kHack_ET0024_ROT, 0);
    // Gun sweep (case 3): angle step = rate * deg2rad * dir per frame.
    ThenScale(kHack_GUN_SWEEP_C3, 2);

    // Gun roll (case 4): target = angle + rate*deg2rad, then an approach helper that
    // snaps when the factor (xmm8) is >= 1, i.e. a full per-frame step. In that case
    // step by rate*deg2rad*e, clamped to the per-frame maximum (xmm7) * e, and leave
    // through the function's epilogue like the helper call does.
    if (uint8_t* site = SiteOf(kHack_GUN_ROLL_C4)) {
        uint8_t* exitJmp = site + 0x2D;  // `jmp epilogue` after the helper call
        if (exitJmp[0] != 0xE9) {
            Log("  [skip] %s: unexpected bytes", kHack_GUN_ROLL_C4.name);
        } else {
            uint8_t* epilogue = RipTarget(exitJmp, 1, 5);
            InstallCaveUnchecked(kHack_GUN_ROLL_C4.name, site, 8, false, [&](Asm& a) {
                a.raw({0xF3, 0x0F, 0x10, 0x93, 0x90, 0x14, 0x00, 0x00});         // movss xmm2, [rbx+0x1490]
                a.raw({0x81, 0x3D}).rip(&S->stepUpToOne, 4).i32(0x3F800000);     // cmp dword [e], 1.0
                uint8_t* je = a.cur();
                a.raw({0x74, 0x00});                                              // je original
                a.raw({0x44, 0x0F, 0x2F, 0x05}).rip(NearOne());                   // comiss xmm8, [1.0]
                uint8_t* jb = a.cur();
                a.raw({0x72, 0x00});                                              // jb original
                a.raw({0xF3, 0x0F, 0x59, 0xD6});                                  // mulss xmm2, xmm6
                a.raw({0xF3, 0x0F, 0x59, 0x15}).rip(&S->stepUpToOne);             // mulss xmm2, [e]
                a.raw({0x0F, 0x28, 0xC7});                                        // movaps xmm0, xmm7
                a.raw({0xF3, 0x0F, 0x59, 0x05}).rip(&S->stepUpToOne);             // mulss xmm0, [e]
                a.raw({0xF3, 0x0F, 0x5D, 0xD0});                                  // minss xmm2, xmm0
                a.raw({0x0F, 0x57, 0xC9});                                        // xorps xmm1, xmm1
                a.raw({0xF3, 0x0F, 0x5C, 0xC8});                                  // subss xmm1, xmm0
                a.raw({0xF3, 0x0F, 0x5F, 0xD1});                                  // maxss xmm2, xmm1
                a.raw({0xF3, 0x0F, 0x58, 0x93, 0x14, 0x37, 0x00, 0x00});         // addss xmm2, [rbx+0x3714]
                a.raw({0xF3, 0x0F, 0x11, 0x93, 0x14, 0x37, 0x00, 0x00});         // movss [rbx+0x3714], xmm2
                a.jmp(epilogue);
                uint8_t* original = a.cur();
                je[1] = (uint8_t)(original - (je + 2));
                jb[1] = (uint8_t)(original - (jb + 2));
                a.jmp(site + 8);                                                  // original path
            });
        }
    }
}
