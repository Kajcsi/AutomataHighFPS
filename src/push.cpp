// Pushable blocks (BehaviorMovableBlock, the bg20a* cubes) and the interaction manager that
// starts a push. The push itself moves the block by the player's root motion each frame,
// which already follows the frame time; what did not: the block's fall off a ledge (position
// integrated once per frame), its pull back toward its area (5% per frame) and the
// interaction cooldowns (1/60 s per frame). Every cave runs the original instructions at e = 1.
// Sites come from analysis/push_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/push.inc"

void FixPushBlocks() {
    Log("pushable blocks:");
    InstallTable(kPbScaledConsts, _countof(kPbScaledConsts));

    // Fall: vy -= g (g = d/300 in xmm0, already per frame time), then y += vy per frame.
    // Exact against 60 Hz ticks: y += e*vy - g*(1-e)/2.
    const float* half = ConstFloat(0.5f);
    const float* e = &S->stepUpToOne;
    PatchSite(kPb_PB_BlockFallExact, [&](Asm& a, uint8_t* site) {
        CmpE1(a);
        uint8_t* v = a.jccForward(0x4);
        Movaps(a, 2, 0);                                            // g
        a.copy(site, 24);                                           // vy -= g; ground y; store vy
        EmitSseRip(a, kMulss, 1, e);
        EmitSseRip(a, kMovss, 3, NearOne());
        EmitSseRip(a, kSubss, 3, e);
        EmitSseRip(a, kMulss, 3, half);
        SseRR(a, kMulss, 3, 2);
        SseRR(a, kSubss, 1, 3);                                     // e*vy - g*(1-e)/2
        a.copy(site + 24, 10);                                      // y += step; store y
        a.jmp(site + 34);
        a.bind(v);
        a.copy(site, 34);
        a.jmp(site + 34);
    });
}
