#pragma once
#include <cstdint>
#include <string>

enum class DisplayMode : int {
    Borderless = 0,
    Windowed = 1,
    Fullscreen = 2,
};

struct ModConfig {
    int autoDetect = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t desktopWidth = 0;
    uint32_t desktopHeight = 0;
    uint32_t windowWidth = 0;
    uint32_t windowHeight = 0;
    DisplayMode displayMode = DisplayMode::Borderless;
    int fixShellResolution = 1;
    int fixWidescreenUI = 1;
    int upscaleFonts = 1;
    uint32_t uiHeight = 720; // Virtual UI height (720 = 2.0x scale at 1440p; 600 = 2.4x vanilla scale; 0 = off)
    float safeZoneScale = 0.88f;
    int steamOnly = 1;
};

const char* DisplayModeToString(DisplayMode mode);
DisplayMode ParseDisplayMode(const char* str);
void ComputeWindowedSize(const ModConfig& cfg, uint32_t& outW, uint32_t& outH);

std::string GetGameDir();
void LogInit(const std::string& logPath);
void LogMsg(const char* fmt, ...);

void DetectDesktopResolution(uint32_t& outW, uint32_t& outH);
ModConfig LoadOrCreateConfig();
void SaveConfig(const ModConfig& cfg);
