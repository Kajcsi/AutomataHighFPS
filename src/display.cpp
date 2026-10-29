#include "display.h"
#include "memory.h"
#include <dxgi1_2.h>

static HWND g_hwnd;
static double g_refreshHz = 60.0;
static ULONGLONG g_lastQuery;

using CreateDXGIFactory_t = HRESULT(WINAPI*)(REFIID, void**);
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using ResizeTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static CreateDXGIFactory_t g_origCreateFactory;
static CreateSwapChain_t g_origCreateSwapChain;
static ResizeTarget_t g_origResizeTarget;
static CreateSwapChainForHwnd_t g_origCreateSwapChainForHwnd;

void* HookImport(uint8_t* image, const char* dll, const char* func, void* replacement) {
    auto* nt = (IMAGE_NT_HEADERS64*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(image + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(image + imp->Name), dll)) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(image + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA64*)(image + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(image + names->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func)) continue;
            void* prev = (void*)iat->u1.Function;
            WriteProtected(&iat->u1.Function, &replacement, sizeof(void*));
            return prev;
        }
    }
    return nullptr;
}

static bool MonitorDevice(HWND hwnd, wchar_t (&device)[CCHDEVICENAME]) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    wcscpy_s(device, mi.szDevice);
    return true;
}

double CurrentRefreshHz() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastQuery < 1000) return g_refreshHz;
    g_lastQuery = now;
    wchar_t dev[CCHDEVICENAME];
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (MonitorDevice(g_hwnd, dev) && EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        if (dm.dmDisplayFrequency != (DWORD)g_refreshHz) Log("monitor refresh rate %lu Hz", dm.dmDisplayFrequency);
        g_refreshHz = dm.dmDisplayFrequency;
    }
    return g_refreshHz;
}

// Raises a fullscreen mode's refresh rate to the highest the monitor offers at that resolution.
static void PickMaxRefresh(HWND hwnd, DXGI_MODE_DESC& mode) {
    wchar_t dev[CCHDEVICENAME];
    if (!MonitorDevice(hwnd, dev)) return;
    DWORD best = 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(dev, i, &dm); ++i)
        if (dm.dmPelsWidth == mode.Width && dm.dmPelsHeight == mode.Height && dm.dmDisplayFrequency > best)
            best = dm.dmDisplayFrequency;
    UINT requested = mode.RefreshRate.Denominator ? mode.RefreshRate.Numerator / mode.RefreshRate.Denominator : 0;
    if (best > requested && best > 60) {
        Log("fullscreen %ux%u: refresh %u Hz -> %lu Hz", mode.Width, mode.Height, requested, best);
        mode.RefreshRate.Numerator = best;
        mode.RefreshRate.Denominator = 1;
    }
}

static void PatchVtable(void* obj, int slot, void* hook, void** orig) {
    void** vtbl = *(void***)obj;
    if (vtbl[slot] == hook) return;
    *orig = vtbl[slot];
    WriteProtected(&vtbl[slot], &hook, sizeof(void*));
}

static HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* desc) {
    if (desc && g_cfg.forceMaxRefresh) {
        BOOL fullscreen = FALSE;
        sc->GetFullscreenState(&fullscreen, nullptr);
        if (fullscreen) {
            DXGI_MODE_DESC m = *desc;
            PickMaxRefresh(g_hwnd, m);
            return g_origResizeTarget(sc, &m);
        }
    }
    return g_origResizeTarget(sc, desc);
}

static HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    DXGI_SWAP_CHAIN_DESC d;
    if (desc) {
        d = *desc;
        g_hwnd = d.OutputWindow;
        g_lastQuery = 0;
        Log("swap chain %ux%u windowed=%d refresh %u/%u", d.BufferDesc.Width, d.BufferDesc.Height, d.Windowed,
            d.BufferDesc.RefreshRate.Numerator, d.BufferDesc.RefreshRate.Denominator);
        if (!d.Windowed && g_cfg.forceMaxRefresh) PickMaxRefresh(d.OutputWindow, d.BufferDesc);
    }
    HRESULT hr = g_origCreateSwapChain(f, dev, desc ? &d : desc, out);
    if (SUCCEEDED(hr) && out && *out) PatchVtable(*out, 14, (void*)&HookResizeTarget, (void**)&g_origResizeTarget);
    return hr;
}

// The game creates its swap chain through IDXGIFactory2::CreateSwapChainForHwnd.
static HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
                                                            IDXGISwapChain1** out) {
    g_hwnd = hwnd;
    g_lastQuery = 0;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC f2;
    if (desc && fs) {
        f2 = *fs;
        Log("swap chain %ux%u windowed=%d refresh %u/%u", desc->Width, desc->Height, fs->Windowed,
            fs->RefreshRate.Numerator, fs->RefreshRate.Denominator);
        if (!fs->Windowed && g_cfg.forceMaxRefresh) {
            DXGI_MODE_DESC m{};
            m.Width = desc->Width;
            m.Height = desc->Height;
            m.RefreshRate = fs->RefreshRate;
            PickMaxRefresh(hwnd, m);
            f2.RefreshRate = m.RefreshRate;
        }
        fs = &f2;
    }
    HRESULT hr = g_origCreateSwapChainForHwnd(f, dev, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) PatchVtable(*out, 14, (void*)&HookResizeTarget, (void**)&g_origResizeTarget);
    return hr;
}

static HRESULT WINAPI HookCreateDXGIFactory(REFIID riid, void** factory) {
    HRESULT hr = g_origCreateFactory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) {
        PatchVtable(*factory, 10, (void*)&HookCreateSwapChain, (void**)&g_origCreateSwapChain);
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(((IUnknown*)*factory)->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
            PatchVtable(f2, 15, (void*)&HookCreateSwapChainForHwnd, (void**)&g_origCreateSwapChainForHwnd);
            f2->Release();
        }
    }
    return hr;
}

void InstallDisplayHooks(uint8_t* image) {
    g_origCreateFactory = (CreateDXGIFactory_t)HookImport(image, "dxgi.dll", "CreateDXGIFactory", (void*)&HookCreateDXGIFactory);
    Log("CreateDXGIFactory hook %s", g_origCreateFactory ? "installed" : "NOT installed");
}
