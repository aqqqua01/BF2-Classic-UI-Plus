#include "config.h"
#include "patcher.h"
#include <windows.h>
#include <unknwn.h>
#include <mutex>

namespace {

HMODULE g_realDInput8 = nullptr;
std::once_flag g_realInitOnce;
std::once_flag g_modInitOnce;
ModConfig g_config;

using FnDirectInput8Create  = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using FnDllCanUnloadNow     = HRESULT(STDAPICALLTYPE*)();
using FnDllGetClassObject   = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
using FnDllRegisterServer   = HRESULT(STDAPICALLTYPE*)();
using FnDllUnregisterServer = HRESULT(STDAPICALLTYPE*)();
using FnGetdfDIJoystick     = const void*(WINAPI*)();

FnDirectInput8Create  g_DirectInput8Create  = nullptr;
FnDllCanUnloadNow     g_DllCanUnloadNow     = nullptr;
FnDllGetClassObject   g_DllGetClassObject   = nullptr;
FnDllRegisterServer   g_DllRegisterServer   = nullptr;
FnDllUnregisterServer g_DllUnregisterServer = nullptr;
FnGetdfDIJoystick     g_GetdfDIJoystick     = nullptr;

void EnsureRealDInput8Loaded() {
    std::call_once(g_realInitOnce, []() {
        char sysDir[MAX_PATH] = {};
        UINT n = GetSystemDirectoryA(sysDir, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 16) {
            std::string fullPath = std::string(sysDir) + "\\dinput8.dll";
            g_realDInput8 = LoadLibraryA(fullPath.c_str());
        }
        if (!g_realDInput8) {
            g_realDInput8 = LoadLibraryA("C:\\Windows\\SysWOW64\\dinput8.dll");
        }
        if (g_realDInput8) {
            g_DirectInput8Create  = reinterpret_cast<FnDirectInput8Create>(GetProcAddress(g_realDInput8, "DirectInput8Create"));
            g_DllCanUnloadNow     = reinterpret_cast<FnDllCanUnloadNow>(GetProcAddress(g_realDInput8, "DllCanUnloadNow"));
            g_DllGetClassObject   = reinterpret_cast<FnDllGetClassObject>(GetProcAddress(g_realDInput8, "DllGetClassObject"));
            g_DllRegisterServer   = reinterpret_cast<FnDllRegisterServer>(GetProcAddress(g_realDInput8, "DllRegisterServer"));
            g_DllUnregisterServer = reinterpret_cast<FnDllUnregisterServer>(GetProcAddress(g_realDInput8, "DllUnregisterServer"));
            g_GetdfDIJoystick     = reinterpret_cast<FnGetdfDIJoystick>(GetProcAddress(g_realDInput8, "GetdfDIJoystick"));
        }
    });
}

void EnsureModInitialized() {
    std::call_once(g_modInitOnce, []() {
        char exeName[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exeName, MAX_PATH);
        // Only activate inside BattlefrontII.exe
        const char* baseName = strrchr(exeName, '\\');
        baseName = baseName ? baseName + 1 : exeName;
        if (_stricmp(baseName, "BattlefrontII.exe") != 0) {
            return;
        }
        LogInit(GetGameDir() + "\\bf2_uifix.log");
        g_config = LoadOrCreateConfig();
        InstallEarlyHooksAndPatches(g_config);
    });
}

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    EnsureRealDInput8Loaded();
    EnsureModInitialized();
    if (!g_DirectInput8Create) return E_FAIL;
    return g_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

HRESULT STDAPICALLTYPE Proxy_DllCanUnloadNow() {
    EnsureRealDInput8Loaded();
    return g_DllCanUnloadNow ? g_DllCanUnloadNow() : S_FALSE;
}

HRESULT STDAPICALLTYPE Proxy_DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    EnsureRealDInput8Loaded();
    return g_DllGetClassObject ? g_DllGetClassObject(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT STDAPICALLTYPE Proxy_DllRegisterServer() {
    EnsureRealDInput8Loaded();
    return g_DllRegisterServer ? g_DllRegisterServer() : E_FAIL;
}

HRESULT STDAPICALLTYPE Proxy_DllUnregisterServer() {
    EnsureRealDInput8Loaded();
    return g_DllUnregisterServer ? g_DllUnregisterServer() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
    EnsureRealDInput8Loaded();
    return g_GetdfDIJoystick ? g_GetdfDIJoystick() : nullptr;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Safe under loader lock: only VirtualProtect/memcpy on already-mapped BattlefrontII.exe & INI I/O
        EnsureModInitialized();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_realDInput8) {
            FreeLibrary(g_realDInput8);
            g_realDInput8 = nullptr;
        }
    }
    return TRUE;
}
