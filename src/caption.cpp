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

    // Letterbox close without the event flag: a caption or phase event that opened with a
    // hold ([0x2D7C]) closes down the plain counter ([0x2D78]). With no plain open to match,
    // release one hold, so an event ending later does not keep the letterbox for a caption
    // that already closed. Orders that never reach this path are unchanged.
    PatchSite(kCp_CP_LetterboxCaptionRelease, [](Asm& a, uint8_t* site) {
        a.copy(site, 7);                                            // sub dword [rcx+0x2D78], 1
        uint8_t* plain = a.jccForward(0x9);                         // jns: a plain open matched
        a.raw({0x83, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0x00});          // cmp dword [rcx+0x2D7C], 0
        uint8_t* none = a.jccForward(0xE);                          // jle
        a.raw({0xFF, 0x89, 0x7C, 0x2D, 0x00, 0x00});                // dec dword [rcx+0x2D7C]
        a.bind(none);
        a.jmp(site + 15);                                           // original path: no plain hold
        a.bind(plain);
        a.copy(site + 7, 6);                                        // mov eax, [rcx+0x2D78]
        a.jmp(site + 15 + (int8_t)site[14]);                        // the jns target
    });
}
