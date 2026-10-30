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

#include <cmath>
#include <cstdio>
#include <cstring>

// Push log: while the player is in the push state (0x3D), each frame's substate, the
// player's displacement, the block check and the block position are written to the log
// (first kMaxPushLines frames of a session), so a block that does not move can be traced
// from the log of a normal play session.
namespace {

using PushHandler = void (*)(uint8_t* player);
using VecLength = float (*)(const float* v);
using BlockCheck = uint32_t (*)(uint8_t* block, const float* delta, int pushing);
PushHandler g_pushHandler;
VecLength g_vecLength;
BlockCheck g_blockCheck;

const int kMaxPushLines = 3000;
int g_pushLines;
struct {
    bool measured, checked;
    float raw[3], len;
    uint32_t result;
    float delta[3], block[3];
} g_push;
uint8_t* g_block;  // the block checked this frame (only valid during that frame)

float PushDeltaLogged(const float* v) {
    float len = g_vecLength(v);
    if (!g_push.measured) {
        g_push.measured = true;
        memcpy(g_push.raw, v, 12);
        g_push.len = len;
    }
    return len;
}

uint32_t BlockCheckLogged(uint8_t* block, const float* delta, int pushing) {
    memcpy(g_push.block, block + 0x50, 12);
    uint32_t r = g_blockCheck(block, delta, pushing);
    g_block = block;
    g_push.checked = true;
    g_push.result = r;
    memcpy(g_push.delta, delta, 12);
    return r;
}

void PushStateLogged(uint8_t* pl) {
    if (g_pushLines >= kMaxPushLines) {
        g_pushHandler(pl);
        return;
    }
    float p0[3], p1[3], b1[3] = {};
    memcpy(p0, pl + 0x50, 12);
    g_block = nullptr;
    int sub = *(int*)(pl + 0x674), sub2 = *(int*)(pl + 0x678);
    memset(&g_push, 0, sizeof g_push);
    g_pushHandler(pl);
    memcpy(p1, pl + 0x50, 12);
    if (g_block) memcpy(b1, g_block + 0x50, 12);
    const float* b0 = g_push.block;
    char check[96] = "-";
    if (g_push.checked)
        snprintf(check, sizeof check, "%s (%.6f, %.6f)", g_push.result ? "blocked" : "ok", g_push.delta[0], g_push.delta[2]);
    Log("push %d.%d->%d.%d e %.4f tick %d | raw %s %.6f (%.6f, %.6f) | check %s | 2B moved %.6f | block %.4f %.4f %.4f -> %.4f %.4f %.4f",
        sub, sub2, *(int*)(pl + 0x674), *(int*)(pl + 0x678), S->stepUpToOne, S->ticks60, g_push.measured ? "yes" : "no",
        g_push.len, g_push.raw[0], g_push.raw[2], check, hypotf(p1[0] - p0[0], p1[2] - p0[2]), b0[0], b0[1], b0[2], b1[0],
        b1[1], b1[2]);
    if (++g_pushLines == kMaxPushLines) Log("push log: limit reached");
}

// Routes a `call rel32` site to `target` (a function with the callee's signature) and
// returns the original callee.
void* RouteCall(const SigSite& s, const void* target) {
    uint8_t* site = SiteOf(s);
    if (!site || site[0] != 0xE8) return nullptr;
    void* callee = RipTarget(site, 1, 5);
    uint8_t* thunk = g_arena.Thunk(target);
    if (!thunk || !InstallCaveUnchecked(s.name, site, 5, true, [&](Asm& a) { a.jmp(thunk); })) return nullptr;
    return callee;
}

void InstallPushLog() {
    g_pushHandler = (PushHandler)RouteCall(kPb_PB_LogPushState, (const void*)&PushStateLogged);
    g_vecLength = (VecLength)RouteCall(kPb_PB_LogPushDelta, (const void*)&PushDeltaLogged);
    g_blockCheck = (BlockCheck)RouteCall(kPb_PB_LogPushCheck, (const void*)&BlockCheckLogged);
}

}  // namespace

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

    InstallPushLog();
}
