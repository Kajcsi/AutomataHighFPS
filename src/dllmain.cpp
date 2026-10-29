// NieR:Automata High FPS Fix - loaded as dinput8.dll from the game folder.
#include "common.h"
#include "memory.h"
#include "game.h"
#include "timing.h"
#include "display.h"
#include "subsystems.h"
#include <unknwn.h>

static wchar_t g_gameDir[MAX_PATH];
static uint8_t* g_image;
static volatile LONG g_initState;  // 0 = pending, 1 = running, 2 = done
static void(WINAPI* g_origGetSystemTimeAsFileTime)(LPFILETIME);
static uint64_t g_codeVa;
static uint8_t g_encryptedHead[64];

const wchar_t* GameDir() { return g_gameDir; }

// ---------------------------------------------------------------------------
// DirectInput forwarding

static HMODULE RealDinput() {
    static HMODULE mod;
    if (!mod) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        mod = LoadLibraryW(path);
    }
    return mod;
}

template <class F>
static F Real(const char* name) {
    HMODULE m = RealDinput();
    return m ? (F)GetProcAddress(m, name) : nullptr;
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    auto f = Real<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>("DirectInput8Create");
    return f ? f(inst, ver, riid, out, outer) : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto f = Real<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto f = Real<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
    return f ? f(clsid, riid, out) : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllRegisterServer() {
    auto f = Real<HRESULT(WINAPI*)()>("DllRegisterServer");
    return f ? f() : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto f = Real<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

// ---------------------------------------------------------------------------
// Deferred initialisation. The executable is wrapped by SteamStub, which
// decrypts .text only after our DllMain has run, so patching waits until the
// game's own CRT startup makes its first GetSystemTimeAsFileTime call.

static bool CodeDecrypted() {
    __try {
        auto* nt = (IMAGE_NT_HEADERS64*)(g_image + ((IMAGE_DOS_HEADER*)g_image)->e_lfanew);
        uint8_t* ep = g_image + nt->OptionalHeader.AddressOfEntryPoint;
        // SteamStub 3.1 header: 0xF0 bytes before the stub entry, XOR-chained dwords.
        uint32_t h[0xF0 / 4];
        memcpy(h, ep - sizeof h, sizeof h);
        uint32_t key = h[0];
        for (int i = 1; i < 0xF0 / 4; ++i) {
            uint32_t v = h[i];
            h[i] ^= key;
            key = v;
        }
        if (h[1] != 0xC0DEC0DF) return true;  // not wrapped
        // DllMain snapshotted the still-encrypted start of the code section; decryption changes it.
        uint64_t codeVa = *(uint64_t*)&h[0x48 / 4];
        if (!g_codeVa) {
            g_codeVa = codeVa;
            memcpy(g_encryptedHead, g_image + codeVa, sizeof g_encryptedHead);
            return false;
        }
        return memcmp(g_image + g_codeVa, g_encryptedHead, sizeof g_encryptedHead) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void RunInit() {
    G.image = g_image;
    Log("code decrypted, installing patches (frame %llu)", g_frame);
    if (!g_arena.Init(g_image, 0x2000000)) {
        Log("could not allocate code memory near the image - mod inactive");
        return;
    }
    if (!ResolveGame()) {
        Log("required signatures not found - unsupported game version, mod inactive");
        return;
    }
    if (!InstallCorePatches()) Log("WARNING: some core patches failed");
    if (g_cfg.fixSubsystems) InstallSubsystemPatches();
    ReleaseScanCopy();
    size_t patches;
    Patches(&patches);
    Log("initialisation finished: %zu patches applied", patches);
}

static bool TryInit() {
    if (g_initState == 2) return true;
    if (!CodeDecrypted()) return false;
    if (InterlockedCompareExchange(&g_initState, 1, 0) != 0) return g_initState == 2;
    RunInit();
    g_initState = 2;
    return true;
}

static void WINAPI HookGetSystemTimeAsFileTime(LPFILETIME ft) {
    g_origGetSystemTimeAsFileTime(ft);
    if (g_initState == 0 && TryInit())
        HookImport(g_image, "KERNEL32.dll", "GetSystemTimeAsFileTime", (void*)g_origGetSystemTimeAsFileTime);
}

static DWORD WINAPI FallbackInitThread(void*) {
    Sleep(3000);
    for (int i = 0; i < 600 && g_initState != 2; ++i) {
        if (TryInit()) break;
        Sleep(50);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_image = (uint8_t*)GetModuleHandleW(nullptr);
    GetModuleFileNameW(nullptr, g_gameDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_gameDir, L'\\')) slash[1] = 0;

    wchar_t path[MAX_PATH];
    wcscpy_s(path, g_gameDir);
    wcscat_s(path, L"AutomataHighFPS.ini");
    LoadConfig(path);
    wcscpy_s(path, g_gameDir);
    wcscat_s(path, L"AutomataHighFPS.log");
    LogOpen(path);
    Log("NieR:Automata High FPS Fix loaded (TargetFPS=%d, enabled=%d)", g_cfg.targetFps, g_cfg.enabled);
    if (!g_cfg.enabled) return TRUE;

    CodeDecrypted();  // takes the snapshot of the encrypted code while SteamStub has not run yet
    g_origGetSystemTimeAsFileTime = (decltype(g_origGetSystemTimeAsFileTime))HookImport(
        g_image, "KERNEL32.dll", "GetSystemTimeAsFileTime", (void*)&HookGetSystemTimeAsFileTime);
    InstallDisplayHooks(g_image);
    if (HANDLE t = CreateThread(nullptr, 0, FallbackInitThread, nullptr, 0, nullptr)) CloseHandle(t);
    if (!g_origGetSystemTimeAsFileTime) Log("GetSystemTimeAsFileTime import not found, using fallback init");
    return TRUE;
}
