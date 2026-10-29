#pragma once
#include "common.h"

// Hooks CreateDXGIFactory in the game's import table (safe to call from DllMain).
void InstallDisplayHooks(uint8_t* image);
// Refresh rate of the monitor showing the game (re-queried about once a second).
double CurrentRefreshHz();
// Replaces an import of `image` by name; returns the previous function or nullptr.
void* HookImport(uint8_t* image, const char* dll, const char* func, void* replacement);
