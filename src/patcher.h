#pragma once
#include "config.h"
#include <cstddef>
#include <cstdint>

void InstallEarlyHooksAndPatches(ModConfig& cfg);

bool CheckBytes(uintptr_t addr, const uint8_t* expected, size_t len);
bool InstallTrampolineN(uintptr_t targetAddr, size_t copyBytes, void* hookFn, void** outTrampoline);

int GetCurrentDisplayModeIndex();
void RequestDisplayModeFromMenu(int modeIndex);
