# BF2 Classic UI+ (`dinput8.dll`)

A lightweight runtime enhancement mod (`dinput8.dll` proxy) for **Star Wars: Battlefront II (Classic, 2005 — Steam)** that modernizes the main menu, HUD scaling, font crispness, and display mode controls without modifying any game files on disk (`.exe` / `.lvl`).

---

## Features

- **Native Resolution Shell (Main Menu):** Removes the hardcoded `800x600` menu cap so the main menu runs at your monitor's full resolution (e.g., `2560x1440`, `3840x2160`, `1920x1080`).
- **Automatic Display Detection & Persistence:** Auto-detects your primary monitor resolution on first launch and remembers any resolution you select in **Options → Video** inside `bf2_uifix.ini`.
- **Virtual 2D UI Canvas & SafeZone Scaling:** Scales 2D UI, menus, and HUD cleanly (`UIHeight=720`, `SafeZoneScale=0.88`) while keeping 3D world rendering, `dgVoodoo`, `ReShade`, and frame-gen/upscalers at full native resolution.
- **4x Font Atlas Upscaler:** Automatically upscales low-res `128x128` / `256x256` bitmap font atlases to `512x512` / `1024x1024` in memory with anti-aliased smoothing (`UpscaleFonts=1`).
- **3 Display Modes (`Borderless`, `Windowed`, `Fullscreen`):**
  - Integrated directly into **Options → Video → Display Mode** (both in the Main Menu and in-game Pause Menu, supporting **Accept**, **Cancel** revert, and **Reset to Defaults**).
  - Live switching at any time with **`F11`** or **`Alt+Enter`** (synced with the menu and saved to `bf2_uifix.ini`).
- **Non-destructive & Steam-Verified:** Works purely in memory via `dinput8.dll`, leaves `d3d9.dll` free for ReShade / dgVoodoo, and verifies active Steam ownership (`AppID 6060`) via `steam_api.dll`.

---

## Installation

1. Download **`BF2-Classic-UI-Plus-v1.0.zip`** from Releases.
2. Extract **`dinput8.dll`** into your game's `GameData` folder (next to `BattlefrontII.exe`), e.g.:
   `...\SteamLibrary\steamapps\common\Star Wars Battlefront II Classic\GameData\`
3. Launch the game **from Steam**.
4. On first launch, `bf2_uifix.ini` is generated automatically for your monitor.

### Uninstallation
Delete `dinput8.dll`, `bf2_uifix.ini`, and `bf2_uifix.log` from `GameData`.

---

## Controls & Configuration (`bf2_uifix.ini`)

| Option | Default | Description |
|---|---|---|
| `AutoDetect` | `1` | Automatically match desktop resolution on launch (`0` to lock custom resolution) |
| `DisplayMode` | `borderless` | `borderless`, `windowed`, or `fullscreen` |
| `FixShellResolution` | `1` | Unlocks Main Menu from `800x600` |
| `FixWidescreenUI` | `1` | Enables virtual 2D UI canvas scaling |
| `UIHeight` | `720` | Virtual 2D UI height (`720` = crisp 2x at 1440p; `0` = off) |
| `SafeZoneScale` | `0.88` | Pulls UI edges slightly inward to prevent clipping (`0.70`–`1.00`) |
| `UpscaleFonts` | `1` | 4x runtime bitmap font atlas upscaler (`1` = on, `0` = off) |

**Hotkeys:** `F11` or `Alt+Enter` cycles `Borderless → Windowed → Fullscreen` live without restarting the game.

---

## Compatibility & Notes

- **Required:** **Steam version** of *Star Wars: Battlefront II (Classic, 2005)* (`AppID 6060`, updated Steam/2017 `BattlefrontII.exe` build).
- **Antivirus / SmartScreen:** Because this mod is an unsigned 32-bit DLL performing runtime memory hooks, some AV scanners may flag it as a false positive. Full C++ source code is provided in this repository so anyone can inspect or compile it locally.

## Building from Source

Requires **Visual Studio (C++ x86 Desktop tools)** with Windows SDK:

```bat
build.bat
```

Outputs `build\dinput8.dll`.

---

## Українською (Коротко)

Мод для **Star Wars Battlefront II Classic (2005, Steam)** у вигляді `dinput8.dll`, який не змінює файли гри на диску:
- Розблоковує головне меню (`Shell`) з `800x600` до рідної роздільності монітора (`2560x1440` тощо) з автодетектом та збереженням вибору.
- Адаптує 2D-інтерфейс та `SafeZone` без втрати чіткості 3D-картинки.
- Покращує шрифти в меню і бою через 4x апскейлер текстурних атласів у пам'яті.
- Додає перемикач **Display Mode** (`Borderless` / `Windowed` / `Fullscreen`) прямо в **Options → Video**, а також гарячі клавіші **`F11`** / **`Alt+Enter`**.
- Працює тільки з ліцензійною Steam-версією гри (`AppID 6060`).
