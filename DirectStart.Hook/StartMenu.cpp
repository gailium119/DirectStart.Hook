#include "StartMenuHooks.h"

/*
 * Windows 8.1 Start Menu Revival 0.1.8 - standalone d3d11.dll build.
 *
 * Original Windhawk mod by Allison (alliethefox), Reprety and Lixkote.
 * The Windhawk runtime is replaced by the DirectStart runtime:
 *   - DS_Log                    -> %LOCALAPPDATA%\DirectStart\StartMenu.log
 *   - DS_GetString/IntSetting   -> HKCU\Software\DirectStart\StartMenu
 *   - DS_HookFunction           -> MinHook
 *   - DS_HookSymbols            -> DirectStart.ini + libvalinet PDB lookup
 *
 * Settings (all under HKCU\Software\DirectStart\StartMenu, HKLM fallback):
 *   theme          REG_SZ    optional msstyles file for the start menu
 *   muipath        REG_SZ    optional explorer.exe.mui path
 *   twinui_muipath REG_SZ    optional twinui.dll.mui path
 *   theme_debug    REG_DWORD log intercepted theme classes
 *   log            REG_DWORD write hook/diagnostic messages to the log
 *   enabled        REG_DWORD master switch (default 1)
 *   symbols        REG_SZ    PDB cache directory
 *   ini            REG_SZ    DirectStart.ini path
 */

#include "DirectStart.h"
#include "PatternScan.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef _WIN64
#define STDCALL  __cdecl
#define SSTDCALL L"__cdecl"
#else
#define STDCALL  __thiscall
#define SSTDCALL L"__thiscall"
#endif

EXTERN_C const IID IID_IStartMenuPopup = { 0x3e558c9e, 0x4da7, 0x4129, { 0x8c, 0x7b, 0xf6, 0xb3, 0x36, 0x87, 0x9f, 0x2d } };

typedef int (WINAPI *LoadStringW_t)(HINSTANCE, UINT, LPWSTR, int);
LoadStringW_t pOriginalLoadStringW = nullptr;

typedef HRESULT (WINAPI *GetThemeFont_t)(
    HTHEME, HDC, int, int, int, LOGFONTW*);
GetThemeFont_t pOriginalGetThemeFont = nullptr;

typedef HRESULT (WINAPI *GetThemeDefaults_t)(
    LPCWSTR, LPWSTR, DWORD, LPWSTR, DWORD);
typedef HRESULT (WINAPI *LoaderLoadTheme_t)(
    HANDLE, HINSTANCE, LPCWSTR, LPCWSTR, LPCWSTR,
    HANDLE*, LPWSTR, int, HANDLE*, LPWSTR, int,
    PVOID, HANDLE*, int, int, BOOL);
typedef HTHEME (WINAPI *OpenThemeDataFromFile_t)(
    HANDLE, HWND, LPCWSTR, DWORD);

struct LocalThemeFile
{
    char header[7];
    void* sharableSectionView;
    HANDLE sharableSection;
    void* nonsharableSectionView;
    HANDLE nonsharableSection;
    char end[3];
};

LocalThemeFile* g_localTheme = nullptr;
GetThemeDefaults_t g_getThemeDefaults = nullptr;
LoaderLoadTheme_t g_loaderLoadTheme = nullptr;
OpenThemeDataFromFile_t g_openThemeDataFromFile = nullptr;

typedef HTHEME (WINAPI *OpenThemeData_t)(HWND, LPCWSTR);
OpenThemeData_t pOriginalOpenThemeData = nullptr;
typedef HTHEME (WINAPI *OpenThemeDataEx_t)(HWND, LPCWSTR, DWORD);
OpenThemeDataEx_t pOriginalOpenThemeDataEx = nullptr;

void ReleaseInactiveTheme()
{
    if (!g_localTheme)
    {
        return;
    }
    if (g_localTheme->sharableSectionView)
        UnmapViewOfFile(g_localTheme->sharableSectionView);
    if (g_localTheme->nonsharableSectionView)
        UnmapViewOfFile(g_localTheme->nonsharableSectionView);
    if (g_localTheme->sharableSection)
        CloseHandle(g_localTheme->sharableSection);
    if (g_localTheme->nonsharableSection)
        CloseHandle(g_localTheme->nonsharableSection);
    free(g_localTheme);
    g_localTheme = nullptr;
}

bool IsStartMenuThemeClass(LPCWSTR classList)
{
    if (!classList)
        return false;

    static const WCHAR* s_rgpszClasses[] =
    {
        L"StartMenu",
        L"StartMenuPlaceListComposited",
        L"StartPanelCompositedBottom::StartPanelPriv",
        L"StartPanelCompositedBottom",
        L"StartPanelBottom::StartPanelPriv",
        L"StartPanelComposited::StartPanelPriv",
        L"StartPanel",
        L"StartPanelPriv",
        L"MorePrograms",
        L"OpenBox",
        L"LogoffButtons",
        L"SoftwareExplorer",
        L"RTStartMenuPlaceListComposited"
    };
    for (const WCHAR* pszClass : s_rgpszClasses)
    {
        if (StrStrIW(classList, pszClass))
            return true;
    }
    return false;
}

bool IsStartMenuNscTree(HWND window)
{
    WCHAR className[64];
    if (!window || !GetClassNameW(window, className, _countof(className)) ||
        lstrcmpiW(className, L"SysTreeView32") != 0)
    {
        return false;
    }
    for (HWND parent = GetParent(window); parent; parent = GetParent(parent))
    {
        if (GetClassNameW(parent, className, _countof(className)) &&
            lstrcmpiW(className, L"Desktop NSCHost") == 0)
        {
            return true;
        }
    }
    return false;
}

HTHEME OpenInactiveTheme(HWND window, LPCWSTR classList, DWORD flags)
{
    if (!g_localTheme || !g_openThemeDataFromFile || !classList)
        return nullptr;

    // The All Apps tree gets its look from INameSpaceTreeControl::SetTheme
    // ("StartMenuHover" / "StartMenuHoverComposited"), which only sets a
    // per-window sub-app name. OpenThemeData therefore sees a bare "TreeView"
    // and the RT style would never be consulted.
    LPCWSTR resolvedClassList = classList;
    if (StrStrIW(classList, L"TreeView") && IsStartMenuNscTree(window))
    {
        BOOL composited = FALSE;
        DwmIsCompositionEnabled(&composited);
        resolvedClassList = composited
            ? L"StartMenuHoverComposited::TreeView;TreeView"
            : L"StartMenuHover::TreeView;TreeView";
    }
    else if (!IsStartMenuThemeClass(classList))
    {
        return nullptr;
    }

    HTHEME theme = g_openThemeDataFromFile(
        reinterpret_cast<HANDLE>(g_localTheme), window, resolvedClassList, 0);
    if (DS_GetIntSetting(L"theme_debug"))
    {
        DS_Log(L"Inactive theme class=%s resolved=%s hwnd=%p result=%p",
               classList, resolvedClassList, window, theme);
    }
    return theme;
}

HTHEME WINAPI Hook_OpenThemeData(HWND window, LPCWSTR classList)
{
    HTHEME theme = OpenInactiveTheme(window, classList, 0);
    return theme ? theme : pOriginalOpenThemeData(window, classList);
}

HTHEME WINAPI Hook_OpenThemeDataEx(
    HWND window, LPCWSTR classList, DWORD flags)
{
    HTHEME theme = OpenInactiveTheme(window, classList, flags);
    return theme ? theme : pOriginalOpenThemeDataEx(window, classList, flags);
}

HRESULT LoadInactiveTheme(LPCWSTR path)
{
    ReleaseInactiveTheme();
    if (!path || !*path)
        return S_FALSE;

    HMODULE uxTheme = LoadLibraryExW(
        L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxTheme)
        return HRESULT_FROM_WIN32(GetLastError());

    g_getThemeDefaults = reinterpret_cast<GetThemeDefaults_t>(
        GetProcAddress(uxTheme, reinterpret_cast<LPCSTR>(7)));
    g_loaderLoadTheme = reinterpret_cast<LoaderLoadTheme_t>(
        GetProcAddress(uxTheme, reinterpret_cast<LPCSTR>(92)));
    g_openThemeDataFromFile = reinterpret_cast<OpenThemeDataFromFile_t>(
        GetProcAddress(uxTheme, reinterpret_cast<LPCSTR>(16)));
    if (!g_getThemeDefaults || !g_loaderLoadTheme ||
        !g_openThemeDataFromFile)
    {
        return E_NOINTERFACE;
    }

    wchar_t color[MAX_PATH] = {};
    wchar_t size[MAX_PATH] = {};
    HRESULT hr = g_getThemeDefaults(
        path, color, _countof(color), size, _countof(size));
    if (FAILED(hr))
        return hr;

    HANDLE sharable = nullptr;
    HANDLE nonsharable = nullptr;
    hr = g_loaderLoadTheme(
        nullptr, nullptr, path, color, size,
        &sharable, nullptr, 0,
        &nonsharable, nullptr, 0,
        nullptr, nullptr, 0, 0, FALSE);
    if (FAILED(hr))
        return hr;

    auto local = static_cast<LocalThemeFile*>(calloc(1, sizeof(LocalThemeFile)));
    if (!local)
    {
        CloseHandle(sharable);
        CloseHandle(nonsharable);
        return E_OUTOFMEMORY;
    }

    CopyMemory(local->header, "thmfile", 7);
    CopyMemory(local->end, "end", 3);
    local->sharableSection = sharable;
    local->nonsharableSection = nonsharable;
    local->sharableSectionView = MapViewOfFile(
        sharable, FILE_MAP_READ, 0, 0, 0);
    local->nonsharableSectionView = MapViewOfFile(
        nonsharable, FILE_MAP_READ, 0, 0, 0);
    if (!local->sharableSectionView || !local->nonsharableSectionView)
    {
        g_localTheme = local;
        ReleaseInactiveTheme();
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_localTheme = local;
    DS_Log(L"Loaded inactive Start-menu theme: %s color=%s size=%s",
           path, color, size);
    return S_OK;
}

HRESULT WINAPI Hook_GetThemeFont(
    HTHEME theme,
    HDC dc,
    int partId,
    int stateId,
    int propertyId,
    LOGFONTW* font)
{
    HRESULT hr = pOriginalGetThemeFont(
        theme, dc, partId, stateId, propertyId, font);
    if (SUCCEEDED(hr) || !font || partId != 5 || stateId != 1 ||
        propertyId != 210)
    {
        return hr;
    }

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        *font = metrics.lfMenuFont;
        DS_Log(
            L"GetThemeFont fallback: originalHr=0x%08X face=%s height=%d",
            static_cast<unsigned int>(hr), font->lfFaceName, font->lfHeight
        );
        return S_OK;
    }

    DS_Log(
        L"GetThemeFont fallback failed: originalHr=0x%08X spiError=%lu",
        static_cast<unsigned int>(hr), GetLastError()
    );
    return hr;
}

HINSTANCE g_hExplorerMui = nullptr;
HINSTANCE g_hTwinUiMui = nullptr;

int WINAPI Hook_LoadStringW(HINSTANCE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax)
{
    HMODULE hExplorer = GetModuleHandleW(nullptr);
    HMODULE hTwinUI = GetModuleHandleW(L"twinui.dll");

    HINSTANCE alternate = nullptr;
    if (hInstance == hExplorer)
        alternate = g_hExplorerMui;
    else if (hInstance == hTwinUI)
        alternate = g_hTwinUiMui;
    if (alternate)
    {
        return pOriginalLoadStringW(alternate, uID, lpBuffer, cchBufferMax);
    }
    return pOriginalLoadStringW(hInstance, uID, lpBuffer, cchBufferMax);
}

PVOID g_vehHandle = nullptr;

typedef HRESULT (WINAPI *CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstance_t pOriginalCoCreateInstance = nullptr;

static const CLSID kClsidGridControl =
    { 0x71c99ab9, 0x508a, 0x4671, { 0x8b, 0x67, 0x30, 0x2e, 0x09, 0x8f, 0xbf, 0xa8 } };
static const IID kIidClassFactory =
    { 0x00000001, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const CLSID kClsidExplorerBrowser =
    { 0x71f96385, 0xddd6, 0x48d3, { 0xa0, 0xc1, 0xae, 0x06, 0xe8, 0xb0, 0x55, 0xfb } };

static HRESULT CreateFromTwinUI(REFCLSID rclsid, LPUNKNOWN pOuter, REFIID riid, LPVOID* ppv)
{
    HMODULE hTwinUI = LoadLibraryW(L"twinui.dll");
    if (!hTwinUI)
        return HRESULT_FROM_WIN32(GetLastError());

    typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);
    DllGetClassObject_t pfnGetClassObject =
        reinterpret_cast<DllGetClassObject_t>(GetProcAddress(hTwinUI, "DllGetClassObject"));
    if (!pfnGetClassObject)
        return REGDB_E_CLASSNOTREG;

    IClassFactory* pFactory = nullptr;
    HRESULT hr = pfnGetClassObject(rclsid, kIidClassFactory, reinterpret_cast<LPVOID*>(&pFactory));
    DS_Log(L"CreateFromTwinUI: DllGetClassObject hr=0x%08X factory=%p",
           static_cast<unsigned int>(hr), pFactory);
    if (SUCCEEDED(hr) && pFactory)
    {
        hr = pFactory->CreateInstance(pOuter, riid, ppv);
        pFactory->Release();
    }
    return hr;
}

HRESULT WINAPI Hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                     REFIID riid, LPVOID* ppv)
{
    HRESULT hr = pOriginalCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);

    if (IsEqualCLSID(rclsid, kClsidGridControl))
    {
        DS_Log(L"CoCreateInstance(CLSID_GridControl): original hr=0x%08X",
               static_cast<unsigned int>(hr));
        if (FAILED(hr))
        {
            hr = CreateFromTwinUI(rclsid, pUnkOuter, riid, ppv);
            DS_Log(L"CoCreateInstance(CLSID_GridControl): twinui fallback hr=0x%08X out=%p",
                   static_cast<unsigned int>(hr), ppv ? *ppv : nullptr);
        }
    }
    else if (IsEqualCLSID(rclsid, kClsidExplorerBrowser))
    {
        DS_Log(L"CoCreateInstance(CLSID_ExplorerBrowser): hr=0x%08X",
               static_cast<unsigned int>(hr));
        if (g_inGridPaneCreate && SUCCEEDED(hr) && ppv && *ppv)
        {
            IUnknown* pNew = static_cast<IUnknown*>(*ppv);
            pNew->AddRef();
            if (g_gridBrowserUnk)
                g_gridBrowserUnk->Release();
            g_gridBrowserUnk = pNew;
        }
    }
    return hr;
}

BOOL InstallCoCreateInstanceHook()
{
    HMODULE hOle32 = LoadLibraryW(L"ole32.dll");
    void* pTarget = hOle32 ? reinterpret_cast<void*>(GetProcAddress(hOle32, "CoCreateInstance")) : nullptr;
    if (!pTarget)
    {
        DS_Log(L"InstallCoCreateInstanceHook: CoCreateInstance not found.");
        return FALSE;
    }
    BOOL ok = DS_HookFunction(pTarget, reinterpret_cast<void*>(Hook_CoCreateInstance),
                                 reinterpret_cast<void**>(&pOriginalCoCreateInstance));
    DS_Log(L"InstallCoCreateInstanceHook: %s", ok ? L"ok" : L"failed");
    return ok;
}

extern "C" BOOL DirectStartStartMenuInit()
{
    DS_SetLogEnabled(TRUE);
    DS_Log(L"DirectStart start-menu hooks: init (d3d11 build)");

    if (!DS_IsEnabled())
    {
        DS_Log(L"Start menu support disabled by registry (enabled=0).");
        DS_SetLogEnabled(DS_GetIntSetting(L"log") != 0);
        return TRUE;
    }

    if (!DS_InitHookEngine())
    {
        DS_Log(L"MinHook initialization failed; no hooks can be installed.");
        DS_SetLogEnabled(DS_GetIntSetting(L"log") != 0);
        return FALSE;
    }

    g_pearlGateTls = TlsAlloc();
    if (g_pearlGateTls == TLS_OUT_OF_INDEXES)
        DS_Log(L"TlsAlloc failed - pearl RT color gate disabled");

    g_vehHandle = AddVectoredExceptionHandler(1, CrashLogVeh);
    InstallCoCreateInstanceHook();

    PCWSTR pszMuiPath = DS_GetStringSetting(L"muipath");
    g_hExplorerMui = pszMuiPath && *pszMuiPath
        ? LoadLibraryExW(
              pszMuiPath, nullptr,
              LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)
        : nullptr;
    DS_FreeStringSetting(pszMuiPath);

    pszMuiPath = DS_GetStringSetting(L"twinui_muipath");
    g_hTwinUiMui = pszMuiPath && *pszMuiPath
        ? LoadLibraryExW(
              pszMuiPath, nullptr,
              LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)
        : nullptr;
    DS_FreeStringSetting(pszMuiPath);

    // The Windhawk mod resolved LoadStringW through an API set. The API set
    // module is not guaranteed to be loaded/exporting it on every 8.1 build,
    // so DirectStart walks the API set / kernelbase / kernel32 instead.
    auto pfnLoadStringW = reinterpret_cast<decltype(&LoadStringW)>(
        DS_GetLoadStringWTarget());
    DS_Log(L"Explorer LoadStringW import target=%p", pfnLoadStringW);
    if (!pfnLoadStringW || !DS_HookFunction(
            reinterpret_cast<void*>(pfnLoadStringW),
            reinterpret_cast<void*>(Hook_LoadStringW),
            reinterpret_cast<void**>(&pOriginalLoadStringW)))
    {
        DS_Log(L"Failed to hook LoadStringW (continuing)");
    }

    HMODULE hUxTheme = GetModuleHandleW(L"UxTheme.dll");
    if (!hUxTheme)
    {
        hUxTheme = LoadLibraryExW(
            L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!hUxTheme)
    {
        DS_Log(L"Failed to acquire handle to UxTheme.dll (continuing)");
    }
    else
    {
        auto pfnGetThemeFont = reinterpret_cast<decltype(&GetThemeFont)>(
            GetProcAddress(hUxTheme, "GetThemeFont")
        );
        DS_Log(L"Explorer GetThemeFont import target=%p", pfnGetThemeFont);
        if (!pfnGetThemeFont || !DS_HookFunction(
                reinterpret_cast<void*>(pfnGetThemeFont),
                reinterpret_cast<void*>(Hook_GetThemeFont),
                reinterpret_cast<void**>(&pOriginalGetThemeFont)))
        {
            DS_Log(L"Failed to hook GetThemeFont (continuing)");
        }

        auto pfnOpenThemeData = reinterpret_cast<decltype(&OpenThemeData)>(
            GetProcAddress(hUxTheme, "OpenThemeData")
        );
        DS_Log(L"Explorer OpenThemeData import target=%p", pfnOpenThemeData);
        if (!pfnOpenThemeData || !DS_HookFunction(
                reinterpret_cast<void*>(pfnOpenThemeData),
                reinterpret_cast<void*>(Hook_OpenThemeData),
                reinterpret_cast<void**>(&pOriginalOpenThemeData)))
        {
            DS_Log(L"Failed to hook OpenThemeData (continuing)");
        }

        auto pfnOpenThemeDataEx = reinterpret_cast<decltype(&OpenThemeDataEx)>(
            GetProcAddress(hUxTheme, "OpenThemeDataEx")
        );
        if (!pfnOpenThemeDataEx || !DS_HookFunction(
                reinterpret_cast<void*>(pfnOpenThemeDataEx),
                reinterpret_cast<void*>(Hook_OpenThemeDataEx),
                reinterpret_cast<void**>(&pOriginalOpenThemeDataEx)))
        {
            DS_Log(L"Failed to hook OpenThemeDataEx (continuing)");
        }
    }

    PCWSTR pszThemePath = DS_GetStringSetting(L"theme");
    HRESULT themeHr = LoadInactiveTheme(pszThemePath);
    DS_Log(L"LoadInactiveTheme returned 0x%08X",
           static_cast<unsigned int>(themeHr));
    DS_FreeStringSetting(pszThemePath);

    BOOL ok = TRUE;
    if (!InitializeTWinUIHooks())
    {
        DS_Log(L"TWinUI hooks failed.");
        ok = FALSE;
    }
    if (!InitializeExplorerHooks())
    {
        DS_Log(L"Explorer hooks failed.");
        ok = FALSE;
    }
    if (!InitializeDUI70Hooks())
    {
        DS_Log(L"DUI70 hooks failed.");
        ok = FALSE;
    }

    DS_Log(L"DirectStart start-menu hooks: init finished, result=%d", ok);
    DS_SetLogEnabled(DS_GetIntSetting(L"log") != 0);
    return ok;
}

extern "C" void DirectStartStartMenuUninit()
{
    DS_Log(L"Uninit");

    if (g_pearlGateTls != TLS_OUT_OF_INDEXES)
    {
        TlsFree(g_pearlGateTls);
        g_pearlGateTls = TLS_OUT_OF_INDEXES;
    }


    if (g_vehHandle)
    {
        RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = nullptr;
    }
}
