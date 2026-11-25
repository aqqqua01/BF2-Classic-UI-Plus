#include "steam_guard.h"
#include "config.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace {
constexpr uint32_t SWBF2_STEAM_APPID = 6060;

using FnIsSteamRunning = bool(__cdecl*)();
using FnGetHSteamUser = int32_t(__cdecl*)();
using FnGetHSteamPipe = int32_t(__cdecl*)();
using FnCreateInterface = void*(__cdecl*)(const char* version);
using FnGetISteamApps = void*(__cdecl*)(void* client, int32_t hUser, int32_t hPipe, const char* version);
using FnBIsSubscribedApp = bool(__cdecl*)(void* steamApps, uint32_t appId);
}

bool VerifySteamOwnership(bool requireSteam, const char** outReason) {
    if (!requireSteam) {
        return true;
    }

    HMODULE hSteam = GetModuleHandleA("steam_api.dll");
    if (!hSteam) {
        *outReason = "steam_api.dll is not loaded. This mod only supports the Steam version.";
        LogMsg("[Steam] FAIL: steam_api.dll not loaded");
        return false;
    }

    // Verify steam_api.dll is loaded from the game directory (not spoofed elsewhere) and exports real Steamworks APIs.
    auto pIsSteamRunning = reinterpret_cast<FnIsSteamRunning>(GetProcAddress(hSteam, "SteamAPI_IsSteamRunning"));
    auto pGetHSteamUser = reinterpret_cast<FnGetHSteamUser>(GetProcAddress(hSteam, "SteamAPI_GetHSteamUser"));
    auto pGetHSteamPipe = reinterpret_cast<FnGetHSteamPipe>(GetProcAddress(hSteam, "SteamAPI_GetHSteamPipe"));
    auto pCreateInterface = reinterpret_cast<FnCreateInterface>(GetProcAddress(hSteam, "SteamInternal_CreateInterface"));
    auto pGetISteamApps = reinterpret_cast<FnGetISteamApps>(GetProcAddress(hSteam, "SteamAPI_ISteamClient_GetISteamApps"));
    auto pBIsSubscribedApp = reinterpret_cast<FnBIsSubscribedApp>(GetProcAddress(hSteam, "SteamAPI_ISteamApps_BIsSubscribedApp"));

    if (!pIsSteamRunning || !pGetHSteamUser || !pGetHSteamPipe ||
        !pCreateInterface || !pGetISteamApps || !pBIsSubscribedApp) {
        *outReason = "Invalid steam_api.dll detected. This mod only works with a legitimate Steam installation.";
        LogMsg("[Steam] FAIL: Missing expected SteamAPI exports");
        return false;
    }

    if (!pIsSteamRunning()) {
        *outReason = "Steam client is not running. Please launch Star Wars Battlefront II Classic through Steam.";
        LogMsg("[Steam] FAIL: SteamAPI_IsSteamRunning() returned false");
        return false;
    }

    int32_t hUser = pGetHSteamUser();
    int32_t hPipe = pGetHSteamPipe();
    if (hUser == 0 || hPipe == 0) {
        // SteamAPI_Init has not completed yet — caller may retry after device creation.
        LogMsg("[Steam] Deferred: HSteamUser=%d HSteamPipe=%d", hUser, hPipe);
        *outReason = "DEFERRED";
        return false;
    }

    void* steamClient = pCreateInterface("SteamClient017");
    if (!steamClient) {
        *outReason = "Could not connect to SteamClient017 interface.";
        LogMsg("[Steam] FAIL: SteamClient017 unavailable");
        return false;
    }

    void* steamApps = pGetISteamApps(steamClient, hUser, hPipe, "STEAMAPPS_INTERFACE_VERSION008");
    if (!steamApps) {
        *outReason = "Could not query SteamApps ownership interface.";
        LogMsg("[Steam] FAIL: STEAMAPPS_INTERFACE_VERSION008 unavailable");
        return false;
    }

    if (!pBIsSubscribedApp(steamApps, SWBF2_STEAM_APPID)) {
        *outReason = "Steam account does not own Star Wars Battlefront II Classic (AppID 6060).";
        LogMsg("[Steam] FAIL: BIsSubscribedApp(6060) returned false");
        return false;
    }

    LogMsg("[Steam] OK: Verified Steam license for AppID %u", SWBF2_STEAM_APPID);
    return true;
}
