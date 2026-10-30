// Script captions (Caption@hap): a caption opens the letterbox and sets the control lock
// (0x2000 in the global flags). An auto-ending caption switches to DONE on its own once
// its gate clears, without closing what it opened; the script's end command only closes a
// caption that is still ACTIVE. When the auto end came first the letterbox and the lock
// stayed on after the cutscene. The auto end now closes it like the end command does.
// Sites come from analysis/caption_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "game.h"

#include "generated/caption.inc"

void FixCaptionLetterbox() {
    Log("caption letterbox:");
    // Caption helper: if the caption is showing, request the letterbox close, clear the lock.
    uint8_t* close = FindUnique("CaptionClose", "40 53 48 83 EC 20 80 79 49 00 48 8B D9 74 ?? 45 33 C0");
    if (!close) return;
    PatchSite(kCp_CP_CaptionAutoEndClose, [&](Asm& a, uint8_t* site) {
        a.raw({0x48, 0x8B, 0xCB});                                  // mov rcx, rbx (the caption)
        a.call(close);
        a.raw({0x48, 0x8B, 0xCB});                                  // mov rcx, rbx
        a.copy(site, 8);                                            // xor r8d, r8d; mov edx, DONE
        a.jmp(site + 8);
    });
}
