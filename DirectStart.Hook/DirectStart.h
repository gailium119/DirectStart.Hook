/*
 * DirectStart - shared runtime for the standalone Windows 8.1 start-menu DLL.
 *
 * This replaces the previous Windhawk compatibility layer. It uses libvalinet
 * for PDB download/parsing (valinet/pdb/pdb.h) and INI access
 * (valinet/ini/ini.h); MinHook is only used for the inline hooks themselves.
 */

#pragma once

#include <windows.h>

#include <cstddef>

// One entry of a symbol table. The symbol strings keep the full Windhawk-style
// signatures used by the original mod; DirectStart normalises them to the
// public PDB name (for example "CPearl::BuildStartMenu") before resolving.
struct DS_SYMBOL_HOOK
{
    const wchar_t* symbols[4];
    void* pFunction;
    void* hookFunction;
    bool optional;
};

// ---------------------------------------------------------------------------
// Logging: %LOCALAPPDATA%\DirectStart\StartMenu.log
// ---------------------------------------------------------------------------
void DS_InitLog();
BOOL DS_SetLogEnabled(BOOL enabled);
BOOL DS_IsLogEnabled();
void DS_Log(const wchar_t* format, ...);

// ---------------------------------------------------------------------------
// Registry settings:
// HKCU\Software\DirectStart\StartMenu (HKLM\SOFTWARE\DirectStart\StartMenu)
//
//   enabled        REG_DWORD  default 1
//   log            REG_DWORD  default 0
//   theme          REG_SZ
//   muipath        REG_SZ
//   twinui_muipath REG_SZ
//   theme_debug    REG_DWORD
//   symbols        REG_SZ     PDB cache directory
//   ini            REG_SZ     DirectStart.ini path
// ---------------------------------------------------------------------------
int DS_GetIntValue(const wchar_t* name, int defaultValue);
int DS_GetIntSetting(const wchar_t* name);
PCWSTR DS_GetStringSetting(const wchar_t* name);
void DS_FreeStringSetting(PCWSTR value);
bool DS_IsEnabled();

// ---------------------------------------------------------------------------
// Hooking / symbol resolution
// ---------------------------------------------------------------------------
BOOL DS_InitHookEngine();
BOOL DS_HookFunction(void* target, void* hook, void** original);

// Resolves the table against DirectStart.ini and, for entries without an INI
// address, against the module's PDB. Addresses resolved from the PDB are
// written back to the INI. Returns FALSE if a non-optional entry failed.
BOOL DS_HookSymbols(
    HMODULE module,
    const DS_SYMBOL_HOOK* hooks,
    std::size_t count);

// Resolves LoadStringW the same way the original mod did (API set host).
void* DS_GetLoadStringWTarget();

// %LOCALAPPDATA%\DirectStart\DirectStart.ini (or the "ini" registry value).
PCWSTR DS_GetIniPath();

// %LOCALAPPDATA%\DirectStart\symbols (or the "symbols" registry value).
PCWSTR DS_GetSymbolsDir();
