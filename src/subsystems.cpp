#include "subsystems.h"
#include "patchkit.h"
#include "game.h"
#include "timing.h"
#include <cmath>

// Each fix below makes a frame-counted system advance at 60 Hz (or scales its
// per-frame amount by the time step) so it behaves the same at any frame rate,
// and leaves vanilla behaviour untouched at 60 fps and below.
// Addresses in comments are for the Steam NieRAutomata.exe.

static const std::initializer_list<uint8_t> kMulss0 = {0xF3, 0x0F, 0x59, 0x05};  // mulss xmm0, [rip+d]
static const std::initializer_list<uint8_t> kMulss6 = {0xF3, 0x0F, 0x59, 0x35};  // mulss xmm6, [rip+d]
static const std::initializer_list<uint8_t> kSubss0 = {0xF3, 0x0F, 0x5C, 0x05};  // subss xmm0, [rip+d]
static const std::initializer_list<uint8_t> kAddss0 = {0xF3, 0x0F, 0x58, 0x05};  // addss xmm0, [rip+d]

// ---------------------------------------------------------------------------
// Menus and UI

static void FixUi() {
    // Held-direction auto-repeat counters (10-frame delay, 3-frame interval) for
    // pad, keyboard and mouse: `inc counter` -> `counter += ticks60`.
    if (uint8_t* m = FindUnique("PadRepeat",
            "8B 4B 30 8B 05 ?? ?? ?? ?? FF C1 89 4B 30 3B C8 76 17 44 8B 05 ?? ?? ?? ?? 41 03 C0 3B C8 72 09 41 2B C8 89 53 2C 89 4B 30")) {
        InstallCave("menu repeat (pad)", m + 9, 5, {0xFF, 0xC1, 0x89, 0x4B, 0x30}, [&](Asm& a) {
            a.raw({0x03, 0x0D}).rip(&S->ticks60);  // add ecx, [ticks60]
            a.raw({0x89, 0x4B, 0x30});             // mov [rbx+0x30], ecx
            a.jmp(m + 14);
        });
    }
    if (uint8_t* m = FindUnique("KbdRepeat",
            "8B 0D ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? FF C1 89 0D ?? ?? ?? ?? 3B C8 76 ?? 8B 15 ?? ?? ?? ?? 03 C2 3B C8 72 ?? 2B CA 89 0D")) {
        uint8_t* counter = RipTarget(m + 14, 2, 6);
        InstallCaveUnchecked("menu repeat (keyboard)", m + 12, 8, false, [&](Asm& a) {
            a.raw({0x03, 0x0D}).rip(&S->ticks60);  // add ecx, [ticks60]
            a.raw({0x89, 0x0D}).rip(counter);      // mov [counter], ecx
            a.jmp(m + 20);
        });
    }
    if (uint8_t* m = FindUnique("MouseRepeat",
            "8B 15 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? FF C2 89 15 ?? ?? ?? ?? 3B D0 76 ?? 44 8B 15 ?? ?? ?? ?? 41 8D 0C 02")) {
        uint8_t* counter = RipTarget(m + 15, 2, 6);
        InstallCaveUnchecked("menu repeat (mouse)", m + 13, 8, false, [&](Asm& a) {
            a.raw({0x03, 0x15}).rip(&S->ticks60);  // add edx, [ticks60]
            a.raw({0x89, 0x15}).rip(counter);      // mov [counter], edx
            a.jmp(m + 21);
        });
    }

    // HUD/UI alpha fades: a += (t - a) * 0.1 per frame.
    ScaleConst("UI fade (UIAllPurpose)", "F3 0F 5C F0 48 8D 4B 40 F3 0F 59 35 ?? ?? ?? ?? F3 0F 58 F0 F3 0F 11 73 74",
               8, kMulss6, Scale::Lerp, 0.1f);
    ScaleConst("UI fade (UIAppWorkBase)",
               "F3 0F 5C F0 48 89 7C 24 40 48 8D 4B 40 F3 0F 59 35 ?? ?? ?? ?? F3 0F 58 F0 F3 0F 11 73 74", 13, kMulss6,
               Scale::Lerp, 0.1f);

    // Long option/pulldown text marquee: 15-frame hold, then +2 px per frame.
    // The functions only update state and offsets, so running them on 60 Hz ticks is exact.
    static const std::initializer_list<uint8_t> kMarqueePrologue = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
    const char* kMarqueeTail = "F3 0F 11 83 98 00 00 00 41 0F 2F C0 76 11 0F 57 35 ?? ?? ?? ?? F3 0F 11 B3 98 00 00 00 EB 03 0F 28 F0 48 8D 4B 40 E8 ?? ?? ?? ?? 85 C0 75 20 8D 50";
    char sig[512];
    wsprintfA(sig, "F3 0F 10 83 98 00 00 00 F3 0F 58 05 ?? ?? ?? ?? %s 0E", kMarqueeTail);
    if (uint8_t* m = FindUnique("MarqueeL", sig)) GateOnTick("option text marquee (left)", m - 0x21B, kMarqueePrologue);
    wsprintfA(sig, "F3 0F 10 83 98 00 00 00 F3 0F 58 05 ?? ?? ?? ?? %s 10", kMarqueeTail);
    if (uint8_t* m = FindUnique("MarqueeR", sig)) GateOnTick("option text marquee (right)", m - 0x21B, kMarqueePrologue);
    if (uint8_t* m = FindUnique("MarqueeOpt", "F3 0F 10 83 B8 00 00 00 F3 0F 58 05 ?? ?? ?? ?? F3 0F 11 83 B8 00 00 00 0F 2F C7 76 11"))
        GateOnTick("option text marquee (item head)", m - 0x165, kMarqueePrologue);

    // Controller rumble envelope counts down per frame.
    if (uint8_t* m = FindUnique("Rumble", "44 8B 05 ?? ?? ?? ?? 48 8D 54 24 30 48 8B CB E8 ?? ?? ?? ?? 8B 43 7C 89 44 24 44"))
        GateCallOnTick("rumble envelope", m + 15);
}

// ---------------------------------------------------------------------------
// Overworld 9S hack-charge gauge: drains 1/360 per frame.

static void FixHackGauge() {
    ScaleConst("9S hack gauge decay", "F3 0F 10 87 84 6D 01 00 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 87 84 6D 01 00", 8, kSubss0,
               Scale::Lin, 1.0f / 360.0f);
}

// ---------------------------------------------------------------------------
// Quests, events, dialogue and world objects

static void FixQuests() {
    // Timers that subtract a fixed 1/60 s per update -> subtract min(step,1)/60.
    const void* fs = &S->frameSec;
    RetargetConst("event fade lead-in", "F3 0F 10 87 A0 01 00 00 0F 2F C6 76 25 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 87 A0 01 00 00",
                  13, kSubss0, 4, 8, fs);
    RetargetConst("event caption lead-in", "F3 0F 10 87 CC 01 00 00 0F 2F C6 72 15 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 87 CC 01 00 00",
                  13, kSubss0, 4, 8, fs);
    RetargetConst("event fade/noise wait", "F3 0F 10 0D ?? ?? ?? ?? 0F 2F C6 72 11 F3 0F 5C C1 F3 0F 11 87 A0 01 00 00",
                  0, {0xF3, 0x0F, 0x10, 0x0D}, 4, 8, fs);
    RetargetConst("forced-walk timer A", "0F 57 C9 0F 2F C1 72 15 F3 0F 5C 05 ?? ?? ?? ?? 0F 2F C1 F3 0F 11 83 BC 00 00 00",
                  8, kSubss0, 4, 8, fs);
    RetargetConst("forced-walk timer B", "0F 57 F6 0F 2F C6 72 15 F3 0F 5C 05 ?? ?? ?? ?? 0F 2F C6 F3 0F 11 83 BC 00 00 00",
                  8, kSubss0, 4, 8, fs);
    RetargetConst("fast travel hold", "F3 0F 10 43 44 0F 2F C6 72 12 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 43 44",
                  10, kSubss0, 4, 8, fs);
    RetargetConst("yes/no dialog input lock", "F3 0F 10 43 58 0F 57 C9 0F 2F C1 72 12 F3 0F 5C 05 ?? ?? ?? ?? 0F 2F C1 F3 0F 11 43 58",
                  13, kSubss0, 4, 8, fs);
    RetargetConst("timed HUD gauge", "0F 29 74 24 40 0F 29 7C 24 30 72 15 F3 0F 5C 05 ?? ?? ?? ?? 0F 2F C1 F3 0F 11 83 BC 00 00 00",
                  12, kSubss0, 4, 8, fs);
    RetargetConst("elevator timer", "F3 0F 10 83 A4 00 00 00 0F 2F C7 72 15 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 83 A4 00 00 00",
                  13, kSubss0, 4, 8, fs);
    if (uint8_t* m = FindUnique("CaptionAutoClose", "0F 2F 0D ?? ?? ?? ?? 0F 28 C1 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 86 8C 2D 00 00")) {
        // Compares against and subtracts the same constant; both must move together.
        RetargetRipOperand("caption auto-close (compare)", m, 3, 7, {0x0F, 0x2F, 0x0D}, fs);
        RetargetRipOperand("caption auto-close (subtract)", m + 10, 4, 8, kSubss0, fs);
    }
    // Placed item: 30-frame countdown (float frames, -1 per update).
    ScaleConst("placed item countdown",
               "FF 83 74 06 00 00 F3 0F 10 83 58 08 00 00 F3 0F 5C 05 ?? ?? ?? ?? 0F 2F C6 F3 0F 11 83 58 08 00 00",
               14, kSubss0, Scale::Lin, 1.0f);

    // Integer frame counters -> advance by 60 Hz ticks.
    // BunkerAction: two 30-frame waits (finish, warp).
    if (uint8_t* m = FindUnique("BunkerWaitA", "FF 81 E0 00 00 00 83 B9 E0 00 00 00 1E 0F 8C"))
        IncByTicks("bunker wait A", m, 0x81);
    if (uint8_t* m = FindUnique("BunkerWaitB", "8B 83 E0 00 00 00 FF C0 89 83 E0 00 00 00 83 F8 1E 7C")) {
        InstallCaveUnchecked("bunker wait B", m + 6, 8, false, [&](Asm& a) {
            a.raw({0x03, 0x05}).rip(&S->ticks60);              // add eax, [ticks60]
            a.raw({0x89, 0x83, 0xE0, 0x00, 0x00, 0x00});       // mov [rbx+0xE0], eax
            a.jmp(m + 14);
        });
    } else if (uint8_t* m2 = FindUnique("BunkerWaitB-compat", "E8 ?? ?? ?? ?? FF 83 E0 00 00 00 83 BB E0 00 00 00 1E 7C")) {
        IncByTicks("bunker wait B", m2 + 5, 0x83);
    }
    // NPC talk release countdown (15 frames before the NPC's talk-end): `dec [rcx+0x28]; cmp [rcx+0x28],0`.
    auto talkRelease = [](const char* name, uint8_t* site) {
        InstallCave(name, site, 7, {0xFF, 0x49, 0x28, 0x83, 0x79, 0x28, 0x00}, [&](Asm& a) {
            a.raw({0x50});                                      // push rax
            a.raw({0x8B, 0x05}).rip(&S->ticks60);               // mov eax, [ticks60]
            a.raw({0x29, 0x41, 0x28});                          // sub [rcx+0x28], eax
            a.raw({0x58});                                      // pop rax
            a.raw({0x83, 0x79, 0x28, 0x00});                    // cmp dword [rcx+0x28], 0
            a.jmp(site + 7);
        });
    };
    if (uint8_t* m = FindUnique("TalkReleaseA", "3D B7 EA 14 B6 0F 85 ?? ?? ?? ?? FF 49 28 83 79 28 00 0F 8F"))
        talkRelease("NPC talk release A", m + 11);
    if (uint8_t* m = FindUnique("TalkReleaseB", "49 8B E3 5F C3 FF 49 28 83 79 28 00 7F"))
        talkRelease("NPC talk release B", m + 5);
    // World update pause while a missing object file loads: gives up after 300 frames.
    if (uint8_t* m = FindUnique("ObjLoadWait",
            "48 8D 0D ?? ?? ?? ?? FF 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 75 0C 81 3D ?? ?? ?? ?? 2C 01 00 00")) {
        uint8_t* site = m + 7;
        uint8_t* counter = RipTarget(site, 2, 6);
        InstallCaveUnchecked("object load wait", site, 6, false, [&](Asm& a) {
            a.raw({0x50});                                      // push rax
            a.raw({0x8B, 0x05}).rip(&S->ticks60);               // mov eax, [ticks60]
            a.raw({0x01, 0x05}).rip(counter);                   // add [counter], eax
            a.raw({0x58});                                      // pop rax
            a.jmp(site + 6);
        });
    }
}

// ---------------------------------------------------------------------------
// Global slow-motion manager (finisher slow-mo, BGM slow, overclock): several
// per-frame countdowns next to delta-based ones, so only the counters change.

static void FixSlowMotion() {
    if (uint8_t* m = FindUnique("SlowMoFinishA", "8B 47 2C 85 C0 7E 24 FF C8 89 47 2C 66 0F 6E C0"))
        DecEaxByTicks("finisher slow-mo A", m + 7, 0x2C);
    if (uint8_t* m = FindUnique("SlowMoFinishB", "8B 47 38 85 C0 7E 24 FF C8 89 47 38 66 0F 6E C0"))
        DecEaxByTicks("finisher slow-mo B", m + 7, 0x38);
    if (uint8_t* m = FindUnique("SlowMoBgm", "8B 47 50 85 C0 7E 5D FF C8 89 47 50 85 C0"))
        DecEaxByTicks("BGM slow countdown", m + 7, 0x50);
    if (uint8_t* m = FindUnique("SlowMoTail", "85 C0 7E 05 FF C8 89 47 64 8B 47 60 85 C0 7E 07 FF C8 89 47 60 EB 07")) {
        DecEaxByTicks("slow-mo lock countdown", m + 4, 0x64);
        DecEaxByTicks("slow-mo rate countdown", m + 16, 0x60);
    }
    if (uint8_t* m = FindUnique("SlowMoRate", "8B 47 40 85 C0 7E 0C F3 0F 10 4F 3C FF C8 89 47 40 EB 0A"))
        DecEaxByTicks("slow-mo scale countdown", m + 12, 0x40);
    // Delayed slow-mo request: float frames, -1.0 per update (`subss xmm0, xmm7`, xmm7 = 1.0 is reused later).
    if (uint8_t* m = FindUnique("SlowMoDelay",
            "F3 0F 10 47 68 0F 2F C6 F3 0F 10 3D ?? ?? ?? ?? 76 20 F3 0F 5C C7 0F 2F C6 F3 0F 11 47 68")) {
        uint8_t* site = m + 18;
        InstallCave("slow-mo request delay", site, 7, {0xF3, 0x0F, 0x5C, 0xC7, 0x0F, 0x2F, 0xC6}, [&](Asm& a) {
            a.raw({0xF3, 0x0F, 0x5C, 0x05}).rip(&S->stepUpToOne);  // subss xmm0, [e]
            a.raw({0x0F, 0x2F, 0xC6});                             // comiss xmm0, xmm6
            a.jmp(site + 7);
        });
    }
}

// ---------------------------------------------------------------------------
// Fishing: the pod's state machine is delta-based except the bobber settle,
// which drains 0.01 per frame.

static void FixFishing() {
    ScaleConst("fishing bobber settle",
               "E8 ?? ?? ?? ?? F3 0F 10 8F 8C 3E 00 00 F3 0F 5C 0D ?? ?? ?? ?? F3 0F 11 87 88 3E 00 00 41 0F 2F C8 F3 0F 11 8F 8C 3E 00 00",
               13, {0xF3, 0x0F, 0x5C, 0x0D}, Scale::Lin, 0.01f);
}

// ---------------------------------------------------------------------------
// Player and enemy movement

// Press buffers (jump, attack, ... 8 counters, 20 frames) at input+0x770.
static void InputBufferTick(uint8_t* input) {
    int* c = (int*)(input + 0x770);
    for (int i = 0; i < 8; ++i)
        if (c[i] > 0) c[i] = c[i] > S->ticks60 ? c[i] - S->ticks60 : 0;
}

// Enemy navmesh pull-back: vanilla pulls min(ramp * dt, 1) of the way back each frame.
static float PullFraction(float x) {
    float e = S->stepUpToOne;
    if (!(e < 1.0f)) return x < 1.0f ? x : 1.0f;  // minss semantics
    float f60 = x / e;
    if (!(f60 < 1.0f)) f60 = 1.0f;
    return 1.0f - powf(1.0f - f60, e);
}

static void FixMovement() {
    // Glide spin kick: pos += forward * 0.14 * stick * 0.001 every frame.
    ScaleConst("glide spin kick drift",
               "F3 0F 10 15 ?? ?? ?? ?? 48 8D 54 24 40 48 8D 4C 24 50 E8 ?? ?? ?? ?? F3 0F 59 35 ?? ?? ?? ?? 48 8D 54 24 50 48 8D 4C 24 60 0F 28 D6 E8",
               23, kMulss6, Scale::Lin, 0.001f);

    // Air control: vel.xz = (1-k)*target + k*vel.xz per frame (k = 0.93 idle, 0.9 steering, 0.5 first frame).
    static const std::initializer_list<uint8_t> kMovss2 = {0xF3, 0x0F, 0x10, 0x15};
    ScaleConst("air control decay", "F3 0F 10 15 ?? ?? ?? ?? 75 49 F7 05 ?? ?? ?? ?? 00 00 00 20 75 3D", 0, kMovss2,
               Scale::Pow, 0.93f);
    if (uint8_t* m = FindUnique("AirCtlSteer",
            "F3 0F 10 15 ?? ?? ?? ?? 85 F6 74 08 F3 0F 10 15 ?? ?? ?? ?? 85 FF 74 03 0F 57 D2 F3 0F 5C F2 0F 28 C2 F3 0F 59 83 90 14 00 00")) {
        ScaleOperand("air control steering", m, kMovss2.begin(), 4, 4, 8, Scale::Pow, 0.9f);
        ScaleOperand("air control first frame", m + 12, kMovss2.begin(), 4, 4, 8, Scale::Pow, 0.5f);
    }

    // Input press buffers count down per frame: buffered jumps expired 2.4x early at 144 fps.
    if (uint8_t* m = FindUnique("InputBufDec",
            "8B 81 70 07 00 00 85 C0 7E 08 FF C8 89 81 70 07 00 00 8B 81 74 07 00 00 85 C0 7E 08 FF C8 89 81 74 07 00 00"))
        RedirectFunction("input press buffers", m, {0x8B, 0x81, 0x70, 0x07, 0x00}, (void*)&InputBufferTick);

    // Attack states: grounded velocity *= 0.8 per frame (player, second attack set, hacked robot).
    if (uint8_t* m = FindUnique("AtkDamp08",
            "C7 87 78 1A 00 00 00 00 80 3F 75 30 F3 0F 10 87 90 14 00 00 F3 0F 59 05 ?? ?? ?? ?? F3 0F 11 87 90 14 00 00 F3 0F 10 87 98 14 00 00 F3 0F 59 05")) {
        ScaleOperand("attack ground damping x", m + 20, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
        ScaleOperand("attack ground damping z", m + 44, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
    }
    if (uint8_t* m = FindUnique("Atk2Damp08",
            "C7 87 78 1A 00 00 00 00 80 3F 44 39 AF A4 14 00 00 75 30 F3 0F 10 87 90 14 00 00 F3 0F 59 05 ?? ?? ?? ?? F3 0F 11 87 90 14 00 00 F3 0F 10 87 98 14 00 00 F3 0F 59 05")) {
        ScaleOperand("attack 2 ground damping x", m + 27, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
        ScaleOperand("attack 2 ground damping z", m + 51, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
    }
    if (uint8_t* m = FindUnique("EmAtkDamp08",
            "83 BF B0 0C 00 00 00 75 30 F3 0F 10 87 A0 0C 00 00 F3 0F 59 05 ?? ?? ?? ?? F3 0F 11 87 A0 0C 00 00 F3 0F 10 87 A8 0C 00 00 F3 0F 59 05")) {
        ScaleOperand("hacked robot attack damping x", m + 17, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
        ScaleOperand("hacked robot attack damping z", m + 41, kMulss0.begin(), 4, 4, 8, Scale::Pow, 0.8f);
    }
    // Plunge attacks: vel.y -= 0.1 per frame.
    ScaleConst("attack 2 plunge",
               "F3 0F 11 87 90 14 00 00 F3 0F 10 87 94 14 00 00 F3 0F 5C 05 ?? ?? ?? ?? F3 0F 11 8F 98 14 00 00 F3 0F 11 87 94 14 00 00",
               16, kSubss0, Scale::Lin, 0.1f);
    if (uint8_t* m = FindUnique("AtkDive",
            "F3 0F 11 87 90 14 00 00 F3 0F 10 87 94 14 00 00 F3 41 0F 5C C4 F3 0F 11 8F 98 14 00 00 F3 0F 11 87 94 14 00 00")) {
        // `subss xmm0, xmm12` (xmm12 = 0.1, also used for clamps) -> subtract a scaled 0.1.
        float* k = NewScaled("attack plunge", Scale::Lin, 0.1f);
        InstallCave("attack plunge", m + 16, 5, {0xF3, 0x41, 0x0F, 0x5C, 0xC4}, [&](Asm& a) {
            a.raw({0xF3, 0x0F, 0x5C, 0x05}).rip(k);  // subss xmm0, [k]
            a.jmp(m + 21);
        });
    }

    // Scripted event lift: pos.y += 0.4 per frame.
    ScaleConst("event lift", "F3 0F 10 47 54 F3 0F 58 05 ?? ?? ?? ?? F3 0F 11 47 54 39 B7 68 0A 00 00", 5, kAddss0,
               Scale::Lin, 0.4f);

    // Slope landing: compares the per-frame squared xz displacement with 0.0025.
    ScaleConst("landing displacement threshold",
               "E8 ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 76 15 48 8B 03 45 33 C9 44 89 74 24 20 41 8D 51 3A", 5,
               {0x0F, 0x2F, 0x05}, Scale::Sq, 0.0025f, 3, 7);

    // Enemy navmesh pull-back: the ramp grows 0.01 per frame, and the pull fraction
    // min(ramp*dt, 1) must compound like 60 Hz steps.
    ScaleConst("enemy pull-back ramp",
               "F3 0F 10 83 54 C0 02 00 F3 0F 58 05 ?? ?? ?? ?? 41 0F 2F C0 F3 0F 11 83 54 C0 02 00", 8, kAddss0,
               Scale::Lin, 0.01f);
    if (uint8_t* m = FindUnique("EmPullClamp",
            "C7 83 54 C0 02 00 00 00 80 3E F3 41 0F 5D F0 48 8D 55 ?? 48 8D 4D ?? 0F 28 D6 E8")) {
        InstallCave("enemy pull-back fraction", m + 10, 5, {0xF3, 0x41, 0x0F, 0x5D, 0xF0}, [&](Asm& a) {
            EmitFloatCall(a, &PullFraction, 6, 6);  // xmm6 = PullFraction(xmm6)
            a.jmp(m + 15);
        });
    }
    // Enemy fall smoothing: x += (t - x) * 0.1 per frame.
    ScaleConst("enemy fall smoothing",
               "F3 0F 10 83 FC AC 02 00 F3 0F 10 8B A4 0C 00 00 F3 0F 5C D0 F3 0F 59 15 ?? ?? ?? ?? F3 0F 58 D0", 20,
               {0xF3, 0x0F, 0x59, 0x15}, Scale::Lerp, 0.1f);
}

void InstallSubsystemPatches() {
    Log("subsystem patches:");
    FixUi();
    FixHackGauge();
    FixQuests();
    FixSlowMotion();
    FixFishing();
    FixMovement();
    FixFlightUnit();
    FixHacking();
    FixMovementExtra();
    FixBulletsAndEnemies();
    FixWorld();
    FixCamera();
    FixExactSmoothing();
    FixAnimationInputs();
    FixRemainingExact();
    FixEnemyFinal();
    FixPushBlocks();
    FixCaptionLetterbox();
    if (g_cfg.fixCloth) FixCloth();
}