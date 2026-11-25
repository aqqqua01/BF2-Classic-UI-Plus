#include "patcher.h"
#include "font_upscale.h"
#include "lua_menu.h"
#include "steam_guard.h"
#include <d3d9.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

constexpr uintptr_t ORIG_BASE = 0x00400000;

// Verified RVAs from BattlefrontII.exe (Steam/GOG 2017 build, timestamp 1508762451)
constexpr uintptr_t RVA_SHELL_RES_GUARD       = 0x0021CC8F; // 84 DB 75 1E (test bl,bl; jne +0x1E)
constexpr uintptr_t RVA_INIT_PP_WIDTH_MOV     = 0x002B23BA; // C7 05 CC 5E 7F 00 [20 03 00 00]
constexpr uintptr_t RVA_INIT_PP_HEIGHT_MOV    = 0x002B23C4; // C7 05 D0 5E 7F 00 [58 02 00 00]
constexpr uintptr_t RVA_SAFEZONE_MOV          = 0x002B6C0B; // C7 04 24 [66 66 66 3F]
constexpr uintptr_t RVA_SET_UISIZE_CALL       = 0x002B6BF6; // E8 C5 A8 00 00 -> call 006C14C0
constexpr uintptr_t RVA_SET_SIZE_FN           = 0x002C14C0; // void __thiscall(int* outWH, int w, int h)
constexpr uintptr_t RVA_WINDOWED_STYLE_MOV    = 0x002B1B5C; // B9 [00 00 C4 00]
constexpr uintptr_t RVA_ENGINE_RESET_FN       = 0x002B68A0; // void __cdecl RecoverDeviceAndReset()
constexpr uintptr_t RVA_PRESENT_PARAMS        = 0x003F5ECC; // D3DPRESENT_PARAMETERS global
constexpr uintptr_t RVA_IDEAL_WIDTH           = 0x003E107C; // uint32_t = 800
constexpr uintptr_t RVA_IDEAL_HEIGHT          = 0x003E1080; // uint32_t = 600
constexpr uintptr_t RVA_SCRIPTCB_SETRES       = 0x00199B50; // 55 8B EC 51 56 (5 bytes)
constexpr uintptr_t RVA_SCRIPTCB_SETVIDOPTS   = 0x0019A5E0; // 55 8B EC 83 E4 F8 (6 bytes)
constexpr uintptr_t RVA_GET_ACTIVE_PROFILE    = 0x0021D5F0; // void* __cdecl GetActiveProfile()
constexpr uintptr_t RVA_SCREEN_WIDTH          = 0x0053E4A4;
constexpr uintptr_t RVA_SCREEN_HEIGHT         = 0x0053E4A8;

constexpr uintptr_t PROFILE_RES_WIDTH_OFF     = 0x0F96;
constexpr uintptr_t PROFILE_RES_HEIGHT_OFF    = 0x0F98;

ModConfig* g_cfg = nullptr;
uintptr_t g_exeBase = 0;

using FnDirect3DCreate9 = IDirect3D9*(WINAPI*)(UINT SDKVersion);
using FnCreateDevice = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface);
using FnReset = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPresentationParameters);
using FnPresent = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self, CONST RECT* pSourceRect, CONST RECT* pDestRect,
    HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion);
using FnScriptCB = int(__cdecl*)();
using FnGetActiveProfile = uint8_t*(__cdecl*)();
using FnSetScreenSize = void(__thiscall*)(void* outWH, uint32_t w, uint32_t h);
using FnSetWindowPos = BOOL(WINAPI*)(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
using FnEngineReset = void(__cdecl*)();

FnDirect3DCreate9 g_origDirect3DCreate9 = nullptr;
FnCreateDevice    g_origCreateDevice = nullptr;
FnReset           g_origReset = nullptr;
FnPresent         g_origPresent = nullptr;
FnScriptCB        g_origSetRes = nullptr;
FnScriptCB        g_origSetVidOpts = nullptr;
FnSetScreenSize   g_origSetScreenSize = nullptr;
FnSetWindowPos    g_origSetWindowPos = nullptr;

HWND g_focusHwnd = nullptr;
bool g_steamChecked = false;
bool g_steamValid = false;
std::atomic<bool> g_inUserResChange{false};
bool g_deviceInitialized = false;
bool g_hotkeyPrevDown = false;
uint32_t g_currentRenderW = 0;
uint32_t g_currentRenderH = 0;
std::atomic<int> g_pendingDisplayMode{-1};

bool WriteMem(uintptr_t addr, const void* bytes, size_t size) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(addr), bytes, size);
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), size, oldProt, &dummy);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), size);
    return true;
}

template <typename T>
bool PatchVtableSlot(void* comObj, size_t slotIndex, T newFn, T* outOrig) {
    void** vtable = *reinterpret_cast<void***>(comObj);
    if (!vtable) return false;
    if (vtable[slotIndex] == reinterpret_cast<void*>(newFn)) {
        return true;
    }
    if (outOrig && !*outOrig) {
        *outOrig = reinterpret_cast<T>(vtable[slotIndex]);
    }
    void* fnPtr = reinterpret_cast<void*>(newFn);
    return WriteMem(reinterpret_cast<uintptr_t>(&vtable[slotIndex]), &fnPtr, sizeof(fnPtr));
}

void SyncProfileResolution(uint32_t w, uint32_t h) {
    auto getProfile = reinterpret_cast<FnGetActiveProfile>(g_exeBase + RVA_GET_ACTIVE_PROFILE);
    uint8_t* prof = getProfile ? getProfile() : nullptr;
    if (!prof) return;
    auto* pw = reinterpret_cast<uint16_t*>(prof + PROFILE_RES_WIDTH_OFF);
    auto* ph = reinterpret_cast<uint16_t*>(prof + PROFILE_RES_HEIGHT_OFF);
    if (*pw != static_cast<uint16_t>(w) || *ph != static_cast<uint16_t>(h)) {
        LogMsg("[Profile] Updating active profile resolution %ux%u -> %ux%u", *pw, *ph, w, h);
        *pw = static_cast<uint16_t>(w);
        *ph = static_cast<uint16_t>(h);
    }
}

void UpdateIdealResolutionGlobals(uint32_t w, uint32_t h) {
    WriteMem(g_exeBase + RVA_IDEAL_WIDTH, &w, sizeof(w));
    WriteMem(g_exeBase + RVA_IDEAL_HEIGHT, &h, sizeof(h));
}

void UpdateWindowedStylePatch(DisplayMode mode) {
    // 0x006B1B5C: B9 [style] (mov ecx, style; used when Windowed != 0)
    const uint8_t movPrefix = 0xB9;
    if (CheckBytes(g_exeBase + RVA_WINDOWED_STYLE_MOV, &movPrefix, 1)) {
        uint32_t styleVal = (mode == DisplayMode::Borderless)
            ? (WS_POPUP | WS_VISIBLE)
            : (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
        WriteMem(g_exeBase + RVA_WINDOWED_STYLE_MOV + 1, &styleVal, 4);
    }
}

uint32_t QueryCurrentDisplayRefreshRate() {
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30) {
        return dm.dmDisplayFrequency;
    }
    return 60;
}

void CallSetWindowPosRaw(HWND hwnd, HWND insertAfter, int x, int y, int cx, int cy, UINT flags) {
    if (g_origSetWindowPos) {
        g_origSetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);
    } else {
        SetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);
    }
}

void ApplyWindowForDisplayMode(HWND hwnd, uint32_t renderW, uint32_t renderH) {
    if (!hwnd || !IsWindow(hwnd) || !g_cfg) return;

    LONG curStyle = GetWindowLongA(hwnd, GWL_STYLE);
    if (g_cfg->displayMode == DisplayMode::Borderless) {
        LONG desired = (curStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE;
        if (curStyle != desired) SetWindowLongA(hwnd, GWL_STYLE, desired);
        uint32_t winW = g_cfg->desktopWidth ? g_cfg->desktopWidth : renderW;
        uint32_t winH = g_cfg->desktopHeight ? g_cfg->desktopHeight : renderH;
        CallSetWindowPosRaw(hwnd, HWND_NOTOPMOST, 0, 0, static_cast<int>(winW), static_cast<int>(winH),
                            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowTextA(hwnd, "Star Wars Battlefront II");
    } else if (g_cfg->displayMode == DisplayMode::Windowed) {
        LONG desired = (curStyle & ~(WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        if (curStyle != desired) SetWindowLongA(hwnd, GWL_STYLE, desired);
        RECT rc = { 0, 0, static_cast<LONG>(renderW), static_cast<LONG>(renderH) };
        AdjustWindowRect(&rc, desired, FALSE);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;
        int x = (static_cast<int>(g_cfg->desktopWidth) - winW) / 2;
        int y = (static_cast<int>(g_cfg->desktopHeight) - winH) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        CallSetWindowPosRaw(hwnd, HWND_NOTOPMOST, x, y, winW, winH,
                            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowTextA(hwnd, "Star Wars Battlefront II [Windowed]");
    } else {
        // Exclusive fullscreen: let D3D9 / dgVoodoo manage window & mode
        SetWindowTextA(hwnd, "Star Wars Battlefront II");
    }
}

BOOL WINAPI Hooked_SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
    if (g_cfg && g_cfg->desktopWidth && g_cfg->desktopHeight) {
        char cls[64] = {};
        bool isGameWnd = (hWnd == g_focusHwnd) ||
                         (GetClassNameA(hWnd, cls, sizeof(cls)) > 0 && std::strcmp(cls, "Star Wars Battlefront II") == 0);
        if (isGameWnd && g_cfg->displayMode == DisplayMode::Borderless) {
            X = 0;
            Y = 0;
            cx = static_cast<int>(g_cfg->desktopWidth);
            cy = static_cast<int>(g_cfg->desktopHeight);
        }
    }
    return g_origSetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

void EnforceSteamOrExit() {
    if (g_steamChecked && g_steamValid) return;
    const char* reason = nullptr;
    if (VerifySteamOwnership(g_cfg->steamOnly != 0, &reason)) {
        g_steamChecked = true;
        g_steamValid = true;
        return;
    }
    if (reason && std::strcmp(reason, "DEFERRED") == 0) {
        return;
    }
    g_steamChecked = true;
    g_steamValid = false;
    LogMsg("[Steam] Blocking mod execution: %s", reason ? reason : "Unknown error");
    MessageBoxA(
        g_focusHwnd,
        reason ? reason : "This mod requires a legitimate Steam copy of Star Wars Battlefront II Classic.",
        "BF2 UI Fix — Steam Required",
        MB_OK | MB_ICONERROR | MB_TOPMOST);
    ExitProcess(1);
}

void AdjustPresentParams(D3DPRESENT_PARAMETERS* pp, bool isReset) {
    if (!pp || !g_cfg) return;

    uint32_t reqW = pp->BackBufferWidth;
    uint32_t reqH = pp->BackBufferHeight;

    if (isReset && g_deviceInitialized && g_inUserResChange.load()) {
        // User explicitly applied a new resolution in Video Options!
        if (reqW >= 640 && reqH >= 480) {
            g_cfg->width = reqW;
            g_cfg->height = reqH;
            g_cfg->autoDetect = (reqW == g_cfg->desktopWidth && reqH == g_cfg->desktopHeight) ? 1 : 0;
            if (g_cfg->displayMode == DisplayMode::Windowed &&
                reqW < g_cfg->desktopWidth && reqH < g_cfg->desktopHeight) {
                g_cfg->windowWidth = reqW;
                g_cfg->windowHeight = reqH;
            }
            SaveConfig(*g_cfg);
            UpdateIdealResolutionGlobals(reqW, reqH);
            LogMsg("[Display] User selected new resolution in-game: %ux%u", reqW, reqH);
        }
    } else if (g_cfg->fixShellResolution) {
        uint32_t targetW = g_cfg->width;
        uint32_t targetH = g_cfg->height;
        if (g_cfg->displayMode == DisplayMode::Windowed) {
            ComputeWindowedSize(*g_cfg, targetW, targetH);
        }

        bool isLowMenuFallback = (reqW <= 800 && reqH <= 600);
        if (isLowMenuFallback || !g_deviceInitialized || g_cfg->autoDetect ||
            (g_cfg->displayMode == DisplayMode::Windowed && (reqW != targetW || reqH != targetH))) {
            if (reqW != targetW || reqH != targetH) {
                LogMsg("[Display] Overriding %ux%u -> %ux%u (%s, mode=%s)",
                       reqW, reqH, targetW, targetH,
                       isReset ? "Reset" : "CreateDevice",
                       DisplayModeToString(g_cfg->displayMode));
                pp->BackBufferWidth = targetW;
                pp->BackBufferHeight = targetH;
            }
        }
    }

    if (g_cfg->displayMode == DisplayMode::Fullscreen) {
        pp->Windowed = FALSE;
        if (pp->FullScreen_RefreshRateInHz == 0) {
            pp->FullScreen_RefreshRateInHz = QueryCurrentDisplayRefreshRate();
        }
    } else {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }

    g_currentRenderW = pp->BackBufferWidth;
    g_currentRenderH = pp->BackBufferHeight;
}

void ApplyDisplayModeLive(IDirect3DDevice9* dev, DisplayMode nextMode, const char* sourceTag) {
    if (!g_cfg || !dev) return;

    g_cfg->displayMode = nextMode;
    UpdateWindowedStylePatch(nextMode);
    SaveConfig(*g_cfg);
    LogMsg("[Display] %s switched DisplayMode -> %s", sourceTag, DisplayModeToString(nextMode));

    auto* pp = reinterpret_cast<D3DPRESENT_PARAMETERS*>(g_exeBase + RVA_PRESENT_PARAMS);
    uint32_t targetW = g_cfg->width;
    uint32_t targetH = g_cfg->height;
    if (nextMode == DisplayMode::Windowed) {
        ComputeWindowedSize(*g_cfg, targetW, targetH);
    }

    pp->BackBufferWidth = targetW;
    pp->BackBufferHeight = targetH;
    if (nextMode == DisplayMode::Fullscreen) {
        pp->Windowed = FALSE;
        pp->FullScreen_RefreshRateInHz = QueryCurrentDisplayRefreshRate();
    } else {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }

    // Trigger engine's own safe device lost -> Reset -> restore sequence
    auto engineReset = reinterpret_cast<FnEngineReset>(g_exeBase + RVA_ENGINE_RESET_FN);
    if (engineReset) {
        engineReset();
    }
}

void CycleDisplayModeLive(IDirect3DDevice9* dev) {
    if (!g_cfg || !dev) return;

    DisplayMode nextMode = DisplayMode::Borderless;
    if (g_cfg->displayMode == DisplayMode::Borderless)      nextMode = DisplayMode::Windowed;
    else if (g_cfg->displayMode == DisplayMode::Windowed)   nextMode = DisplayMode::Fullscreen;
    else                                                    nextMode = DisplayMode::Borderless;

    ApplyDisplayModeLive(dev, nextMode, "Live hotkey");
}

HRESULT STDMETHODCALLTYPE Hooked_Present(
    IDirect3DDevice9* self, CONST RECT* pSourceRect, CONST RECT* pDestRect,
    HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
    HRESULT hr = g_origPresent(self, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

    int pending = g_pendingDisplayMode.exchange(-1);
    if (pending >= 0 && pending <= 2) {
        auto nextMode = static_cast<DisplayMode>(pending);
        if (g_cfg && g_cfg->displayMode != nextMode) {
            ApplyDisplayModeLive(self, nextMode, "Video menu");
        }
    }

    HWND fg = GetForegroundWindow();
    if (g_focusHwnd && fg == g_focusHwnd) {
        bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        bool altEnter = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                        ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0);
        bool pressed = f11 || altEnter;
        if (pressed && !g_hotkeyPrevDown) {
            g_hotkeyPrevDown = true;
            CycleDisplayModeLive(self);
        } else if (!pressed) {
            g_hotkeyPrevDown = false;
        }
    } else {
        g_hotkeyPrevDown = false;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_Reset(
    IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    EnforceSteamOrExit();
    AdjustPresentParams(pPresentationParameters, true);
    HRESULT hr = g_origReset(self, pPresentationParameters);
    if (SUCCEEDED(hr) && pPresentationParameters) {
        OnDeviceResetForFontUpscaler();
        if (g_cfg->autoDetect && !g_inUserResChange.load() && g_cfg->displayMode != DisplayMode::Windowed) {
            SyncProfileResolution(g_cfg->width, g_cfg->height);
        }
        HWND targetWnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : g_focusHwnd;
        ApplyWindowForDisplayMode(targetWnd, pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface) {
    g_focusHwnd = hFocusWindow;
    EnforceSteamOrExit();
    AdjustPresentParams(pPresentationParameters, false);

    HRESULT hr = g_origCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                    pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        g_deviceInitialized = true;
        PatchVtableSlot(*ppReturnedDeviceInterface, 16, Hooked_Reset, &g_origReset);
        PatchVtableSlot(*ppReturnedDeviceInterface, 17, Hooked_Present, &g_origPresent);
        InstallFontUpscalerHooks(*ppReturnedDeviceInterface, g_cfg->upscaleFonts != 0);
        if (pPresentationParameters) {
            HWND targetWnd = pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow : hFocusWindow;
            ApplyWindowForDisplayMode(targetWnd, pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
        }
        if (g_cfg->autoDetect && g_cfg->displayMode != DisplayMode::Windowed) {
            SyncProfileResolution(g_cfg->width, g_cfg->height);
        }
    }
    return hr;
}

IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT SDKVersion) {
    EnforceSteamOrExit();
    IDirect3D9* d3d = g_origDirect3DCreate9(SDKVersion);
    if (d3d) {
        // IDirect3D9::CreateDevice is vtable slot 16
        PatchVtableSlot(d3d, 16, Hooked_CreateDevice, &g_origCreateDevice);
        LogMsg("[D3D9] Hooked IDirect3D9::CreateDevice");
    }
    return d3d;
}

void SaveIfUserChangedProfileResolution() {
    auto getProfile = reinterpret_cast<FnGetActiveProfile>(g_exeBase + RVA_GET_ACTIVE_PROFILE);
    if (!getProfile) return;
    if (uint8_t* prof = getProfile()) {
        uint16_t w = *reinterpret_cast<uint16_t*>(prof + PROFILE_RES_WIDTH_OFF);
        uint16_t h = *reinterpret_cast<uint16_t*>(prof + PROFILE_RES_HEIGHT_OFF);
        if (w >= 640 && h >= 480 && (w != g_cfg->width || h != g_cfg->height)) {
            g_cfg->width = w;
            g_cfg->height = h;
            g_cfg->autoDetect = (w == g_cfg->desktopWidth && h == g_cfg->desktopHeight) ? 1 : 0;
            SaveConfig(*g_cfg);
            UpdateIdealResolutionGlobals(w, h);
        }
    }
}

int __cdecl Hooked_ScriptCB_SetResolution() {
    g_inUserResChange.store(true);
    int ret = g_origSetRes();
    SaveIfUserChangedProfileResolution();
    g_inUserResChange.store(false);
    return ret;
}

int __cdecl Hooked_ScriptCB_SetPCVideoOptions() {
    g_inUserResChange.store(true);
    int ret = g_origSetVidOpts();
    SaveIfUserChangedProfileResolution();
    g_inUserResChange.store(false);
    return ret;
}

void __fastcall Hooked_SetUIScreenSize(void* outWH, void* /*edx*/, uint32_t physW, uint32_t physH) {
    uint32_t uiW = physW;
    uint32_t uiH = physH;
    if (g_cfg && g_cfg->fixWidescreenUI && g_cfg->uiHeight >= 480 && physH > g_cfg->uiHeight) {
        uiH = g_cfg->uiHeight;
        uiW = static_cast<uint32_t>((static_cast<double>(physW) * static_cast<double>(uiH)) / static_cast<double>(physH) + 0.5);
        // Keep even width for clean center alignment
        uiW &= ~1u;
        LogMsg("[UI] Virtual UI canvas: %ux%u (Physical 3D: %ux%u, Scale: %.2fx)",
               uiW, uiH, physW, physH, static_cast<double>(physH) / static_cast<double>(uiH));
    }
    g_origSetScreenSize(outWH, uiW, uiH);
}

bool HookIAT(uintptr_t modBase, const char* dllName, const char* funcName, void* hookFn, void** outOrig) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(modBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(modBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(modBase + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* curDll = reinterpret_cast<const char*>(modBase + imp->Name);
        if (_stricmp(curDll, dllName) != 0) continue;

        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            modBase + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(modBase + imp->FirstThunk);

        for (; origThunk->u1.AddressOfData; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(origThunk->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(modBase + origThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) == 0) {
                if (outOrig) *outOrig = reinterpret_cast<void*>(iatThunk->u1.Function);
                void* pHook = hookFn;
                return WriteMem(reinterpret_cast<uintptr_t>(&iatThunk->u1.Function), &pHook, sizeof(pHook));
            }
        }
    }
    return false;
}

void ApplyStaticMemoryPatches() {
    if (g_cfg->fixShellResolution) {
        // 1. Allow Main Menu (Shell) to use profile/configured resolution instead of forcing 800x600
        const uint8_t shellSig[4] = { 0x84, 0xDB, 0x75, 0x1E };
        if (CheckBytes(g_exeBase + RVA_SHELL_RES_GUARD, shellSig, 4)) {
            const uint8_t nops[2] = { 0x90, 0x90 };
            WriteMem(g_exeBase + RVA_SHELL_RES_GUARD + 2, nops, 2);
            LogMsg("[Patch] Unlocked Shell resolution guard");
        } else {
            LogMsg("[Patch] WARN: Shell resolution guard signature mismatch");
        }

        // 2. Patch initial PresentParams defaults (800x600 -> targetW x targetH)
        const uint8_t initSigW[2] = { 0xC7, 0x05 };
        const uint8_t initValW[4] = { 0x20, 0x03, 0x00, 0x00 }; // 800
        const uint8_t initValH[4] = { 0x58, 0x02, 0x00, 0x00 }; // 600
        if (CheckBytes(g_exeBase + RVA_INIT_PP_WIDTH_MOV, initSigW, 2) &&
            CheckBytes(g_exeBase + RVA_INIT_PP_WIDTH_MOV + 6, initValW, 4) &&
            CheckBytes(g_exeBase + RVA_INIT_PP_HEIGHT_MOV, initSigW, 2) &&
            CheckBytes(g_exeBase + RVA_INIT_PP_HEIGHT_MOV + 6, initValH, 4)) {
            WriteMem(g_exeBase + RVA_INIT_PP_WIDTH_MOV + 6, &g_cfg->width, 4);
            WriteMem(g_exeBase + RVA_INIT_PP_HEIGHT_MOV + 6, &g_cfg->height, 4);
            LogMsg("[Patch] Initial PresentParams set to %ux%u", g_cfg->width, g_cfg->height);
        }

        // 3. Update IdealResolution globals
        UpdateIdealResolutionGlobals(g_cfg->width, g_cfg->height);
    }

    if (g_cfg->fixWidescreenUI) {
        // 4. Patch SafeZone scale (default 0.90f -> configured e.g. 0.92f)
        const uint8_t szSig[7] = { 0xC7, 0x04, 0x24, 0x66, 0x66, 0x66, 0x3F };
        if (CheckBytes(g_exeBase + RVA_SAFEZONE_MOV, szSig, 7)) {
            WriteMem(g_exeBase + RVA_SAFEZONE_MOV + 3, &g_cfg->safeZoneScale, sizeof(float));
            LogMsg("[Patch] SafeZoneScale set to %.2f", g_cfg->safeZoneScale);
        }

        // 4b. Hook the 2D UI size setter at 0x006B6BF6 (keeps 3D backbuffer/viewport at full resolution while scaling UI/fonts)
        const uint8_t uiCallSig[5] = { 0xE8, 0xC5, 0xA8, 0x00, 0x00 };
        if (CheckBytes(g_exeBase + RVA_SET_UISIZE_CALL, uiCallSig, 5)) {
            g_origSetScreenSize = reinterpret_cast<FnSetScreenSize>(g_exeBase + RVA_SET_SIZE_FN);
            int32_t rel = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(&Hooked_SetUIScreenSize) - (g_exeBase + RVA_SET_UISIZE_CALL + 5));
            WriteMem(g_exeBase + RVA_SET_UISIZE_CALL + 1, &rel, 4);
            LogMsg("[Hook] Hooked 2D UI virtual canvas scaler (UIHeight=%u)", g_cfg->uiHeight);
        } else {
            LogMsg("[Hook] WARN: 2D UI size setter signature mismatch");
        }
    }

    // 5. Configure initial window style for DisplayMode (borderless / windowed / fullscreen)
    UpdateWindowedStylePatch(g_cfg->displayMode);
    LogMsg("[Patch] Initial DisplayMode = %s", DisplayModeToString(g_cfg->displayMode));

    // 6. Hook ScriptCB_SetResolution & ScriptCB_SetPCVideoOptions to save in-game resolution changes
    const uint8_t setResSig[5] = { 0x55, 0x8B, 0xEC, 0x51, 0x56 };
    if (CheckBytes(g_exeBase + RVA_SCRIPTCB_SETRES, setResSig, 5)) {
        if (InstallTrampolineN(g_exeBase + RVA_SCRIPTCB_SETRES, 5,
                               reinterpret_cast<void*>(Hooked_ScriptCB_SetResolution),
                               reinterpret_cast<void**>(&g_origSetRes))) {
            LogMsg("[Hook] Hooked ScriptCB_SetResolution");
        }
    }
    const uint8_t setVidSig[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    if (CheckBytes(g_exeBase + RVA_SCRIPTCB_SETVIDOPTS, setVidSig, 6)) {
        if (InstallTrampolineN(g_exeBase + RVA_SCRIPTCB_SETVIDOPTS, 6,
                               reinterpret_cast<void*>(Hooked_ScriptCB_SetPCVideoOptions),
                               reinterpret_cast<void**>(&g_origSetVidOpts))) {
            LogMsg("[Hook] Hooked ScriptCB_SetPCVideoOptions");
        }
    }
}

} // namespace

bool CheckBytes(uintptr_t addr, const uint8_t* expected, size_t len) {
    return std::memcmp(reinterpret_cast<const void*>(addr), expected, len) == 0;
}

bool InstallTrampolineN(uintptr_t targetAddr, size_t copyBytes, void* hookFn, void** outTrampoline) {
    if (copyBytes < 5) return false;
    void* stub = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    uint8_t* stubBytes = static_cast<uint8_t*>(stub);
    std::memcpy(stubBytes, reinterpret_cast<void*>(targetAddr), copyBytes);
    stubBytes[copyBytes] = 0xE9; // jmp back to targetAddr + copyBytes
    int32_t relBack = static_cast<int32_t>((targetAddr + copyBytes) - (reinterpret_cast<uintptr_t>(stubBytes) + copyBytes + 5));
    std::memcpy(stubBytes + copyBytes + 1, &relBack, 4);

    uint8_t patch[16];
    std::memset(patch, 0x90, copyBytes);
    patch[0] = 0xE9;
    int32_t relHook = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hookFn) - (targetAddr + 5));
    std::memcpy(patch + 1, &relHook, 4);
    if (!WriteMem(targetAddr, patch, copyBytes)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    *outTrampoline = stub;
    return true;
}

int GetCurrentDisplayModeIndex() {
    int pending = g_pendingDisplayMode.load();
    if (pending >= 0 && pending <= 2) {
        return pending + 1;
    }
    if (!g_cfg) return 1;
    return static_cast<int>(g_cfg->displayMode) + 1;
}

void RequestDisplayModeFromMenu(int modeIndex) {
    if (modeIndex < 1 || modeIndex > 3 || !g_cfg) return;
    auto targetMode = static_cast<DisplayMode>(modeIndex - 1);
    if (targetMode != g_cfg->displayMode) {
        g_pendingDisplayMode.store(static_cast<int>(targetMode));
    } else {
        g_pendingDisplayMode.store(-1);
    }
}

void InstallEarlyHooksAndPatches(ModConfig& cfg) {
    g_cfg = &cfg;
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    LogMsg("[Init] BattlefrontII.exe base = 0x%08X", static_cast<uint32_t>(g_exeBase));

    ApplyStaticMemoryPatches();
    InstallLuaMenuHook(g_exeBase);

    if (HookIAT(g_exeBase, "d3d9.dll", "Direct3DCreate9",
                reinterpret_cast<void*>(Hooked_Direct3DCreate9),
                reinterpret_cast<void**>(&g_origDirect3DCreate9))) {
        LogMsg("[Hook] Hooked d3d9.dll!Direct3DCreate9 in IAT");
    } else {
        LogMsg("[Hook] WARN: Could not hook Direct3DCreate9 in IAT");
    }

    if (HookIAT(g_exeBase, "USER32.dll", "SetWindowPos",
                reinterpret_cast<void*>(Hooked_SetWindowPos),
                reinterpret_cast<void**>(&g_origSetWindowPos))) {
        LogMsg("[Hook] Hooked USER32.dll!SetWindowPos in IAT");
    }
}
