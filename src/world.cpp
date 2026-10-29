// Per-frame constants found by sweeping the whole executable for frame-step idioms
// (lerps, decays, ramps and countdowns without the frame delta), outside the
// systems that have their own file: ambient audio smoothing, effect particles,
// foot IK, map gimmick timers and fades, item lifetimes, creature NPC states,
// player animation blends and scripted states, and Ending E credit enemies.
// Sites come from analysis/sweep1_patches.tsv via tools/gen_patch_tables.py.
#include "subsystems.h"
#include "patchkit.h"
#include "timing.h"

#include "generated/sweep1.inc"

void FixWorld() {
    Log("world:");
    InstallTable(kSw1ScaledConsts, _countof(kSw1ScaledConsts));
    InstallTable(kSw1ArgCalls, _countof(kSw1ArgCalls));
    InstallTable(kSw1VecCalls, _countof(kSw1VecCalls));
    // Flight-unit attach state: 5-frame delay counter.
    DecEaxByTicks(kSw1_SW1_PlFU_DelayCtr);
}
