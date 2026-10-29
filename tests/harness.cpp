// Offline test harness: maps a decrypted NieRAutomata.exe without running it,
// applies the mod's patches, then calls the patched game functions directly.
// Usage: harness.exe <unpacked exe> <dump file> [functional]
#include "../src/common.h"
#include "../src/memory.h"
#include "../src/game.h"
#include "../src/timing.h"
#include "../src/subsystems.h"
#include "../src/display.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cmath>

const wchar_t* GameDir() { return L".\\"; }

static int g_failures;
// Defined in tests/subsystem_tests.cpp.
void RunSubsystemTests(uint8_t* img, int* failures);
extern const uint8_t* g_pristineImage;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        bool c_ = (cond);                                    \
        printf("%s ", c_ ? "PASS" : "FAIL");                 \
        printf(__VA_ARGS__);                                 \
        printf("\n");                                        \
        if (!c_) ++g_failures;                               \
    } while (0)

static uint64_t ReturnZero() { return 0; }

static void ResolveImports(uint8_t* img) {
    auto* nt = (IMAGE_NT_HEADERS64*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(img + dir.VirtualAddress); imp->Name; ++imp) {
        const char* dll = (const char*)(img + imp->Name);
        HMODULE m = _stricmp(dll, "steam_api64.dll") ? LoadLibraryA(dll) : nullptr;
        auto* names = (IMAGE_THUNK_DATA64*)(img + imp->OriginalFirstThunk);
        auto* iat = (IMAGE_THUNK_DATA64*)(img + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            FARPROC p = nullptr;
            if (m) {
                p = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)
                        ? GetProcAddress(m, (LPCSTR)IMAGE_ORDINAL64(names->u1.Ordinal))
                        : GetProcAddress(m, (LPCSTR)((IMAGE_IMPORT_BY_NAME*)(img + names->u1.AddressOfData))->Name);
            }
            void* v = p ? (void*)p : (void*)&ReturnZero;
            WriteProtected(&iat->u1.Function, &v, sizeof v);
        }
    }
}

template <class T>
static T& At(uint8_t* img, uint32_t rva) { return *(T*)(img + rva); }

static void Functional(uint8_t* img) {
    // Steam 2021 NieRAutomata.exe RVAs used to set up game state for the tests.
    const uint32_t kQpcBase = 0x176F578, kMsPerTick = 0x176F5A8, kTimeObjPtr = 0x176DEA0, kNodeFn = 0x14BFA0;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    At<int64_t>(img, kQpcBase) = 0;
    At<double>(img, kMsPerTick) = 1000.0 / (double)freq.QuadPart;

    // --- Time step: no longer clamped to >= 1.0 -------------------------------
    alignas(16) uint8_t obj[0x100] = {};
    *(float*)(obj + 0xB8) = 0.06f;  // 60 fps frames per millisecond
    auto stepAfter = [&](double ms) {
        QueryPerformanceCounter(&now);
        *(int64_t*)(obj + 0xA8) = now.QuadPart - (int64_t)(ms * freq.QuadPart / 1000.0);
        G.updateTimeStep(obj, 1, 1.0f);
        return *(float*)(obj + 0xB0);
    };
    float s144 = stepAfter(1000.0 / 144), s60 = stepAfter(1000.0 / 60), s30 = stepAfter(1000.0 / 30),
          s20 = stepAfter(1000.0 / 20);
    CHECK(fabs(s144 - 0.4167f) < 0.02f, "step at 144 fps = %.4f (vanilla clamps to 1.0)", s144);
    CHECK(fabs(s60 - 1.0f) < 0.02f, "step at 60 fps = %.4f", s60);
    CHECK(fabs(s30 - 2.0f) < 0.03f, "step at 30 fps = %.4f", s30);
    CHECK(s20 == 2.0f, "step at 20 fps clamped to %.4f (vanilla upper clamp kept)", s20);

    // --- 60 Hz tick accumulator over one simulated second at 144 fps -----------
    int ticks = 0;
    for (int i = 0; i < 144; ++i) {
        QueryPerformanceCounter(&now);
        *(int64_t*)(obj + 0xA8) = now.QuadPart - (int64_t)(freq.QuadPart / 144);
        OnUpdateTimeStep(obj, 1, 1.0f);
        ticks += S->ticks60;
    }
    CHECK(ticks >= 58 && ticks <= 61, "60 Hz ticks over 144 frames = %d", ticks);
    QueryPerformanceCounter(&now);
    *(int64_t*)(obj + 0xA8) = now.QuadPart - freq.QuadPart / 50;
    OnUpdateTimeStep(obj, 1, 1.0f);
    CHECK(S->ticks60 == 1, "ticks60 at 50 fps = %d (vanilla: 1 per frame)", S->ticks60);
    CHECK(S->targetUnits == 0.0, "no frame limit by default (limiter target %.3f units)", S->targetUnits);

    // --- Hit-stop counters in time nodes --------------------------------------
    alignas(16) uint8_t tobj[0x200] = {};
    alignas(16) uint8_t node[0x48] = {};
    *(void**)(tobj + 0x60) = node;               // active node list head
    At<void*>(img, kTimeObjPtr) = tobj;          // global time object
    *(float*)(tobj + 0x74) = 0.5f;               // layer 0 delta
    *(int*)(node + 0x08) = 0;                    // layer
    *(int*)(node + 0x0C) = 1;                    // refcount
    *(float*)(node + 0x10) = 1.0f;               // rate
    *(float*)(node + 0x18) = 0.25f;              // hit-stop rate
    *(int*)(node + 0x20) = 1;                    // hit-stop active
    *(int*)(node + 0x24) = 2;                    // duration (frames)
    *(int*)(node + 0x28) = 1;                    // delay (frames)
    auto nodeFn = (void (*)(void*))(img + kNodeFn);
    auto runNode = [&](int t) { S->ticks60 = t; nodeFn(tobj); };
    runNode(0);
    CHECK(*(int*)(node + 0x28) == 1 && *(float*)(node + 0x14) == 1.0f, "no 60 Hz tick: delay stays %d", *(int*)(node + 0x28));
    runNode(1);
    CHECK(*(int*)(node + 0x28) == 0, "tick: delay -> %d", *(int*)(node + 0x28));
    runNode(1);
    CHECK(*(int*)(node + 0x24) == 1 && *(float*)(node + 0x14) == 0.25f && fabs(*(float*)(node + 0x1C) - 0.125f) < 1e-6,
          "tick: duration -> %d, rate %.2f, node delta %.3f", *(int*)(node + 0x24), *(float*)(node + 0x14), *(float*)(node + 0x1C));
    runNode(0);
    CHECK(*(int*)(node + 0x24) == 1, "no tick: duration stays %d", *(int*)(node + 0x24));
    runNode(1);
    CHECK(*(int*)(node + 0x20) == 0 && *(float*)(node + 0x14) == 1.0f, "tick: hit-stop ends (active %d, rate %.2f)",
          *(int*)(node + 0x20), *(float*)(node + 0x14));

    // --- Frame-tick getter ------------------------------------------------------
    float& step = *(float*)(G.timeObj + 0xB0);
    auto frameTicks = (int (*)())G.frameTicksFn;
    *(float*)(tobj + 0xA4) = 1.0f;  // delta returned for the default handle
    step = 1.0f;
    CHECK(frameTicks() == 1, "frame ticks at 60 fps = 1");
    *(float*)(tobj + 0xA4) = 2.0f;
    step = 2.0f;
    CHECK(frameTicks() == 2, "frame ticks at 30 fps = 2");
    *(float*)(tobj + 0xA4) = 0.4f;
    step = 0.4f;
    S->ticks60 = 0;
    CHECK(frameTicks() == 0, "frame ticks at 150 fps without 60 Hz tick = 0 (vanilla 1)");
    S->ticks60 = 1;
    CHECK(frameTicks() == 1, "frame ticks at 150 fps on 60 Hz tick = 1");

    // --- Millisecond timers -----------------------------------------------------
    auto timeCountAdd = (void (*)(int*, float))G.timeCountAddFn;
    int counter = 0;
    step = 1.0f;
    for (int i = 0; i < 60; ++i, ++g_frame) timeCountAdd(&counter, 1.0f / 60);
    CHECK(counter == 960, "TimeCountAction after 1 s at 60 fps = %d ms (vanilla 960)", counter);
    counter = 0;
    step = 60.0f / 144;
    for (int i = 0; i < 144; ++i, ++g_frame) timeCountAdd(&counter, step / 60);
    CHECK(counter >= 958 && counter <= 961, "TimeCountAction after 1 s at 144 fps = %d ms (vanilla 864)", counter);
    counter = 0;
    step = 60.0f / 2000;
    for (int i = 0; i < 2000; ++i, ++g_frame) timeCountAdd(&counter, step / 60);
    CHECK(counter >= 958 && counter <= 961, "TimeCountAction after 1 s at 2000 fps = %d ms (vanilla 0: timer never fires)", counter);
}

static void* ImportSlot(uint8_t* img, const char* dll, const char* func) {
    auto* nt = (IMAGE_NT_HEADERS64*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(img + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(img + imp->Name), dll)) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(img + imp->OriginalFirstThunk);
        auto* iat = (IMAGE_THUNK_DATA64*)(img + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat)
            if (!IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                !strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(img + names->u1.AddressOfData))->Name, func))
                return (void*)iat->u1.Function;
    }
    return nullptr;
}

static bool InHarness(void* p) {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    return (uint8_t*)p >= base && (uint8_t*)p < base + nt->OptionalHeader.SizeOfImage;
}

// The game creates its swap chain like this: CreateDXGIFactory (imported), then
// IDXGIFactory2::CreateSwapChainForHwnd on a D3D11 device.
static void DisplayHooks(uint8_t* img) {
    InstallDisplayHooks(img);
    using CreateFactory_t = HRESULT(WINAPI*)(REFIID, void**);
    using CreateDevice_t = decltype(&D3D11CreateDevice);
    auto createFactory = (CreateFactory_t)ImportSlot(img, "dxgi.dll", "CreateDXGIFactory");
    auto createDevice = (CreateDevice_t)ImportSlot(img, "d3d11.dll", "D3D11CreateDevice");
    CHECK(createFactory && InHarness((void*)createFactory), "CreateDXGIFactory import hooked");
    if (!createFactory || !createDevice) return;
    IDXGIFactory* factory = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    ID3D11Device* device = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory), (void**)&factory)) ||
        FAILED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2)) ||
        FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
        printf("SKIP display hooks: no DXGI 1.2 / WARP device\n");
        return;
    }
    CHECK(InHarness((*(void***)factory2)[15]), "CreateSwapChainForHwnd hooked");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 64;
    desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
    fs.RefreshRate = {60, 1};
    fs.Windowed = TRUE;
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = factory2->CreateSwapChainForHwnd(device, wnd, &desc, &fs, nullptr, &sc);
    CHECK(SUCCEEDED(hr) && sc, "swap chain created through the hook (hr 0x%08lx)", hr);
    if (sc) {
        CHECK(InHarness((*(void***)sc)[14]), "ResizeTarget hooked on the new swap chain");
        DXGI_MODE_DESC mode{};
        mode.Width = 64;
        mode.Height = 64;
        CHECK(SUCCEEDED(sc->ResizeTarget(&mode)), "windowed ResizeTarget passes through");
        sc->Release();
    }
    DestroyWindow(wnd);
    device->Release();
    factory2->Release();
    factory->Release();
}

// Writes one JSON line per patch: image-relative site, original and patched bytes, cave bytes.
static void Dump(const wchar_t* path, uint8_t* img, const uint8_t* pristine) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    auto hex = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) fprintf(f, "%02x", p[i]);
    };
    size_t count;
    const PatchRecord* r = Patches(&count);
    for (size_t i = 0; i < count; ++i, ++r) {
        size_t rva = (size_t)(r->site - img);
        fprintf(f, "{\"name\":\"%s\",\"rva\":%zu,\"len\":%zu,\"orig\":\"", r->name, rva, r->len);
        hex(pristine + rva, r->len + 16);
        fprintf(f, "\",\"new\":\"");
        hex(r->site, r->len + 16);
        fprintf(f, "\",\"cave\":%llu,\"caveBytes\":\"", (unsigned long long)(r->cave ? r->cave - img : 0));
        if (r->cave) hex(r->cave, r->caveLen);
        fprintf(f, "\"}\n");
    }
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep output up to a crash
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        auto* er = ep->ExceptionRecord;
        printf("CRASH 0x%08lX at harness+0x%llx (access %p)\n", er->ExceptionCode,
               (unsigned long long)((uint8_t*)er->ExceptionAddress - (uint8_t*)GetModuleHandleW(nullptr)),
               er->NumberParameters > 1 ? (void*)er->ExceptionInformation[1] : nullptr);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    LogOpen(L"harness.log");
    uint8_t* img = (uint8_t*)LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!img) {
        printf("FAIL could not map %ls (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    ResolveImports(img);
    auto* nt = (IMAGE_NT_HEADERS64*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    size_t imageSize = nt->OptionalHeader.SizeOfImage;
    uint8_t* pristine = (uint8_t*)malloc(imageSize);
    memcpy(pristine, img, imageSize);
    G.image = img;
    CHECK(g_arena.Init(img, imageSize), "near arena allocated");
    CHECK(ResolveGame(), "signatures resolved");
    CHECK(InstallCorePatches(), "core patches installed");
    InstallSubsystemPatches();
    g_pristineImage = pristine;
    if (argc > 3) {
        Functional(img);
        RunSubsystemTests(img, &g_failures);
        DisplayHooks(img);
    }
    Dump(argv[2], img, pristine);
    printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
