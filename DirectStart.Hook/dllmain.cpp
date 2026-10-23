/*
 * DirectStart.Hook - Windows 8.1 start-menu restoration via d3d11.dll proxy.
 *
 * The proxy/injection model (a name-forwarding d3d11.dll placed in C:\Windows,
 * loaded by explorer.exe) is adapted from early ExplorerPatcher source by
 * valinet:
 * https://github.com/valinet/ExplorerPatcher
 *
 * The actual start-menu hook logic lives in StartMenuHooks.cpp and is the
 * Windows 8.1 Start Menu Revival Windhawk mod (Allison/alliethefox, Reprety,
 * Lixkote), with the Windhawk runtime replaced by DirectStart (libvalinet PDB
 * and INI handling, MinHook for inline hooks).
 *
 * This file is part of DirectStart and is licensed under GPL-2.0.
 */

#if !defined(_M_X64)
#error DirectStart Hook only supports 64-bit Windows 8.1.
#endif

#include <windows.h>

#include <cwchar>
#include <string>

#include "DirectStart.h"
#include "D3D11Proxy.h"

// Provided by StartMenuHooks.cpp.
extern "C" BOOL DirectStartStartMenuInit();

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "DbgHelp.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Winhttp.lib")

extern "C" void* g_d3d11_procs[kD3D11ExportCount] = {};

namespace
{
    HMODULE g_module = nullptr;
    volatile LONG g_workerStarted = 0;
    volatile LONG g_proxyResolved = 0;
    INIT_ONCE g_d3d11_once = INIT_ONCE_STATIC_INIT;

    bool EqualsIgnoreCase(const wchar_t* left, const wchar_t* right)
    {
        return _wcsicmp(left, right) == 0;
    }

    std::wstring GetModulePath(HMODULE module)
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                module, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < path.size() - 1)
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    bool IsExplorerProcess()
    {
        const std::wstring imagePath = GetModulePath(nullptr);
        const wchar_t* name = wcsrchr(imagePath.c_str(), L'\\');
        name = name ? name + 1 : imagePath.c_str();
        return EqualsIgnoreCase(name, L"explorer.exe");
    }

    bool IsWindows81()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
        {
            return false;
        }

        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion)
        {
            return false;
        }

        RTL_OSVERSIONINFOW version = {};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion(&version) != 0)
        {
            return false;
        }

        return version.dwMajorVersion == 6 && version.dwMinorVersion == 3;
    }

    BOOL CALLBACK InitializeSystemD3D11(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t systemDirectory[MAX_PATH] = {};
        if (!GetSystemDirectoryW(
                systemDirectory, static_cast<UINT>(_countof(systemDirectory))))
        {
            return FALSE;
        }

        const std::wstring systemD3D11 =
            std::wstring(systemDirectory) + L"\\d3d11.dll";

        // The loader keys modules by full path, so this returns the real
        // System32 d3d11 even though this module is also named d3d11.dll.
        HMODULE module = LoadLibraryExW(
            systemD3D11.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module || module == g_module)
        {
            return FALSE;
        }

        LONG resolved = 0;
        for (size_t index = 0; index < kD3D11ExportCount; ++index)
        {
            g_d3d11_procs[index] = reinterpret_cast<void*>(
                GetProcAddress(module, kD3D11ExportNames[index]));
            if (g_d3d11_procs[index])
            {
                ++resolved;
            }
        }
        g_proxyResolved = resolved;

        return resolved == static_cast<LONG>(kD3D11ExportCount);
    }

    DWORD WINAPI HookWorker(LPVOID)
    {
        DS_InitLog();
        DS_SetLogEnabled(TRUE);

        DS_Log(
            L"DirectStart d3d11 proxy starting: module=%p proxy=lazy (first exported call loads System32 d3d11)",
            g_module);

        if (!IsExplorerProcess())
        {
            DS_SetLogEnabled(FALSE);
            return 0;
        }
        if (!IsWindows81())
        {
            DS_Log(
                L"Unsupported Windows version; start-menu hooks are not installed.");
            DS_SetLogEnabled(FALSE);
            return 0;
        }

        const BOOL ok = DirectStartStartMenuInit();
        DS_Log(
            L"DirectStart start-menu init returned %d",
            static_cast<int>(ok));
        return 0;
    }
}

// Called from D3D11Proxy_Dispatch (D3D11ProxyDispatch.asm) on the first call
// to any exported function. RCX contains the export index.
extern "C" void D3D11Proxy_Resolve(UINT_PTR)
{
    InitOnceExecuteOnce(
        &g_d3d11_once, InitializeSystemD3D11, nullptr, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);

        // The real d3d11 is loaded lazily from D3D11Proxy_Dispatch on the first
        // exported call, not here: d3d11's own DllMain must not run under our
        // loader lock.

        if (!IsExplorerProcess())
        {
            return TRUE;
        }

        if (InterlockedCompareExchange(&g_workerStarted, 1, 0) == 0)
        {
            HANDLE thread = CreateThread(
                nullptr, 0, HookWorker, nullptr, 0, nullptr);
            if (thread)
            {
                CloseHandle(thread);
            }
            else
            {
                InterlockedExchange(&g_workerStarted, 0);
            }
        }
    }
    return TRUE;
}
