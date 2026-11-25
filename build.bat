@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_PATH=%%i
if "%VS_PATH%"=="" (
    echo Visual Studio C++ tools not found!
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1

if not exist build mkdir build
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD ^
    src\dllmain.cpp src\config.cpp src\steam_guard.cpp src\font_upscale.cpp src\patcher.cpp src\lua_menu.cpp ^
    /Fe:build\dinput8.dll /Fo:build\ ^
    /link /DEF:dinput8.def user32.lib gdi32.lib

if errorlevel 1 exit /b 1
echo Built build\dinput8.dll successfully!
