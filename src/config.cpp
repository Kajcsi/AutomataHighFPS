#include "common.h"
#include <cwchar>

Config g_cfg;

static const char kDefaultIni[] =
    "; NieR:Automata High FPS Fix\r\n"
    "[General]\r\n"
    "; 1 = mod active, 0 = vanilla behaviour (DLL only forwards DirectInput).\r\n"
    "Enabled=1\r\n"
    "; Frame rate limit. -1 = monitor refresh rate, 0 = unlimited, or a number such as 144.\r\n"
    "TargetFPS=-1\r\n"
    "; Key that toggles a temporary 60 fps cap (virtual-key code, 0x7A = F11, 0 = off).\r\n"
    "ToggleCapKey=0x7A\r\n"
    "\r\n"
    "[Fixes]\r\n"
    "; Per-frame counters (hit-stop, transition cooldowns, ...) advance at 60 Hz.\r\n"
    "FixFrameCounters=1\r\n"
    "; Millisecond quest timers keep their sub-millisecond remainders.\r\n"
    "FixMsTimers=1\r\n"
    "; Hacking, menus, flight unit, fishing and movement fixes.\r\n"
    "FixSubsystems=1\r\n"
    "\r\n"
    "[Limits]\r\n"
    "; Optional frame rate cap while a menu is open (0 = no cap).\r\n"
    "MenuFPS=0\r\n"
    "; Remove the frame limit on loading screens (faster loads).\r\n"
    "UnlimitedLoading=1\r\n"
    "\r\n"
    "[Display]\r\n"
    "; In exclusive fullscreen, request the highest refresh rate the monitor\r\n"
    "; supports at the chosen resolution instead of 60 Hz.\r\n"
    "ForceMaxRefreshRate=1\r\n";

static int ReadInt(const wchar_t* path, const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, path);
    if (!buf[0]) return def;
    return (int)wcstol(buf, nullptr, 0);  // accepts decimal, 0x hex and negative values
}

void LoadConfig(const wchar_t* path) {
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &written, nullptr);
            CloseHandle(h);
        }
    }
    g_cfg.enabled = ReadInt(path, L"General", L"Enabled", 1) != 0;
    g_cfg.targetFps = ReadInt(path, L"General", L"TargetFPS", -1);
    g_cfg.toggleKey = ReadInt(path, L"General", L"ToggleCapKey", VK_F11);
    g_cfg.fixFrameCounters = ReadInt(path, L"Fixes", L"FixFrameCounters", 1) != 0;
    g_cfg.fixMsTimers = ReadInt(path, L"Fixes", L"FixMsTimers", 1) != 0;
    g_cfg.fixSubsystems = ReadInt(path, L"Fixes", L"FixSubsystems", 1) != 0;
    g_cfg.menuFps = ReadInt(path, L"Limits", L"MenuFPS", 0);
    g_cfg.unlimitedLoading = ReadInt(path, L"Limits", L"UnlimitedLoading", 1) != 0;
    g_cfg.forceMaxRefresh = ReadInt(path, L"Display", L"ForceMaxRefreshRate", 1) != 0;
    if (g_cfg.targetFps < -1) g_cfg.targetFps = -1;
}
