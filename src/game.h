#pragma once
#include "common.h"

// Addresses resolved by signature scan. Field comments give the Steam 2021
// NieRAutomata.exe address for reference; the Compat exe resolves to its own.
struct GameAddrs {
    uint8_t* image = nullptr;
    size_t imageSize = 0;
    uint8_t* text = nullptr;
    size_t textSize = 0;

    // Code sites
    uint8_t* timeStepCall = nullptr;    // 14075EB4F  call UpdateTimeStep in the main update
    uint8_t* limiterLoad = nullptr;     // 14024A1EE  limiter reads framework+0x308 (target, 1/3 ms)
    uint8_t* stepClampJae = nullptr;    // 14014BEB4  jae over "step = 1.0" lower clamp
    uint8_t* nodeCounters = nullptr;    // 14014C0CB  time-node delay / hit-stop counters
    uint8_t* frameTicksFn = nullptr;    // 1407A9110  (int)(delta + 0.99)
    uint8_t* timeCountAddFn = nullptr;  // 1409CFEE0  counter_ms += (int)(dt * 1000)
    uint8_t* msCountdown = nullptr;     // 140348B9C  remaining_ms -= (int)(dt * 1000)

    // Functions
    void (*updateTimeStep)(void* obj, int variableStep, float fixedStep) = nullptr;  // 14014BDB0
    float (*getDelta)(void* handle) = nullptr;                                         // 14014B990

    // Data
    uint32_t* scriptFlags = nullptr;  // 1410297C0
    uint8_t* timeObj = nullptr;       // 141029F20, step float at +0xB0
    void* deltaHandle = nullptr;      // 141029FF0
    uint8_t* uiMgr = nullptr;         // 140F68AF0, menu-open flag at +0x1E4
    uint32_t* loadFlag = nullptr;     // 14100D410
    uint32_t* shortcutMenu = nullptr; // 141029760

    float Step() const { return timeObj ? *(float*)(timeObj + 0xB0) : 1.0f; }
};
extern GameAddrs G;

// Finds every signature. Returns false when a site the core unlock needs is missing.
bool ResolveGame();

// Signature lookup for subsystem patches: returns the unique match or nullptr (and logs).
uint8_t* FindUnique(const char* name, const char* sig);
// Frees the pristine code copy signatures are matched against (call after all patches).
void ReleaseScanCopy();
