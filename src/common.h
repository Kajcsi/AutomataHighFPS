#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstdlib>

// Logging (AutomataHighFPS.log next to the game executable).
void LogOpen(const wchar_t* path);
void Log(const char* fmt, ...);

// User settings (AutomataHighFPS.ini next to the game executable).
struct Config {
    bool enabled = true;
    int targetFps = -1;            // -1 = monitor refresh rate, 0 = unlimited
    bool fixFrameCounters = true;  // per-frame counters tick at 60 Hz
    bool fixMsTimers = true;       // millisecond timers keep sub-ms remainders
    bool fixSubsystems = true;     // hacking, menus, flight unit, fishing, movement fixes
    int menuFps = 0;               // optional cap while a menu is open (0 = off)
    bool unlimitedLoading = true;  // uncap loading screens (faster loads)
    bool forceMaxRefresh = true;   // exclusive fullscreen uses the highest refresh rate
    int toggleKey = VK_F11;        // toggles a temporary 60 fps cap
};
extern Config g_cfg;
void LoadConfig(const wchar_t* path);

// Directory of the game executable, with trailing backslash.
const wchar_t* GameDir();
