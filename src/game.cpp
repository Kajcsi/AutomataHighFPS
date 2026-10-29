#include "game.h"
#include "memory.h"

GameAddrs G;

// Signatures are matched against a copy of the code taken before any patching,
// so a signature that overlaps an earlier patch site still matches.
static uint8_t* g_textCopy;

static uint8_t* Find(const char* name, const char* sig, size_t* countOut = nullptr) {
    Pattern p;
    if (!ParsePattern(sig, p)) {
        Log("  bad signature %s", name);
        return nullptr;
    }
    const uint8_t* scan = g_textCopy ? g_textCopy : G.text;
    uint8_t* first;
    size_t count = ScanPattern(scan, G.textSize, p, &first);
    if (countOut) *countOut = count;
    Log("  sig %-14s %zu match(es)%s", name, count, count ? "" : "  <-- not found");
    return count ? G.text + (first - scan) : nullptr;
}

void ReleaseScanCopy() {
    if (g_textCopy) VirtualFree(g_textCopy, 0, MEM_RELEASE);
    g_textCopy = nullptr;
}

uint8_t* FindUnique(const char* name, const char* sig) {
    size_t count;
    uint8_t* m = Find(name, sig, &count);
    return count == 1 ? m : nullptr;
}

bool ResolveGame() {
    auto* dos = (IMAGE_DOS_HEADER*)G.image;
    auto* nt = (IMAGE_NT_HEADERS64*)(G.image + dos->e_lfanew);
    G.imageSize = nt->OptionalHeader.SizeOfImage;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!memcmp(sec->Name, ".text", 6)) {
            G.text = G.image + sec->VirtualAddress;
            G.textSize = sec->Misc.VirtualSize;
        }
    }
    if (!G.text) return false;
    Log("image %p size %zx, .text %p size %zx, timestamp %08x", G.image, G.imageSize, G.text, G.textSize,
        nt->FileHeader.TimeDateStamp);
    if ((g_textCopy = (uint8_t*)VirtualAlloc(nullptr, G.textSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
        memcpy(g_textCopy, G.text, G.textSize);

    if (uint8_t* m = FindUnique("MainTimeStep",
            "8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 33 D2 25 00 00 00 50 3D 00 00 00 50 0F 28 D6 0F 95 C2 E8 ?? ?? ?? ??")) {
        G.scriptFlags = (uint32_t*)RipTarget(m, 2, 6);
        G.timeObj = RipTarget(m + 6, 3, 7);
        G.timeStepCall = m + 31;
        G.updateTimeStep = (decltype(G.updateTimeStep))RipTarget(m + 31, 1, 5);
    }
    if (uint8_t* m = FindUnique("Limiter",
            "8B 83 08 03 00 00 45 0F 57 C0 F2 4C 0F 2A C0 F2 44 0F 5C 83 B0 02 00 00"))
        G.limiterLoad = m;
    if (uint8_t* m = FindUnique("StepClamp",
            "0F 2F 0D ?? ?? ?? ?? F3 0F 11 8B B0 00 00 00 73 0F C7 83 B0 00 00 00 00 00 80 3F"))
        G.stepClampJae = m + 15;
    if (uint8_t* m = FindUnique("NodeCounters",
            "8B 42 28 85 C0 7E 0E FF C8 C7 42 14 00 00 80 3F 89 42 28 EB 23 FF 4A 24 44 39 52 24 7F 10"))
        G.nodeCounters = m;
    if (uint8_t* m = FindUnique("FrameTicks",
            "48 83 EC 28 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 58 05 ?? ?? ?? ?? F3 0F 2C C0 48 83 C4 28 C3")) {
        G.frameTicksFn = m;
        G.deltaHandle = RipTarget(m + 4, 3, 7);
        G.getDelta = (decltype(G.getDelta))RipTarget(m + 11, 1, 5);
    }
    if (uint8_t* m = FindUnique("TimeCountAdd",
            "0F 57 C0 0F 2F C8 72 0F F3 0F 59 0D ?? ?? ?? ?? F3 48 0F 2C C1 01 01 C3"))
        G.timeCountAddFn = m;
    if (uint8_t* m = FindUnique("MsCountdown",
            "E8 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? F3 0F 2C C0 8B 5F 30 2B D8"))
        G.msCountdown = m + 5;
    if (uint8_t* m = Find("UiMgr", "48 8D 54 24 30 48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? FF 50 10"))
        G.uiMgr = RipTarget(m + 5, 3, 7);
    if (uint8_t* m = FindUnique("LoadFlag",
            "B8 01 00 00 00 86 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00"))
        G.loadFlag = (uint32_t*)RipTarget(m + 11, 2, 10);
    if (uint8_t* m = FindUnique("ShortcutMenu",
            "89 5C 24 70 0F 29 74 24 50 E8 ?? ?? ?? ?? 89 05 ?? ?? ?? ??"))
        G.shortcutMenu = (uint32_t*)RipTarget(m + 14, 2, 6);

    Log("flags %p timeObj %p handle %p getDelta %p uiMgr %p load %p shortcut %p", G.scriptFlags, G.timeObj,
        G.deltaHandle, G.getDelta, G.uiMgr, G.loadFlag, G.shortcutMenu);
    return G.timeStepCall && G.limiterLoad && G.stepClampJae && G.timeObj;
}
