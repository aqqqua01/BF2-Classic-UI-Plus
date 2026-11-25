#include "config.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_logMutex;
std::string g_logPath;
}

std::string GetGameDir() {
    char buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, len);
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        return path.substr(0, slash);
    }
    return ".";
}

void LogInit(const std::string& logPath) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logPath = logPath;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath.c_str(), "w") == 0 && f) {
        fprintf(f, "[BF2UIFix] Log initialized\n");
        fclose(f);
    }
}

void LogMsg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logPath.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath.c_str(), "a") == 0 && f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

void DetectDesktopResolution(uint32_t& outW, uint32_t& outH) {
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) {
        outW = dm.dmPelsWidth;
        outH = dm.dmPelsHeight;
        return;
    }
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    outW = (cx >= 640) ? static_cast<uint32_t>(cx) : 1920u;
    outH = (cy >= 480) ? static_cast<uint32_t>(cy) : 1080u;
}

static std::string GetIniPath() {
    return GetGameDir() + "\\bf2_uifix.ini";
}

const char* DisplayModeToString(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::Windowed:   return "windowed";
        case DisplayMode::Fullscreen: return "fullscreen";
        case DisplayMode::Borderless:
        default:                      return "borderless";
    }
}

DisplayMode ParseDisplayMode(const char* str) {
    if (!str || !*str) return DisplayMode::Borderless;
    if (_stricmp(str, "windowed") == 0 || _stricmp(str, "window") == 0 || std::strcmp(str, "1") == 0) {
        return DisplayMode::Windowed;
    }
    if (_stricmp(str, "fullscreen") == 0 || _stricmp(str, "exclusive") == 0 || std::strcmp(str, "2") == 0) {
        return DisplayMode::Fullscreen;
    }
    return DisplayMode::Borderless;
}

void ComputeWindowedSize(const ModConfig& cfg, uint32_t& outW, uint32_t& outH) {
    if (cfg.windowWidth >= 640 && cfg.windowHeight >= 480 &&
        cfg.windowWidth < cfg.desktopWidth && cfg.windowHeight < cfg.desktopHeight) {
        outW = cfg.windowWidth;
        outH = cfg.windowHeight;
        return;
    }
    if (cfg.width >= 640 && cfg.height >= 480 &&
        cfg.width < cfg.desktopWidth && cfg.height < cfg.desktopHeight) {
        outW = cfg.width;
        outH = cfg.height;
        return;
    }
    // If target resolution equals desktop, pick a comfortable centered window size matching monitor aspect
    uint32_t baseH = (cfg.desktopHeight >= 1400) ? 1080u :
                     (cfg.desktopHeight >= 1000) ? 900u : 720u;
    if (baseH >= cfg.desktopHeight) {
        baseH = (cfg.desktopHeight * 80) / 100;
    }
    uint32_t baseW = static_cast<uint32_t>(
        (static_cast<double>(cfg.desktopWidth) * static_cast<double>(baseH)) /
        static_cast<double>(cfg.desktopHeight) + 0.5);
    baseW &= ~1u;
    baseH &= ~1u;
    outW = (baseW >= 640) ? baseW : 1280u;
    outH = (baseH >= 480) ? baseH : 720u;
}

void SaveConfig(const ModConfig& cfg) {
    std::string ini = GetIniPath();
    FILE* f = nullptr;
    if (fopen_s(&f, ini.c_str(), "w") != 0 || !f) {
        LogMsg("[Config] Failed to write %s", ini.c_str());
        return;
    }
    fprintf(f,
        "[Display]\n"
        "; AutoDetect=1 automatically picks your monitor resolution (e.g. %ux%u).\n"
        "; If you change resolution in the game's Video menu, AutoDetect becomes 0 and your choice is saved.\n"
        "AutoDetect=%d\n"
        "Width=%u\n"
        "Height=%u\n"
        "; DisplayMode: borderless (no borders, full screen), windowed (draggable window), fullscreen (exclusive).\n"
        "; Press F11 or Alt+Enter in-game to cycle modes live.\n"
        "DisplayMode=%s\n"
        "; Optional custom size used in windowed mode (0 = auto-fit inside monitor).\n"
        "WindowWidth=%u\n"
        "WindowHeight=%u\n"
        "; FixShellResolution=1 unlocks the Main Menu from 800x600 to match your resolution.\n"
        "FixShellResolution=%d\n"
        "; FixWidescreenUI=1 scales the 2D UI & fonts cleanly instead of shrinking them at 1440p/4K.\n"
        "FixWidescreenUI=%d\n"
        "; UpscaleFonts=1 upscales bitmap UI font atlases 4x with crisp anti-aliased edges.\n"
        "UpscaleFonts=%d\n"
        "; UIHeight sets the virtual UI height while keeping 3D rendering at full Width x Height.\n"
        "; 720 = crisp 2x UI at 1440p (recommended), 600 = classic larger UI, 0 = native 1:1 pixels.\n"
        "UIHeight=%u\n"
        "; SafeZoneScale controls HUD & menu edge margin (0.88 = comfortable top/side margin, 0.90 = vanilla).\n"
        "SafeZoneScale=%.2f\n"
        "; SteamOnly=1 requires a legitimate purchased Steam copy (AppID 6060).\n"
        "SteamOnly=%d\n",
        cfg.desktopWidth, cfg.desktopHeight,
        cfg.autoDetect,
        cfg.width,
        cfg.height,
        DisplayModeToString(cfg.displayMode),
        cfg.windowWidth,
        cfg.windowHeight,
        cfg.fixShellResolution,
        cfg.fixWidescreenUI,
        cfg.upscaleFonts,
        cfg.uiHeight,
        cfg.safeZoneScale,
        cfg.steamOnly);
    fclose(f);
    LogMsg("[Config] Saved %s (AutoDetect=%d, %ux%u, Mode=%s, UIHeight=%u, UpscaleFonts=%d, SafeZone=%.2f)",
           ini.c_str(), cfg.autoDetect, cfg.width, cfg.height, DisplayModeToString(cfg.displayMode),
           cfg.uiHeight, cfg.upscaleFonts, cfg.safeZoneScale);
}

ModConfig LoadOrCreateConfig() {
    ModConfig cfg;
    DetectDesktopResolution(cfg.desktopWidth, cfg.desktopHeight);
    cfg.width = cfg.desktopWidth;
    cfg.height = cfg.desktopHeight;

    std::string ini = GetIniPath();
    DWORD attrs = GetFileAttributesA(ini.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        LogMsg("[Config] No bf2_uifix.ini found; auto-detected desktop %ux%u", cfg.desktopWidth, cfg.desktopHeight);
        SaveConfig(cfg);
        return cfg;
    }

    cfg.autoDetect = GetPrivateProfileIntA("Display", "AutoDetect", 1, ini.c_str());
    UINT w = GetPrivateProfileIntA("Display", "Width", cfg.desktopWidth, ini.c_str());
    UINT h = GetPrivateProfileIntA("Display", "Height", cfg.desktopHeight, ini.c_str());

    char modeBuf[32] = {};
    GetPrivateProfileStringA("Display", "DisplayMode", "", modeBuf, sizeof(modeBuf), ini.c_str());
    bool missingDisplayMode = (modeBuf[0] == '\0');
    if (!missingDisplayMode) {
        cfg.displayMode = ParseDisplayMode(modeBuf);
    } else {
        int legacyBorderless = GetPrivateProfileIntA("Display", "Borderless", 1, ini.c_str());
        cfg.displayMode = legacyBorderless ? DisplayMode::Borderless : DisplayMode::Fullscreen;
    }

    UINT winW = GetPrivateProfileIntA("Display", "WindowWidth", 0, ini.c_str());
    UINT winH = GetPrivateProfileIntA("Display", "WindowHeight", 0, ini.c_str());
    if (winW >= 640 && winH >= 480) {
        cfg.windowWidth = winW;
        cfg.windowHeight = winH;
    }

    cfg.fixShellResolution = GetPrivateProfileIntA("Display", "FixShellResolution", 1, ini.c_str());
    cfg.fixWidescreenUI = GetPrivateProfileIntA("Display", "FixWidescreenUI", 1, ini.c_str());
    UINT uf = GetPrivateProfileIntA("Display", "UpscaleFonts", 999999, ini.c_str());
    bool missingUpscaleFonts = (uf == 999999);
    if (!missingUpscaleFonts) cfg.upscaleFonts = (uf != 0) ? 1 : 0;
    UINT uiH = GetPrivateProfileIntA("Display", "UIHeight", 999999, ini.c_str());
    bool missingUiHeight = (uiH == 999999);
    if (!missingUiHeight && (uiH == 0 || (uiH >= 480 && uiH <= 2160))) {
        cfg.uiHeight = uiH;
    }
    cfg.steamOnly = GetPrivateProfileIntA("Display", "SteamOnly", 1, ini.c_str());

    char szBuf[32] = {};
    GetPrivateProfileStringA("Display", "SafeZoneScale", "0.88", szBuf, sizeof(szBuf), ini.c_str());
    float sz = static_cast<float>(atof(szBuf));
    if (sz >= 0.70f && sz <= 1.00f) {
        cfg.safeZoneScale = sz;
    }

    if (cfg.autoDetect) {
        bool changed = (w != cfg.desktopWidth || h != cfg.desktopHeight ||
                        missingUiHeight || missingUpscaleFonts || missingDisplayMode);
        cfg.width = cfg.desktopWidth;
        cfg.height = cfg.desktopHeight;
        if (changed) {
            SaveConfig(cfg);
        }
    } else {
        if (w >= 640 && h >= 480) {
            cfg.width = w;
            cfg.height = h;
        }
        if (missingUiHeight || missingUpscaleFonts || missingDisplayMode) {
            SaveConfig(cfg);
        }
    }

    LogMsg("[Config] Loaded: AutoDetect=%d, Target=%ux%u, Desktop=%ux%u, Mode=%s, UIHeight=%u, UpscaleFonts=%d, SafeZone=%.2f, SteamOnly=%d",
           cfg.autoDetect, cfg.width, cfg.height, cfg.desktopWidth, cfg.desktopHeight,
           DisplayModeToString(cfg.displayMode), cfg.uiHeight, cfg.upscaleFonts, cfg.safeZoneScale, cfg.steamOnly);
    return cfg;
}
