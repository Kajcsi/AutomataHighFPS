// Pushable blocks (BehaviorMovableBlock, the bg20a* cubes) and the interaction manager that
// starts a push. Each frame of a push the player's state moves the block by the player's
// root motion and flags it as moved; the block's own update then rebuilds its bones and
// collision. Fixed here: the moved flag was cleared every frame by the block's pre-update,
// so a push the block's update did not see in the same frame was never applied; the
// per-frame 0.0001 displacement threshold (a speed threshold that rose with the frame rate);
// the block's fall off a ledge (position integrated once per frame); its pull back toward
// its area (5% per frame) and the interaction cooldowns (1/60 s per frame).
// Sites come from analysis/push_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/push.inc"

void FixPushBlocks() {
    Log("pushable blocks:");
    InstallTable(kPbScaledConsts, _countof(kPbScaledConsts));

    // Moved flag [0x978]: kept until the block update applies it, then cleared there.
    PatchSite(kPb_PB_BlockMovedKeep, [](Asm& a, uint8_t* site) { a.jmp(site + 10); });
    PatchSite(kPb_PB_BlockMovedConsume, [](Asm& a, uint8_t* site) {
        a.copy(site, 7);                                            // cmp dword [rdi+0x978], 0
        uint8_t* done = a.jccForward(0x4);
        a.raw({0xC7, 0x87, 0x78, 0x09, 0x00, 0x00}).i32(0);         // mov dword [rdi+0x978], 0 (ZF stays 0)
        a.bind(done);
        a.jmp(site + 7);
    });

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
