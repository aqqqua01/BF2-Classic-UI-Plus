#include "lua_menu.h"
#include "patcher.h"
#include "config.h"
#include <windows.h>
#include <cstring>

namespace {

// Lua 5.0 C API RVAs verified in BattlefrontII.exe (base 0x00400000, lua_Number = float)
constexpr uintptr_t RVA_LUA_DOBUFFER     = 0x0029B700; // 55 8B EC FF 75 14 (6 bytes)
constexpr uintptr_t RVA_LUA_LOADBUFFER   = 0x0029B030;
constexpr uintptr_t RVA_LUA_PCALL        = 0x0029BEB0;
constexpr uintptr_t RVA_LUA_SETTOP       = 0x0029C400;
constexpr uintptr_t RVA_LUA_TONUMBER     = 0x0029AE10;
constexpr uintptr_t RVA_LUA_TOSTRING     = 0x0029ADC0;
constexpr uintptr_t RVA_LUA_GETTOP       = 0x0029BBA0;
constexpr uintptr_t RVA_LUA_ISNUMBER     = 0x0029BC80;
constexpr uintptr_t RVA_LUA_PUSHNUMBER   = 0x0029C060;
constexpr uintptr_t RVA_LUA_PUSHSTRING   = 0x0029C080;
constexpr uintptr_t RVA_LUA_PUSHCCLOSURE = 0x0029BF30;
constexpr uintptr_t RVA_LUA_RAWSET       = 0x0029C1E0;
constexpr int LUA_GLOBALSINDEX_V5        = -10001;

using FnLuaDoBuffer     = int(__cdecl*)(void* L, const char* buff, size_t size, const char* name);
using FnLuaLoadBuffer   = int(__cdecl*)(void* L, const char* buff, size_t size, const char* name);
using FnLuaPCall        = int(__cdecl*)(void* L, int nargs, int nresults, int errfunc);
using FnLuaSetTop       = void(__cdecl*)(void* L, int idx);
using FnLuaToNumber     = float(__cdecl*)(void* L, int idx);
using FnLuaToString     = const char*(__cdecl*)(void* L, int idx, int len);
using FnLuaGetTop       = int(__cdecl*)(void* L);
using FnLuaIsNumber     = int(__cdecl*)(void* L, int idx);
using FnLuaPushNumber   = void(__cdecl*)(void* L, float n);
using FnLuaPushString   = void(__cdecl*)(void* L, const char* s);
using FnLuaPushCClosure = void(__cdecl*)(void* L, int(__cdecl* fn)(void*), int n);
using FnLuaRawSet       = void(__cdecl*)(void* L, int idx);

FnLuaDoBuffer     g_origLuaDoBuffer   = nullptr;
FnLuaLoadBuffer   g_luaLoadBuffer     = nullptr;
FnLuaPCall        g_luaPCall          = nullptr;
FnLuaSetTop       g_luaSetTop         = nullptr;
FnLuaToNumber     g_luaToNumber       = nullptr;
FnLuaToString     g_luaToString       = nullptr;
FnLuaGetTop       g_luaGetTop         = nullptr;
FnLuaIsNumber     g_luaIsNumber       = nullptr;
FnLuaPushNumber   g_luaPushNumber     = nullptr;
FnLuaPushString   g_luaPushString     = nullptr;
FnLuaPushCClosure g_luaPushCClosure   = nullptr;
FnLuaRawSet       g_luaRawSet         = nullptr;

void* g_registeredLuaState = nullptr;

int __cdecl LuaCB_GetDisplayMode(void* L) {
    g_luaPushNumber(L, static_cast<float>(GetCurrentDisplayModeIndex()));
    return 1;
}

int __cdecl LuaCB_SetDisplayMode(void* L) {
    if (g_luaGetTop(L) >= 1 && g_luaIsNumber(L, 1)) {
        int modeIdx = static_cast<int>(g_luaToNumber(L, 1) + 0.5f);
        RequestDisplayModeFromMenu(modeIdx);
    }
    return 0;
}

int __cdecl LuaCB_Log(void* L) {
    if (g_luaGetTop(L) >= 1 && g_luaToString) {
        const char* msg = g_luaToString(L, 1, 0);
        if (msg) {
            LogMsg("[LuaMenu] %s", msg);
        }
    }
    return 0;
}

void RegisterLuaGlobalFn(void* L, const char* name, int(__cdecl* fn)(void*)) {
    g_luaPushString(L, name);
    g_luaPushCClosure(L, fn, 0);
    g_luaRawSet(L, LUA_GLOBALSINDEX_V5);
}

void EnsureLuaGlobalsRegistered(void* L) {
    if (!L || L == g_registeredLuaState) return;
    g_registeredLuaState = L;
    RegisterLuaGlobalFn(L, "BF2UIFix_GetDisplayMode", LuaCB_GetDisplayMode);
    RegisterLuaGlobalFn(L, "BF2UIFix_SetDisplayMode", LuaCB_SetDisplayMode);
    RegisterLuaGlobalFn(L, "BF2UIFix_Log", LuaCB_Log);
    LogMsg("[LuaMenu] Registered DisplayMode callbacks in Lua state 0x%p", L);
}

bool ContainsCaseInsensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nLen = std::strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        if (_strnicmp(p, needle, nLen) == 0) {
            return true;
        }
    }
    return false;
}

void RunInjectedLua(void* L, const char* code, const char* chunkName) {
    int savedTop = g_luaGetTop(L);
    int rc = g_luaLoadBuffer(L, code, std::strlen(code), chunkName);
    if (rc == 0) {
        rc = g_luaPCall(L, 0, 0, 0);
    }
    if (rc != 0) {
        const char* err = (g_luaGetTop(L) > savedTop && g_luaToString) ? g_luaToString(L, -1, 0) : nullptr;
        LogMsg("[LuaMenu] ERROR running %s (rc=%d): %s", chunkName, rc, err ? err : "unknown");
    }
    g_luaSetTop(L, savedTop);
}

const char* kPreVideoScriptLua = R"lua(
local function _bf2uifix_patch_el_table(elements)
    if type(elements) ~= "table" then return 0 end
    local count = 0
    for k, v in pairs(elements) do
        if type(v) == "table" and (k == "windowed" or v.tag == "windowed") then
            v.hidden = nil
            v.title = "Display Mode"
            v.control = "dropdown"
            v.values = { "Borderless", "Windowed", "Fullscreen" }
            v.selValue = BF2UIFix_GetDisplayMode()
            count = count + 1
        end
    end
    return count
end

local function _bf2uifix_config_windowed_el(tbl)
    if type(tbl) ~= "table" then return 0 end
    local n = 0
    if tbl.elements then n = n + _bf2uifix_patch_el_table(tbl.elements) end
    if tbl.Layout and tbl.Layout.elements then n = n + _bf2uifix_patch_el_table(tbl.Layout.elements) end
    if tbl.layout and tbl.layout.elements then n = n + _bf2uifix_patch_el_table(tbl.layout.elements) end
    return n
end
_bf2uifix_config_windowed_el_fn = _bf2uifix_config_windowed_el

if Form_CreateVertical then
    BF2UIFix_Log("pre: Form_CreateVertical exists")
    if not _bf2uifix_orig_Form_CreateVertical then
        _bf2uifix_orig_Form_CreateVertical = Form_CreateVertical
        Form_CreateVertical = function(a, b, c, d, e)
            local matched = 0
            if ifs_opt_pcvideo_general_options_layout then
                matched = matched + _bf2uifix_config_windowed_el(ifs_opt_pcvideo_general_options_layout)
                matched = matched + _bf2uifix_config_windowed_el(a)
                matched = matched + _bf2uifix_config_windowed_el(b)
                matched = matched + _bf2uifix_config_windowed_el(c)
                if matched > 0 then
                    BF2UIFix_Log("Form_CreateVertical patched windowed -> dropdown")
                end
            end
            local res = _bf2uifix_orig_Form_CreateVertical(a, b, c, d, e)
            local f = res
            if type(f) ~= "table" or not f.elements then
                if type(a) == "table" and (a.dropdowns or a.radiobuttons) then f = a
                elseif type(a) == "table" and type(a.form) == "table" then f = a.form end
            end
            if type(f) == "table" and ifs_opt_pcvideo_general_options_layout then
                if f.dropdowns and f.dropdowns.windowed then
                    BF2UIFix_Log("Form_CreateVertical created dropdowns.windowed!")
                elseif f.radiobuttons and f.radiobuttons.windowed then
                    BF2UIFix_Log("Form_CreateVertical STILL created radiobuttons.windowed!")
                end
            end
            return res
        end
    end
else
    BF2UIFix_Log("pre: Form_CreateVertical NIL")
end
)lua";

const char* kPostVideoScriptLua = R"lua(
if ifs_opt_pcvideo_fnBuildScreen and not _bf2uifix_orig_fnBuildScreen then
    _bf2uifix_orig_fnBuildScreen = ifs_opt_pcvideo_fnBuildScreen
    ifs_opt_pcvideo_fnBuildScreen = function(a, b, c)
        if _bf2uifix_config_windowed_el_fn and ifs_opt_pcvideo_general_options_layout then
            _bf2uifix_config_windowed_el_fn(ifs_opt_pcvideo_general_options_layout)
            BF2UIFix_Log("ifs_opt_pcvideo_fnBuildScreen configured Display Mode dropdown")
        end
        return _bf2uifix_orig_fnBuildScreen(a, b, c)
    end
end

if ifs_opt_pcvideo then
    if ifs_opt_pcvideo.formcontainergeneral then
        local f = ifs_opt_pcvideo.formcontainergeneral.form
        if f then
            if f.dropdowns and f.dropdowns.windowed then
                BF2UIFix_Log("post: form.dropdowns.windowed ALREADY OK")
            elseif f.radiobuttons and f.radiobuttons.windowed then
                BF2UIFix_Log("post: form.radiobuttons.windowed ALREADY BUILT")
            else
                BF2UIFix_Log("post: form exists without windowed")
            end
        else
            BF2UIFix_Log("post: formcontainergeneral.form NIL")
        end
    else
        BF2UIFix_Log("post: formcontainergeneral NIL (not built yet)")
    end
end

local function _bf2uifix_sync_el(screen, forceRedraw)
    local cur = BF2UIFix_GetDisplayMode()
    local form = screen and screen.formcontainergeneral and screen.formcontainergeneral.form
    local changed = nil
    local function syncOne(elements)
        if type(elements) ~= "table" then return end
        for k, el in pairs(elements) do
            if type(el) == "table" and (k == "windowed" or el.tag == "windowed") then
                el.hidden = nil
                el.title = "Display Mode"
                el.control = "dropdown"
                if not el.values or table.getn(el.values) ~= 3 then
                    el.values = { "Borderless", "Windowed", "Fullscreen" }
                end
                if el.selValue ~= cur then
                    el.selValue = cur
                    changed = 1
                end
            end
        end
    end
    if ifs_opt_pcvideo_general_options_layout then
        syncOne(ifs_opt_pcvideo_general_options_layout.elements)
    end
    if form and form.elements then
        syncOne(form.elements)
    end
    if form and (changed or forceRedraw) then
        if Form_SetValues then
            Form_SetValues(form)
        elseif Form_UpdateElement then
            Form_UpdateElement(form, "windowed")
        end
    end
end

if ifs_opt_pcvideo and not ifs_opt_pcvideo._bf2uifix_hooked then
    ifs_opt_pcvideo._bf2uifix_hooked = 1

    local origEnter = ifs_opt_pcvideo.Enter
    if origEnter then
        ifs_opt_pcvideo.Enter = function(this, bFwd, a)
            this._bf2uifix_orig_mode = BF2UIFix_GetDisplayMode()
            origEnter(this, bFwd, a)
            _bf2uifix_sync_el(this, 1)
            local form = this.formcontainergeneral and this.formcontainergeneral.form
            if form then
                if form.dropdowns and form.dropdowns.windowed then
                    BF2UIFix_Log("Enter: form.dropdowns.windowed OK")
                else
                    BF2UIFix_Log("Enter: form.dropdowns.windowed MISSING")
                end
                if Form_ShowHideElements then
                    Form_ShowHideElements(form)
                end
                _bf2uifix_sync_el(this, 1)
            end
        end
    end

    local function _bf2uifix_find_el(screen)
        local form = screen and screen.formcontainergeneral and screen.formcontainergeneral.form
        if form and type(form.elements) == "table" then
            for k, el in pairs(form.elements) do
                if type(el) == "table" and (k == "windowed" or el.tag == "windowed") then
                    return el
                end
            end
        end
        local layout = ifs_opt_pcvideo_general_options_layout
        if layout and type(layout.elements) == "table" then
            for k, el in pairs(layout.elements) do
                if type(el) == "table" and (k == "windowed" or el.tag == "windowed") then
                    return el
                end
            end
        end
        return nil
    end

    local origApply = ifs_opt_pcvideo.ApplySettings
    if origApply then
        ifs_opt_pcvideo.ApplySettings = function(this, a, b, c)
            local el = _bf2uifix_find_el(this)
            local chosen = el and el.selValue
            if el then
                el.selValue = 1
            end
            local ret = origApply(this, a, b, c)
            if el and chosen then
                el.selValue = chosen
                BF2UIFix_SetDisplayMode(chosen)
            end
            return ret
        end
    end

    local origAccept = ifs_opt_pcvideo.Input_Accept
    if origAccept then
        ifs_opt_pcvideo.Input_Accept = function(this, a, b)
            local btn = this.CurButton
            if btn == "cancel" and this._bf2uifix_orig_mode then
                BF2UIFix_SetDisplayMode(this._bf2uifix_orig_mode)
            end
            local ret = origAccept(this, a, b)
            if btn == "autodetect" then
                BF2UIFix_SetDisplayMode(1)
                _bf2uifix_sync_el(this, 1)
            end
            return ret
        end
    end

    local origBack = ifs_opt_pcvideo.Input_Back
    if origBack then
        ifs_opt_pcvideo.Input_Back = function(this, a, b)
            if this.bResetProfile and this._bf2uifix_orig_mode then
                BF2UIFix_SetDisplayMode(this._bf2uifix_orig_mode)
            end
            return origBack(this, a, b)
        end
    end

    local origUpdate = ifs_opt_pcvideo.Update
    if origUpdate then
        ifs_opt_pcvideo.Update = function(this, fDt, a)
            origUpdate(this, fDt, a)
            local form = this.formcontainergeneral and this.formcontainergeneral.form
            local dd = form and form.dropdowns and form.dropdowns.windowed
            if not (dd and dd.expanded) then
                _bf2uifix_sync_el(this, nil)
            end
        end
    end

    BF2UIFix_Log("Hooked ifs_opt_pcvideo Display Mode dropdown successfully")
end
)lua";

int __cdecl Hooked_LuaDoBuffer(void* L, const char* buff, size_t size, const char* name) {
    bool isVideoOpt = ContainsCaseInsensitive(name, "ifs_opt_pcvideo");
    if (isVideoOpt) {
        EnsureLuaGlobalsRegistered(L);
        RunInjectedLua(L, kPreVideoScriptLua, "@bf2_uifix_pre");
    }

    int rc = g_origLuaDoBuffer(L, buff, size, name);

    if (isVideoOpt && rc == 0) {
        EnsureLuaGlobalsRegistered(L);
        RunInjectedLua(L, kPostVideoScriptLua, "@bf2_uifix_post");
    }
    return rc;
}

} // namespace

void InstallLuaMenuHook(uintptr_t exeBase) {
    g_luaLoadBuffer   = reinterpret_cast<FnLuaLoadBuffer>(exeBase + RVA_LUA_LOADBUFFER);
    g_luaPCall        = reinterpret_cast<FnLuaPCall>(exeBase + RVA_LUA_PCALL);
    g_luaSetTop       = reinterpret_cast<FnLuaSetTop>(exeBase + RVA_LUA_SETTOP);
    g_luaToNumber     = reinterpret_cast<FnLuaToNumber>(exeBase + RVA_LUA_TONUMBER);
    g_luaToString     = reinterpret_cast<FnLuaToString>(exeBase + RVA_LUA_TOSTRING);
    g_luaGetTop       = reinterpret_cast<FnLuaGetTop>(exeBase + RVA_LUA_GETTOP);
    g_luaIsNumber     = reinterpret_cast<FnLuaIsNumber>(exeBase + RVA_LUA_ISNUMBER);
    g_luaPushNumber   = reinterpret_cast<FnLuaPushNumber>(exeBase + RVA_LUA_PUSHNUMBER);
    g_luaPushString   = reinterpret_cast<FnLuaPushString>(exeBase + RVA_LUA_PUSHSTRING);
    g_luaPushCClosure = reinterpret_cast<FnLuaPushCClosure>(exeBase + RVA_LUA_PUSHCCLOSURE);
    g_luaRawSet       = reinterpret_cast<FnLuaRawSet>(exeBase + RVA_LUA_RAWSET);

    const uint8_t sig[6] = { 0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x14 };
    uintptr_t target = exeBase + RVA_LUA_DOBUFFER;
    if (CheckBytes(target, sig, sizeof(sig))) {
        if (InstallTrampolineN(target, 6,
                               reinterpret_cast<void*>(Hooked_LuaDoBuffer),
                               reinterpret_cast<void**>(&g_origLuaDoBuffer))) {
            LogMsg("[Hook] Hooked lua_dobuffer for Video Options Display Mode UI");
        } else {
            LogMsg("[Hook] WARN: Failed to trampoline lua_dobuffer");
        }
    } else {
        LogMsg("[Hook] WARN: lua_dobuffer signature mismatch");
    }
}
