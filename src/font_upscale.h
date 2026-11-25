#pragma once
#include <d3d9.h>

void InstallFontUpscalerHooks(IDirect3DDevice9* device, bool enabled);
void OnDeviceResetForFontUpscaler();
