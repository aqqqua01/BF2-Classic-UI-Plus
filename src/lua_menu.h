#pragma once
#include <cstdint>

/** Installs Lua 5.0 runtime hooks to add the Display Mode selector to Options -> Video. */
void InstallLuaMenuHook(uintptr_t exeBase);
