/*
 * Windows 8.1 Start Menu Revival 0.1.8 - standalone winmm.dll build.
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

BOOL IsStartMenuEnabled()
{
    return DS_IsEnabled();
}

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
    if (!window || !GetClassNameW(window, className, ARRAYSIZE(className)) ||
        lstrcmpiW(className, L"SysTreeView32") != 0)
    {
        return false;
    }
    for (HWND parent = GetParent(window); parent; parent = GetParent(parent))
    {
        if (GetClassNameW(parent, className, ARRAYSIZE(className)) &&
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
        path, color, ARRAYSIZE(color), size, ARRAYSIZE(size));
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

typedef void (__fastcall *CPearl_BuildStartMenu_t)(void* pThis);
CPearl_BuildStartMenu_t pOriginalCPearl_BuildStartMenu = nullptr;

typedef LRESULT (__fastcall *CTray_v_WndProc_t)(void* pThis, HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
CTray_v_WndProc_t pOriginalCTray_v_WndProc = nullptr;

typedef void (__fastcall *CPearl_CloseStartMenu_t)(void* pThis);
CPearl_CloseStartMenu_t pOriginalCPearl_CloseStartMenu = nullptr;

typedef void (__fastcall *CPearl_DestroyStartMenu_t)(void* pThis);
CPearl_DestroyStartMenu_t pOriginalCPearl_DestroyStartMenu = nullptr;

typedef HRESULT (__fastcall *CPearl_UpdateTheme_t)(void* pPearl);
CPearl_UpdateTheme_t pOriginalCPearl_UpdateTheme = nullptr;

typedef HRESULT (__cdecl *DesktopV2_Create_t)(
    IMenuPopup** ppMenuPopup,
    IMenuBand** ppMenuBand,
    void** ppDeskHost,
    IUnknown** ppUnknown,
    HWND hWnd
);
DesktopV2_Create_t pOriginalDesktopV2_Create = nullptr;

typedef int (__fastcall *CDesktopHost__AddWin32Controls_t)(void* pThis);
CDesktopHost__AddWin32Controls_t pOriginalCDesktopHost__AddWin32Controls = nullptr;

typedef HWND (__fastcall *CDesktopHost_Create_t)(void* pThis);
CDesktopHost_Create_t pOriginalCDesktopHost_Create = nullptr;

typedef HRESULT (__fastcall *CDesktopHost_InitializeSearchPane_t)(void* pThis);
CDesktopHost_InitializeSearchPane_t pOriginalCDesktopHost_InitializeSearchPane = nullptr;

typedef HRESULT (__fastcall *CDesktopHost_AcquireSearchServices_t)(
    void* pThis,
    const GUID* serviceId,
    void** service
);
CDesktopHost_AcquireSearchServices_t pOriginalCDesktopHost_AcquireSearchServices = nullptr;

BOOL __fastcall Hook_CDesktopHost__AddWin32Controls(void* pThis)
{
    int mode = pThis ? *reinterpret_cast<int*>((char*)pThis + 0x184) : -1;
    DS_Log(L"CDesktopHost::_AddWin32Controls: begin this=%p mode=%d", pThis, mode);

    int result = pOriginalCDesktopHost__AddWin32Controls(pThis);

    DS_Log(L"CDesktopHost::_AddWin32Controls: end result=%d", result);
    return result;
}

HWND __fastcall Hook_CDesktopHost_Create(void* pThis)
{
    DS_Log(L"CDesktopHost::_Create: begin this=%p", pThis);
    HWND hwndRet = pOriginalCDesktopHost_Create(pThis);
    DS_Log(
        L"CDesktopHost::_Create: end hwnd=%p valid=%d",
        hwndRet, IsWindow(hwndRet)
    );
    return hwndRet;
}

HRESULT __fastcall Hook_CDesktopHost_InitializeSearchPane(void* pThis)
{
    // Windows 8.1 AMD64's Windows.UI.Search implementation reaches
    // CASTAThreadHost::StartThread with a null ASTA host when the retained
    // Desktop2 menu is built directly on Explorer's tray thread. Returning a
    // failure would make CDesktopHost::_Create destroy the whole menu, so
    // report success while leaving search/type-to-search uninitialized.
    DS_Log(
        L"CDesktopHost::_InitializeSearchPane: skipped Windows.UI.Search initialization for host=%p",
        pThis
    );
    return S_OK;
}

HRESULT __fastcall Hook_CDesktopHost_AcquireSearchServices(
    void* pThis,
    const GUID* serviceId,
    void** service)
{
    if (service)
    {
        *service = nullptr;
    }
    DS_Log(
        L"CDesktopHost::_AcquireSearchServices: blocked for host=%p",
        pThis
    );
    return E_NOINTERFACE;
}

typedef HRESULT (__fastcall *DesktopV2_Build_t)(void* pDeskHost, DWORD dwFlags);
DesktopV2_Build_t pOriginalDesktopV2_Build = nullptr;
HRESULT __fastcall Hook_DesktopV2_Build(void* pDeskHost, DWORD dwFlags)
{
    DS_Log(L"DesktopV2_Build: begin pDeskHost=%p dwFlags=%d", pDeskHost, dwFlags);
    HRESULT hr = pOriginalDesktopV2_Build(pDeskHost, dwFlags);
    DS_Log(
        L"DesktopV2_Build: end hr=0x%08X",
        static_cast<unsigned int>(hr)
    );
    return hr;
}

typedef HRESULT (__fastcall *CPearl_Initialize_t)(void* pThis, HWND a2, void* a3);
CPearl_Initialize_t pOriginalCPearl_Initialize = nullptr;
HRESULT __fastcall Hook_CPearl_Initialize(void* pThis, HWND a2, void* a3)
{
    return pOriginalCPearl_Initialize(pThis, a2, a3);
}

void __fastcall Hook_CPearl_BuildStartMenu(void* pThis)
{
    DS_Log(L"CPearl::BuildStartMenu: Hook Entrance. pThis = 0x%p", pThis);
    if (!IsStartMenuEnabled() || !pThis)
    {
        pOriginalCPearl_BuildStartMenu(pThis);
        return;
    }

    if (pOriginalCPearl_CloseStartMenu) pOriginalCPearl_CloseStartMenu(pThis);
    
    void** ppUnknownContainer = (void**)((char*)pThis + (23 * sizeof(void*))); 
    if (ppUnknownContainer && *ppUnknownContainer)
    {
        void* pContainer = *ppUnknownContainer;
        void** pVtbl = *(void***)pContainer;
        if (pVtbl && pVtbl[3])
        {
            void (__fastcall *pNotifyFunc)(void*) = (void (__fastcall *)(void*))pVtbl[3];
            pNotifyFunc(pContainer);
        }
    }
    
    if (pOriginalCPearl_DestroyStartMenu)
    {
        pOriginalCPearl_DestroyStartMenu(pThis);
    }
    
    HWND hWndTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hWndTray && pOriginalDesktopV2_Create)
    {
        IMenuPopup* pOutMenuPopup = nullptr;
        IMenuBand* pOutMenuBand = nullptr; 
        void* pDeskHost = nullptr;
        IUnknown* pUnknown = nullptr;
        
        HRESULT hrCreate = pOriginalDesktopV2_Create(
            &pOutMenuPopup,
            &pOutMenuBand,
            &pDeskHost,
            &pUnknown,
            hWndTray
        );
        DS_Log(
            L"CPearl::BuildStartMenu: Create HRESULT = 0x%X, Popup = 0x%p, Host = 0x%p",
            hrCreate,
            pOutMenuPopup,
            pDeskHost
        );
        
        if (SUCCEEDED(hrCreate) && pDeskHost)
        {
            if (pOutMenuPopup)
            {
                void** pVtblSurface = *(void***)pOutMenuPopup;
                if (pVtblSurface)
                {
                    typedef HRESULT (__fastcall *QueryInterface_t)(void* pThis, REFIID riid, void** ppvObj);
                    QueryInterface_t pQueryInterface = (QueryInterface_t)pVtblSurface[0];
                    void* pTargetNotificationField = (char*)pThis + 160;
                    if (pQueryInterface)
                    {
                        pQueryInterface(pOutMenuPopup, IID_IStartMenuPopup, (void**)pTargetNotificationField);
                    }
                }
            }
            
            if (pUnknown)
            {
                IUnknown_SetSite(pUnknown, reinterpret_cast<IUnknown*>((char*)pThis + 16));
            }

            if (pOriginalDesktopV2_Build)
            {
                DS_Log(L"CPearl::BuildStartMenu: Invoking DesktopV2_Build with flag 0x0");
                HRESULT hrBuild = Hook_DesktopV2_Build(pDeskHost, 0x0);
                DS_Log(L"CPearl::BuildStartMenu: DesktopV2_Build returned HRESULT = 0x%I64X", hrBuild);
            }

            if (pOutMenuPopup)
            {
                IUnknown* pUnkSurface = (IUnknown*)pOutMenuPopup;
                pUnkSurface->Release();
            }
        }
    }
}

typedef void (__fastcall *CPearl_DisplayStartMenu_t)(void* pThis);
CPearl_DisplayStartMenu_t pOriginalCPearl_DisplayStartMenu = nullptr;
static HWND g_gridPaneParentHwnd = nullptr;
static HWND g_gridHostHwnd = nullptr;
static void* g_gridHostElement = nullptr;
static void DumpDuiTree(void* pRoot);
static bool g_inGridPaneCreate = false;
static IUnknown* g_gridBrowserUnk = nullptr;
static const IID kIidExplorerBrowser =
    { 0xdfd3b6b5, 0xc10c, 0x4be9, { 0x85, 0xf6, 0xa6, 0x69, 0x69, 0xf4, 0x02, 0xf6 } };
static const IID kIidFolderView2 =
    { 0x1af3a467, 0x214f, 0x4298, { 0x90, 0x8e, 0x06, 0xb0, 0x3e, 0x0b, 0x39, 0xf9 } };

static void LogGridBrowserState()
{
    if (!g_gridBrowserUnk)
    {
        DS_Log(L"GridBrowser: no ExplorerBrowser captured during CreatePane.");
        return;
    }
    IExplorerBrowser* peb = nullptr;
    HRESULT hr = g_gridBrowserUnk->QueryInterface(kIidExplorerBrowser, reinterpret_cast<void**>(&peb));
    if (FAILED(hr) || !peb)
    {
        DS_Log(L"GridBrowser: QI(IExplorerBrowser) hr=0x%08X", static_cast<unsigned int>(hr));
        return;
    }
    IFolderView2* pfv = nullptr;
    hr = peb->GetCurrentView(kIidFolderView2, reinterpret_cast<void**>(&pfv));
    DS_Log(L"GridBrowser: GetCurrentView(IFolderView2) hr=0x%08X view=%p", static_cast<unsigned int>(hr), pfv);
    if (SUCCEEDED(hr) && pfv)
    {
        int count = -1;
        HRESULT hrCount = pfv->ItemCount(SVGIO_ALLVIEW, &count);
        DS_Log(L"GridBrowser: ItemCount(ALLVIEW) hr=0x%08X count=%d", static_cast<unsigned int>(hrCount), count);
        pfv->Release();
    }
    peb->Release();
}
static volatile LONG g_windowTreeDumpCount = 0;

static void DumpWindowTreeRec(HWND hwnd, int depth)
{
    if (depth > 8)
        return;

    wchar_t indent[40] = {};
    for (int i = 0; i < depth * 2 && i < 38; ++i)
        indent[i] = L' ';

    wchar_t cls[96] = L"?";
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    const unsigned long style = static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const unsigned long exStyle = static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

    DS_Log(L"WinTree: %s hwnd=%p class=%s visible=%d size=%ldx%ld at=(%ld,%ld) style=0x%08lX ex=0x%08lX",
           indent, hwnd, cls, IsWindowVisible(hwnd) ? 1 : 0,
           rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top, style, exStyle);

    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        DumpWindowTreeRec(child, depth + 1);
}

static bool g_appsFolderLogged = false;
static void LogAppsFolderEnumeration()
{
    if (g_appsFolderLogged)
        return;
    g_appsFolderLogged = true;

    typedef HRESULT (WINAPI *SHGetKnownFolderItem_t)(const GUID&, int, HANDLE, REFIID, void**);
    static const GUID kFolderIdAppsFolder =
        { 0x1e87508d, 0x89c2, 0x42f0, { 0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58 } };
    static const IID kIidShellItem =
        { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
    static const GUID kBhidEnumItems =
        { 0x94f60519, 0x2850, 0x4924, { 0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39 } };
    static const IID kIidEnumShellItems =
        { 0x70629033, 0xe363, 0x4a28, { 0xa5, 0x67, 0x0d, 0xb7, 0x80, 0x06, 0xe6, 0xd7 } };

    HMODULE hShell = LoadLibraryW(L"shell32.dll");
    SHGetKnownFolderItem_t pfn = hShell
        ? reinterpret_cast<SHGetKnownFolderItem_t>(GetProcAddress(hShell, "SHGetKnownFolderItem"))
        : nullptr;
    if (!pfn)
    {
        DS_Log(L"AppsFolder: SHGetKnownFolderItem not available.");
        return;
    }

    typedef void (WINAPI *CoTaskMemFree_t)(LPVOID);
    HMODULE hOle32 = LoadLibraryW(L"ole32.dll");
    CoTaskMemFree_t pfnCoTaskMemFree = hOle32
        ? reinterpret_cast<CoTaskMemFree_t>(GetProcAddress(hOle32, "CoTaskMemFree"))
        : nullptr;

    IShellItem* psi = nullptr;
    HRESULT hr = pfn(kFolderIdAppsFolder, 0, nullptr, kIidShellItem,
                     reinterpret_cast<void**>(&psi));
    DS_Log(L"AppsFolder: SHGetKnownFolderItem hr=0x%08X item=%p", static_cast<unsigned int>(hr), psi);
    if (FAILED(hr) || !psi)
        return;

    IEnumShellItems* penum = nullptr;
    hr = psi->BindToHandler(nullptr, kBhidEnumItems, kIidEnumShellItems, reinterpret_cast<void**>(&penum));
    DS_Log(L"AppsFolder: BindToHandler(EnumItems) hr=0x%08X enum=%p", static_cast<unsigned int>(hr), penum);
    if (SUCCEEDED(hr) && penum)
    {
        int count = 0;
        IShellItem* pItem = nullptr;
        ULONG fetched = 0;
        while (count < 2000 && penum->Next(1, &pItem, &fetched) == S_OK && fetched == 1)
        {
            if (count < 8)
            {
                PWSTR pszName = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_NORMALDISPLAY, &pszName)) && pszName)
                {
                    DS_Log(L"AppsFolder: item #%d = %s", count, pszName);
                    if (pfnCoTaskMemFree) pfnCoTaskMemFree(pszName);
                }
            }
            pItem->Release();
            pItem = nullptr;
            ++count;
        }
        DS_Log(L"AppsFolder: total items enumerated = %d", count);
        penum->Release();
    }
    psi->Release();
}

static void DumpStartMenuWindowTree(const wchar_t* reason)
{
    // These dumps enumerate windows, the Apps folder and the DirectUI tree.
    // Only do that when the user explicitly enabled verbose logging.
    if (!DS_IsLogEnabled())
        return;
    if (InterlockedIncrement(&g_windowTreeDumpCount) > 4)
        return;
    HWND root = g_gridPaneParentHwnd ? GetAncestor(g_gridPaneParentHwnd, GA_ROOT) : nullptr;
    if (!root)
    {
        root = FindWindowW(L"DV2ControlHost", nullptr);
    }
    DS_Log(L"WinTree: dump begin (%s) root=%p gridParent=%p gridHost=%p",
           reason, root, g_gridPaneParentHwnd, g_gridHostHwnd);
    if (root)
        DumpWindowTreeRec(root, 0);
    DS_Log(L"WinTree: dump end");
    LogGridBrowserState();
    if (reason && wcsstr(reason, L"after DisplayStartMenu"))
        LogAppsFolderEnumeration();
    if (g_gridHostElement)
        DumpDuiTree(g_gridHostElement);
    else
        DS_Log(L"DuiTree: no grid host element captured (menu built before the mod loaded?).");
}

void __fastcall Hook_CPearl_DisplayStartMenu(void* pThis)
{
    DS_Log(L"CPearl::DisplayStartMenu: this=%p popup=%p",
           pThis,
           pThis ? *reinterpret_cast<void**>((char*)pThis + 0xA0) : nullptr);
    pOriginalCPearl_DisplayStartMenu(pThis);
    DumpStartMenuWindowTree(L"after DisplayStartMenu");
}

typedef HRESULT (__fastcall *CPearl_ShowStartPage_t)(void* pThis);
CPearl_ShowStartPage_t pOriginalCPearl_ShowStartPage = nullptr;
HRESULT __fastcall Hook_CPearl_ShowStartPage(void* pThis)
{
    if (IsStartMenuEnabled() && pThis)
    {
        HWND hWndTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
        if (hWndTaskbar)
        {
            PostMessageW(hWndTaskbar, 0x568, (WPARAM)1, (LPARAM)0); 
            return 0; 
        }
    }
    return pOriginalCPearl_ShowStartPage(pThis);
}

typedef HRESULT (__thiscall *Element_SetClass_t)(void* pThis, LPCWSTR pszClass);
Element_SetClass_t pfnElement_SetClass = nullptr;

struct DuiElementArray
{
    UINT sizeAndFlags;
#ifdef _WIN64
    UINT alignmentPadding;
#endif
    void* bufferOrFirstItem;
};

typedef DuiElementArray* (__thiscall *Element_GetChildren_t)(void* pThis, void** ppValue);
Element_GetChildren_t pfnElement_GetChildren = nullptr;

typedef void (__thiscall *Value_Release_t)(void* pThis);
Value_Release_t pfnValue_Release = nullptr;

typedef HRESULT (__thiscall *CDUIResourceManager_CreateElement_t)(void* pThis, LPCWSTR pszResID, void* pParent, void* pUnknown, DWORD* pdwDeferCookie, void** ppElementOut);
CDUIResourceManager_CreateElement_t pOriginalCDUIResourceManager_CreateElement = nullptr;

typedef HRESULT (__fastcall *CImmersiveSystemTile__Initialize_t)(void* pThis);
CImmersiveSystemTile__Initialize_t pOriginalCImmersiveSystemTile__Initialize = nullptr;

thread_local bool g_inCImmersiveSystemTile_Initialize = false;
thread_local void* g_pSystemTileRootElement = nullptr;

typedef int (__thiscall *Element_GetInt_t)(void* pThis);
typedef bool (__thiscall *Element_GetBool_t)(void* pThis);
typedef const WCHAR* (__thiscall *Element_GetClass_t)(void* pThis, void** ppValue);
static Element_GetInt_t pfnElement_GetWidth = nullptr;
static Element_GetInt_t pfnElement_GetHeight = nullptr;
static Element_GetInt_t pfnElement_GetX = nullptr;
static Element_GetInt_t pfnElement_GetY = nullptr;
static Element_GetBool_t pfnElement_GetVisible = nullptr;
static Element_GetClass_t pfnElement_GetClass = nullptr;
static bool g_duiDumpResolved = false;

static void ResolveDuiDumpFunctions()
{
    if (g_duiDumpResolved)
        return;
    g_duiDumpResolved = true;
    HMODULE hDui = GetModuleHandleW(L"dui70.dll");
    if (!hDui)
        return;
    pfnElement_GetWidth = reinterpret_cast<Element_GetInt_t>(GetProcAddress(hDui, "?GetWidth@Element@DirectUI@@QEAAHXZ"));
    pfnElement_GetHeight = reinterpret_cast<Element_GetInt_t>(GetProcAddress(hDui, "?GetHeight@Element@DirectUI@@QEAAHXZ"));
    pfnElement_GetX = reinterpret_cast<Element_GetInt_t>(GetProcAddress(hDui, "?GetX@Element@DirectUI@@QEAAHXZ"));
    pfnElement_GetY = reinterpret_cast<Element_GetInt_t>(GetProcAddress(hDui, "?GetY@Element@DirectUI@@QEAAHXZ"));
    pfnElement_GetVisible = reinterpret_cast<Element_GetBool_t>(GetProcAddress(hDui, "?GetVisible@Element@DirectUI@@QEAA_NXZ"));
    pfnElement_GetClass = reinterpret_cast<Element_GetClass_t>(GetProcAddress(hDui, "?GetClass@Element@DirectUI@@QEAAPEBGPEAPEAVValue@2@@Z"));
}

static void DumpDuiTreeRec(void* pElement, int depth, int* pBudget)
{
    if (!pElement || depth > 10 || *pBudget <= 0)
        return;
    --*pBudget;

    wchar_t indent[48] = {};
    for (int i = 0; i < depth * 2 && i < 46; ++i)
        indent[i] = L' ';

    void* pClassValue = nullptr;
    const WCHAR* cls = pfnElement_GetClass ? pfnElement_GetClass(pElement, &pClassValue) : nullptr;
    DS_Log(L"DuiTree: %s el=%p class=%s pos=(%d,%d) size=%dx%d visible=%d",
           indent, pElement, cls ? cls : L"(null)",
           pfnElement_GetX ? pfnElement_GetX(pElement) : -1,
           pfnElement_GetY ? pfnElement_GetY(pElement) : -1,
           pfnElement_GetWidth ? pfnElement_GetWidth(pElement) : -1,
           pfnElement_GetHeight ? pfnElement_GetHeight(pElement) : -1,
           pfnElement_GetVisible ? (pfnElement_GetVisible(pElement) ? 1 : 0) : -1);
    if (pClassValue && pfnValue_Release)
        pfnValue_Release(pClassValue);

    void* pChildrenValue = nullptr;
    DuiElementArray* pChildren = pfnElement_GetChildren ? pfnElement_GetChildren(pElement, &pChildrenValue) : nullptr;
    if (pChildren)
    {
        const UINT count = pChildren->sizeAndFlags & 0x0FFFFFFF;
        void** items = (pChildren->sizeAndFlags & 0x10000000)
            ? static_cast<void**>(pChildren->bufferOrFirstItem)
            : &pChildren->bufferOrFirstItem;
        for (UINT i = 0; i < count; ++i)
            DumpDuiTreeRec(items[i], depth + 1, pBudget);
    }
    if (pChildrenValue && pfnValue_Release)
        pfnValue_Release(pChildrenValue);
}

static void DumpDuiTree(void* pRoot)
{
    ResolveDuiDumpFunctions();
    if (!pRoot || IsBadReadPtr(pRoot, sizeof(void*)) || !pfnElement_GetChildren)
    {
        DS_Log(L"DuiTree: cannot dump (root=%p).", pRoot);
        return;
    }
    int budget = 400;
    DS_Log(L"DuiTree: dump begin root=%p", pRoot);
    DumpDuiTreeRec(pRoot, 0, &budget);
    DS_Log(L"DuiTree: dump end (budget left=%d)", budget);
}

HRESULT ApplyStartMenuClassToTree(void* pElement, UINT depth, UINT* pElementCount)
{
    if (!pElement || depth >= 256)
        return E_FAIL;

    HRESULT hr = pfnElement_SetClass(pElement, L"StartMenu");
    DS_Log(L"ApplyClass: depth=%u element=%p SetClass hr=0x%08X",
           depth, pElement, static_cast<unsigned int>(hr));

    if (FAILED(hr))
        return hr;

    if (hr != S_FALSE)
        ++*pElementCount;

    void* pChildrenValue = nullptr;
    DuiElementArray* pChildren = pfnElement_GetChildren(pElement, &pChildrenValue);
    if (pChildren)
    {
        const UINT count = pChildren->sizeAndFlags & 0x0FFFFFFF;
        void** items = (pChildren->sizeAndFlags & 0x10000000)
            ? static_cast<void**>(pChildren->bufferOrFirstItem)
            : &pChildren->bufferOrFirstItem;

        for (UINT i = 0; i < count; ++i)
        {
            HRESULT hrChild = ApplyStartMenuClassToTree(items[i], depth + 1, pElementCount);
            if (FAILED(hrChild))
            {
                hr = hrChild;
                break;
            }
        }
    }

    if (pChildrenValue)
        pfnValue_Release(pChildrenValue);

    return FAILED(hr) ? hr : S_OK;
}

HRESULT __thiscall Hook_CDUIResourceManager_CreateElement(
    void* pThis,
    LPCWSTR pszResID,
    void* pParent,
    void* pUnknown,
    DWORD* pdwDeferCookie,
    void** ppElementOut)
{
    HRESULT hr = pOriginalCDUIResourceManager_CreateElement(
        pThis, pszResID, pParent, pUnknown, pdwDeferCookie, ppElementOut);

    if (g_inCImmersiveSystemTile_Initialize)
    {
        DS_Log(L"CreateElement (in tile init): resid=%s parent=%p hr=0x%08X out=%p",
               (pszResID && !IsBadReadPtr(pszResID, sizeof(WCHAR))) ? pszResID : L"(null)",
               pParent,
               static_cast<unsigned int>(hr),
               ppElementOut ? *ppElementOut : nullptr);
    }

    if (g_inCImmersiveSystemTile_Initialize && SUCCEEDED(hr) && pParent &&
        pszResID && _wcsicmp(pszResID, L"main") == 0)
    {
        g_pSystemTileRootElement = pParent;
        DS_Log(L"CreateElement Hook: Captured system-tile root. ResID = %s, Root = %p",
               pszResID, g_pSystemTileRootElement);
    }
    return hr;
}

HRESULT __fastcall Hook_CImmersiveSystemTile__Initialize(void* pThis)
{
    DS_Log(L"CImmersiveSystemTile::_Initialize: Hook triggered for instance = %p, thread = %lu",
           pThis, GetCurrentThreadId());

    g_inCImmersiveSystemTile_Initialize = true;
    g_pSystemTileRootElement = nullptr;

    HRESULT hr = pOriginalCImmersiveSystemTile__Initialize(pThis);

    g_inCImmersiveSystemTile_Initialize = false;

    DS_Log(L"CImmersiveSystemTile::_Initialize: Base initialization returned HRESULT = 0x%08X", hr);

    if (FAILED(hr))
    {
        DS_Log(L"CImmersiveSystemTile::_Initialize: Aborting styling. Base initialization returned failure.");
        return hr;
    }

    if (!g_pSystemTileRootElement)
    {
        DS_Log(L"CImmersiveSystemTile::_Initialize: Error. Failed to capture the CSystemTileElement root.");
        return hr;
    }

    if (!pfnElement_SetClass || !pfnElement_GetChildren || !pfnValue_Release)
    {
        DS_Log(L"CImmersiveSystemTile::_Initialize: Fatal. Essential DirectUI functions are missing or unmapped.");
        return hr;
    }

    UINT elementCount = 0;
    HRESULT hrClass = ApplyStartMenuClassToTree(g_pSystemTileRootElement, 0, &elementCount);
    DS_Log(L"CImmersiveSystemTile::_Initialize: recursive SetClass(\"StartMenu\") returned 0x%08X after %u elements",
           hrClass, elementCount);

    g_pSystemTileRootElement = nullptr;
    return hr;
}

struct LauncherSettings_t
{
    char pad[2]; // Offset 0 to 1
    bool _fNeedToReadForcedScaleFactorFromRegistry; // Offset 2
    char pad2[9]; // Offset 3 to 11
    int32_t _forcedScaleFactorForAllParts; // Offset 12 (0xC)
    char pad3[8]; // Offset 16 to 23
    HWND _hwndForScaleFactors; // Offset 24 (0x18)
};

LauncherSettings_t* g_pLauncherSettings = nullptr;

typedef HRESULT (__cdecl *CHostElement_Create_t)(
    void* pLauncherApps,
    bool punkSite,
    unsigned int ppeHost,
    IUnknown* a4,
    void** fDblBuffer
);
CHostElement_Create_t pOriginalCHostElement_Create = nullptr;

HRESULT Hook_CHostElement_Create(
    void* pLauncherApps,
    bool punkSite,
    unsigned int ppeHost,
    IUnknown* a4,
    void** fDblBuffer)
{
    DS_Log(L"CHostElement::Create: Hook triggered. pLauncherApps=%p", pLauncherApps);

    int32_t saved_forcedScaleFactorForAllParts = 0;
    bool saved_fNeedToReadForcedScaleFactorFromRegistry = false;
    HWND saved_hwndForScaleFactors = nullptr;

    if (g_pLauncherSettings)
    {
        saved_forcedScaleFactorForAllParts = g_pLauncherSettings->_forcedScaleFactorForAllParts;
        saved_fNeedToReadForcedScaleFactorFromRegistry = g_pLauncherSettings->_fNeedToReadForcedScaleFactorFromRegistry;
        saved_hwndForScaleFactors = g_pLauncherSettings->_hwndForScaleFactors;

        DS_Log(L"CHostElement::Create [Pre-Call Save]: "
               L"scaleFactor=%d, needToReadReg=%d, hwnd=%p",
               saved_forcedScaleFactorForAllParts,
               saved_fNeedToReadForcedScaleFactorFromRegistry,
               saved_hwndForScaleFactors);
    }
    else
    {
        DS_Log(L"CHostElement::Create Warning: g_pLauncherSettings pointer is null!");
    }

    HRESULT hr = pOriginalCHostElement_Create(pLauncherApps, punkSite, ppeHost, a4, fDblBuffer);

    if (IsStartMenuEnabled() && g_pLauncherSettings)
    {
        g_pLauncherSettings->_forcedScaleFactorForAllParts = saved_forcedScaleFactorForAllParts;
        g_pLauncherSettings->_fNeedToReadForcedScaleFactorFromRegistry = saved_fNeedToReadForcedScaleFactorFromRegistry;
        g_pLauncherSettings->_hwndForScaleFactors = saved_hwndForScaleFactors;

        DS_Log(L"CHostElement::Create [Post-Call Restore]: "
               L"Restored values because IsStartMenuEnabled() is TRUE. "
               L"scaleFactor=%d, needToReadReg=%d, hwnd=%p",
               g_pLauncherSettings->_forcedScaleFactorForAllParts,
               g_pLauncherSettings->_fNeedToReadForcedScaleFactorFromRegistry,
               g_pLauncherSettings->_hwndForScaleFactors);
    }

    DS_Log(L"CHostElement::Create: Finished execution. HR=0x%08X", hr);
    return hr;
}

typedef HRESULT (__cdecl *RichText_SetBaseline_t)(void* pThis, int baseline);
RichText_SetBaseline_t pfnRichText_SetBaseline = nullptr;

typedef HRESULT (__cdecl *RichText_SetLineSpacing_t)(void* pThis, int spacing);
RichText_SetLineSpacing_t pfnRichText_SetLineSpacing = nullptr;

typedef int (__cdecl *GetScaledIntFromResource_t)(unsigned int resId, int scaleFactor);
GetScaledIntFromResource_t pfnGetScaledIntFromResource = nullptr;

typedef DEVICE_SCALE_FACTOR (__cdecl *CLauncherSettings_GetScaleFactorForPart_t)(void* pThis, int part);
CLauncherSettings_GetScaleFactorForPart_t pfnGetScaleFactorForPart = nullptr;

typedef void (__fastcall *CSystemTileElement_AdjustNameBaselineLinespacing_t)(void* pThis);
CSystemTileElement_AdjustNameBaselineLinespacing_t pOriginalCSystemTileElement_AdjustNameBaselineLinespacing = nullptr;

inline HRESULT ResultFromWin32Count(UINT cchResult, UINT cchBuffer)
{
    return cchResult && cchResult <= cchBuffer ? S_OK : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
}

inline int GetScaledInt(int iValue, DEVICE_SCALE_FACTOR scaleFactor)
{
    if (iValue > 0)
    {
        return iValue * scaleFactor / 100;
    }
    return iValue;
}

inline int GetScaledIntFromResource(UINT uResId, DEVICE_SCALE_FACTOR scaleFactor)
{
    int iValue = 0;

    WCHAR szValue[12];
    int cchResult = LoadStringW(g_hTwinUiMui, uResId, szValue, ARRAYSIZE(szValue));
    if (SUCCEEDED(ResultFromWin32Count(cchResult, ARRAYSIZE(szValue))))
    {
        iValue = GetScaledInt(_wtoi(szValue), scaleFactor);
    }
    return iValue;
}

void LogTwinUiStyleStrings(void* pNameElement)
{
    static bool s_done = false;
    if (s_done || !DS_GetIntSetting(L"theme_debug"))
        return;
    s_done = true;

    DS_Log(L"twinui MUI handle = %p", g_hTwinUiMui);
    for (UINT id = 9820; id <= 9830; ++id)
    {
        WCHAR text[160] = {};
        int len = g_hTwinUiMui ? LoadStringW(g_hTwinUiMui, id, text, ARRAYSIZE(text)) : -1;
        DS_Log(L"twinui string %u: len=%d text=\"%s\"", id, len, text);
    }
    for (UINT id = 12000; id <= 12030; ++id)
    {
        WCHAR text[160] = {};
        int len = g_hTwinUiMui ? LoadStringW(g_hTwinUiMui, id, text, ARRAYSIZE(text)) : -1;
        DS_Log(L"twinui string %u: len=%d text=\"%s\"", id, len, text);
    }

    typedef int (__cdecl *Element_GetFontSize_t)(void*);
    HMODULE hDui70 = GetModuleHandleW(L"dui70.dll");
    Element_GetFontSize_t pfnGetFontSize = hDui70
        ? reinterpret_cast<Element_GetFontSize_t>(
              GetProcAddress(hDui70, "?GetFontSize@Element@DirectUI@@QEAAHXZ"))
        : nullptr;
    if (pfnGetFontSize && pNameElement)
        DS_Log(L"display name element %p font size = %d", pNameElement, pfnGetFontSize(pNameElement));
}

void __fastcall Hook_CSystemTileElement_AdjustNameBaselineLinespacing(void* pThis)
{
    DS_Log(L"AdjustNameBaselineLinespacing: Hook triggered for instance = %p", pThis);

    pOriginalCSystemTileElement_AdjustNameBaselineLinespacing(pThis);

    if (!IsStartMenuEnabled() || !pThis)
        return;

    if (!pfnGetScaleFactorForPart || !pfnRichText_SetBaseline || !pfnRichText_SetLineSpacing)
    {
        DS_Log(L"AdjustNameBaselineLinespacing: Error. Missing required function pointers.");
        return;
    }


    if (!g_pLauncherSettings)
    {
        DS_Log(L"AdjustNameBaselineLinespacing: g_pLauncherSettings is null, skipping.");
        return;
    }

    DEVICE_SCALE_FACTOR scaleFactor = pfnGetScaleFactorForPart(g_pLauncherSettings, 0);
    DS_Log(L"AdjustNameBaselineLinespacing: ScaleFactor metric resolved = %d", scaleFactor);

    void* pPeDisplayName = *reinterpret_cast<void**>((char*)pThis + 0x128);
    DS_Log(L"AdjustNameBaselineLinespacing: _peDisplayName pointer = %p", pPeDisplayName);

    if (!pPeDisplayName)
    {
        DS_Log(L"AdjustNameBaselineLinespacing: there is no element.");
        return;
    }

    int baseLine = GetScaledIntFromResource(12019, scaleFactor);
    if (!baseLine)
        baseLine = 18;
    DS_Log(L"baseLine = %d", baseLine);
    pfnRichText_SetBaseline(pPeDisplayName, baseLine);

    int lineSpacing = GetScaledIntFromResource(12022, scaleFactor);
    if (!lineSpacing)
        lineSpacing = 28;
    DS_Log(L"lineSpacing = %d", lineSpacing);
    pfnRichText_SetLineSpacing(pPeDisplayName, lineSpacing);

    LogTwinUiStyleStrings(pPeDisplayName);

    DS_Log(L"AdjustNameBaselineLinespacing: success.");
}

typedef HRESULT (__fastcall *CImmersiveLauncher_ShowStartView_t)(void* pThis, __int32 mMethod, __int32 mFlags);
CImmersiveLauncher_ShowStartView_t pOriginalCImmersiveLauncher_ShowStartView = nullptr;
HRESULT __fastcall Hook_CImmersiveLauncher_ShowStartView(void* pThis, __int32 mMethod, __int32 mFlags)
{
    if (IsStartMenuEnabled())
    {
        HWND hWndTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
        if (hWndTaskbar)
        {
            PostMessageW(hWndTaskbar, 0x568, (WPARAM)1, (LPARAM)0); 
        }
        return 0; 
    }
    return pOriginalCImmersiveLauncher_ShowStartView(pThis, mMethod, mFlags);
}

typedef bool (__fastcall *CPearl_IsStartMenuOpen_t)(void* pThis);
CPearl_IsStartMenuOpen_t pfnCPearl_IsStartMenuOpen = nullptr;
static ULONGLONG g_lastMenuCloseTick = 0;
static bool PearlGetMenuOpen(void* pPearl);
void __fastcall Hook_CPearl_CloseStartMenu(void* pThis);

LRESULT __fastcall Hook_CTray_v_WndProc(void* pThis, HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (IsStartMenuEnabled())
    {
        switch (uMsg)
        {
            case 0x40D: 
                if (pThis)
                {
                    Hook_CPearl_BuildStartMenu((char*)pThis + 1064);
                }
                return 0;

            case 0x568:
                DS_Log(L"CTray::v_WndProc: displaying through IMenuPopup::Popup.");
                if (pThis && pOriginalCPearl_DisplayStartMenu)
                {
                    void* pPearl = (char*)pThis + 1064;
                    const bool isOpen = pfnCPearl_IsStartMenuOpen
                        ? pfnCPearl_IsStartMenuOpen(pPearl)
                        : PearlGetMenuOpen(pPearl);
                    const bool toggles = wParam <= 11 && ((0x826u >> wParam) & 1u) != 0;

                    if (isOpen && toggles)
                    {
                        Hook_CPearl_CloseStartMenu(pPearl);
                    }
                    else if (!isOpen && toggles && GetTickCount64() - g_lastMenuCloseTick < 300)
                    {
                        // no
                    }
                    else
                    {
                        Hook_CPearl_DisplayStartMenu(pPearl);
                    }
                }
                return 0;
        }
    }
    return pOriginalCTray_v_WndProc(pThis, hWnd, uMsg, wParam, lParam);
}

static void PearlSetMenuOpen(void* pPearl, bool fOpen);

void __fastcall Hook_CPearl_CloseStartMenu(void* pThis)
{
    PearlSetMenuOpen(pThis, false);
    pOriginalCPearl_CloseStartMenu(pThis);
}

void __fastcall Hook_CPearl_DestroyStartMenu(void* pThis)
{ 
    PearlSetMenuOpen(pThis, false);
    pOriginalCPearl_DestroyStartMenu(pThis);
}

HRESULT __fastcall Hook_CPearl_UpdateTheme(void* pPearl)
{ 
    return pOriginalCPearl_UpdateTheme(pPearl);
}

HRESULT __cdecl Hook_DesktopV2_Create(
    IMenuPopup** ppMenuPopup,
    IMenuBand** ppMenuBand,
    void** ppDeskHost,
    IUnknown** ppUnknown,
    HWND hWnd
)
{ 
    return pOriginalDesktopV2_Create(ppMenuPopup, ppMenuBand, ppDeskHost, ppUnknown, hWnd);
}

static void LogGuidValue(const wchar_t* tag, const GUID* g)
{
    if (!g)
    {
        DS_Log(L"%s: (null guid)", tag);
        return;
    }
    DS_Log(L"%s: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", tag,
           g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
           g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

typedef HRESULT (__thiscall *GridPane_GetControl_t)(void*, const GUID*, void**);
GridPane_GetControl_t pOriginalGridPane_GetControl = nullptr;
HRESULT __thiscall Hook_GridPane_GetControl(void* pThis, const GUID* pGuid, void** ppv)
{
    LogGuidValue(L"GridPane::GetControl: request", pGuid);
    HRESULT hr = pOriginalGridPane_GetControl(pThis, pGuid, ppv);
    DS_Log(L"GridPane::GetControl: hr=0x%08X out=%p", static_cast<unsigned int>(hr), ppv ? *ppv : nullptr);
    return hr;
}

typedef HRESULT (__thiscall *StartMenuHost_CreateControl_t)(void*, HWND, void*, void*);
StartMenuHost_CreateControl_t pOriginalStartMenuHost_CreateControl = nullptr;
HRESULT __thiscall Hook_StartMenuHost_CreateControl(void* pThis, HWND hwnd, void* pHost, void* pSettings)
{
    DS_Log(L"CStartMenuHost::CreateControl: begin this=%p hwnd=%p host=%p settings=%p", pThis, hwnd, pHost, pSettings);
    HRESULT hr = pOriginalStartMenuHost_CreateControl(pThis, hwnd, pHost, pSettings);
    DS_Log(L"CStartMenuHost::CreateControl: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef HRESULT (__thiscall *StartMenuHost_SetItemCollection_t)(void*, void*);
StartMenuHost_SetItemCollection_t pOriginalStartMenuHost_SetItemCollection = nullptr;
HRESULT __thiscall Hook_StartMenuHost_SetItemCollection(void* pThis, void* pCollection)
{
    DS_Log(L"CStartMenuHost::SetItemCollection: begin this=%p collection=%p", pThis, pCollection);
    HRESULT hr = pOriginalStartMenuHost_SetItemCollection(pThis, pCollection);
    DS_Log(L"CStartMenuHost::SetItemCollection: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef HRESULT (__thiscall *StartMenuHost_CreateMvc_t)(void*);
StartMenuHost_CreateMvc_t pOriginalStartMenuHost_CreateMvc = nullptr;
HRESULT __thiscall Hook_StartMenuHost_CreateMvc(void* pThis)
{
    DS_Log(L"CStartMenuHost::_CreateMvcComponents: begin this=%p", pThis);
    HRESULT hr = pOriginalStartMenuHost_CreateMvc(pThis);
    DS_Log(L"CStartMenuHost::_CreateMvcComponents: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

#define DEFINE_VOID_LOG_HOOK(Id, Label)                                              \
    typedef void (__thiscall *Id##_t)(void*);                                        \
    static Id##_t pOriginal_##Id = nullptr;                                          \
    static void __thiscall Hook_##Id(void* pThis)                                    \
    {                                                                                \
        DS_Log(L"" Label ": begin this=%p", pThis);                                  \
        pOriginal_##Id(pThis);                                                       \
        DS_Log(L"" Label ": end");                                                   \
    }

DEFINE_VOID_LOG_HOOK(StartMenuHost_OnHostShowing, L"CStartMenuHost::OnHostShowing")
DEFINE_VOID_LOG_HOOK(StartMenuHost_OnHostShown, L"CStartMenuHost::OnHostShown")
DEFINE_VOID_LOG_HOOK(StartMenuHost_OnHostHiding, L"CStartMenuHost::OnHostHiding")
DEFINE_VOID_LOG_HOOK(StartMenuHost_OnHostHidden, L"CStartMenuHost::OnHostHidden")
DEFINE_VOID_LOG_HOOK(StartMenuHost_OnVisibleRealized, L"CStartMenuHost::OnVisibleRealized")
DEFINE_VOID_LOG_HOOK(StartMenuHost_OnFullyRendered, L"CStartMenuHost::OnFullyRendered")
DEFINE_VOID_LOG_HOOK(StartMenuHost_ClearTileNotifications, L"CStartMenuHost::ClearTileNotifications")

typedef HRESULT (__thiscall *StartMenuHost_RefreshContents_t)(void*);
static StartMenuHost_RefreshContents_t pOriginalStartMenuHost_RefreshContents = nullptr;
static HRESULT __thiscall Hook_StartMenuHost_RefreshContents(void* pThis)
{
    DS_Log(L"CStartMenuHost::RefreshContents: begin this=%p", pThis);
    HRESULT hr = pOriginalStartMenuHost_RefreshContents(pThis);
    DS_Log(L"CStartMenuHost::RefreshContents: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef void (__thiscall *GridPane_OnVisibility_t)(void*, bool);
static GridPane_OnVisibility_t pOriginalGridPane_OnVisibilityChanged = nullptr;
static GridPane_OnVisibility_t pOriginalGridPane_OnVisibilityChanging = nullptr;
static void __thiscall Hook_GridPane_OnVisibilityChanged(void* pThis, bool fVisible)
{
    DS_Log(L"GridPane::OnStartMenuVisibilityChanged: visible=%d this=%p", fVisible ? 1 : 0, pThis);
    pOriginalGridPane_OnVisibilityChanged(pThis, fVisible);
}
static void __thiscall Hook_GridPane_OnVisibilityChanging(void* pThis, bool fVisible)
{
    DS_Log(L"GridPane::OnStartMenuVisibilityChanging: visible=%d this=%p", fVisible ? 1 : 0, pThis);
    pOriginalGridPane_OnVisibilityChanging(pThis, fVisible);
}

#define DEFINE_TILE_TRACE_HOOK(Id, Label)                                                    \
    typedef INT_PTR (__thiscall *Id##_t)(void*, void*, void*, void*, void*);                 \
    static Id##_t pOriginal_##Id = nullptr;                                                  \
    static volatile LONG g_traceCount_##Id = 0;                                              \
    static INT_PTR __thiscall Hook_##Id(void* a, void* b, void* c, void* d, void* e)         \
    {                                                                                        \
        const LONG n = InterlockedIncrement(&g_traceCount_##Id);                             \
        if (n <= 16)                                                                          \
            DS_Log(L"TRACE enter #%ld " Label ": this=%p a2=%p a3=%p", n, a, b, c);          \
        INT_PTR r = pOriginal_##Id(a, b, c, d, e);                                           \
        if (n <= 16)                                                                          \
            DS_Log(L"TRACE leave #%ld " Label ": ret=0x%llX", n, static_cast<unsigned long long>(r)); \
        return r;                                                                            \
    }

DEFINE_TILE_TRACE_HOOK(TT_DS_OnCollectionChanged, L"CTileItemDataSource::OnCollectionChanged")
DEFINE_TILE_TRACE_HOOK(TT_DS_OnItemsAdded, L"CTileItemDataSource::OnItemsAdded")
DEFINE_TILE_TRACE_HOOK(TT_DS_SetCollection, L"CTileItemDataSource::SetCollection")
DEFINE_TILE_TRACE_HOOK(TT_Coll_OnCollectionChange, L"CTileCollection::OnCollectionChange")
DEFINE_TILE_TRACE_HOOK(TT_Shaper_OnItemsAdded, L"CGroupShaper::OnItemsAdded")
DEFINE_TILE_TRACE_HOOK(TT_Model_OnItemsAdded, L"CTileModel::OnItemsAdded")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_OnTilesAdded, L"CTileController::OnTilesAdded")
DEFINE_TILE_TRACE_HOOK(TT_Layout_AddItem, L"CTileLayout::AddItem")
DEFINE_TILE_TRACE_HOOK(TT_View_AddTile, L"CTileView::AddTile")
DEFINE_TILE_TRACE_HOOK(TT_View_Initialize, L"CTileView::_Initialize")
DEFINE_TILE_TRACE_HOOK(TT_View_SetSize, L"CTileView::SetSize")
DEFINE_TILE_TRACE_HOOK(TT_View_UpdateLayout, L"CTileView::UpdateLayout")
DEFINE_TILE_TRACE_HOOK(TT_View_CreateNewTile, L"CTileView::_CreateNewTile")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_PerformRealization, L"CTileController::PerformRealization")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_PerformRealizationPriv, L"CTileController::_PerformRealization")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_RealizeTile, L"CTileController::_RealizeTile")
DEFINE_TILE_TRACE_HOOK(TT_Factory_CreateTileElement, L"CTileUIFactory::CreateTileElement")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_NotifyModelAboutRealization, L"CTileController::_NotifyModelAboutRealization")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_BuildRealizationRequestsVisible, L"CTileController::_BuildModelRealizationRequestsForVisibleTiles")
DEFINE_TILE_TRACE_HOOK(TT_Model_SetRealizedTiles, L"CTileModel::SetRealizedTiles")
DEFINE_TILE_TRACE_HOOK(TT_Model_UpdateNewlyRealizedTiles, L"CTileModel::_UpdateNewlyRealizedTiles")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_OnNewlyRealizedTilesUpdated, L"CTileController::OnNewlyRealizedTilesUpdated")
DEFINE_TILE_TRACE_HOOK(TT_Grid_OnNewlyRealizedTilesUpdated, L"CGridModelModify::OnNewlyRealizedTilesUpdated")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_UpdateTile, L"CTileController::_UpdateTile")
DEFINE_TILE_TRACE_HOOK(TT_View_OnTilesRealized, L"CTileView::OnTilesRealized")
DEFINE_TILE_TRACE_HOOK(TT_Ctrl_OnVisibleRegionRealized, L"CTileController::_OnVisibleRegionRealized")
DEFINE_TILE_TRACE_HOOK(TT_View_InitializeNewTileElement, L"CTileView::_InitializeNewTileElement")
DEFINE_TILE_TRACE_HOOK(TD_CreateLiveTileDataProvider, L"CreateLiveTileDataProvider")
DEFINE_TILE_TRACE_HOOK(TD_LiveProvider_Ctor, L"CLiveTileDataProvider::CLiveTileDataProvider")
DEFINE_TILE_TRACE_HOOK(TD_LiveProvider_UpdateTile, L"CLiveTileDataProvider::UpdateTile")
DEFINE_TILE_TRACE_HOOK(TD_Model_OnTileDataAvailable, L"CTileModel::OnTileDataAvailable")
DEFINE_TILE_TRACE_HOOK(TD_Ctrl_OnTileDataUpdated, L"CTileController::OnTileDataUpdated")
DEFINE_TILE_TRACE_HOOK(TD_Ctrl_UpdateTileData, L"CTileController::_UpdateTileData")
DEFINE_TILE_TRACE_HOOK(TD_DS_GetTileItem, L"CTileItemDataSource::GetTileItem")
DEFINE_TILE_TRACE_HOOK(TD_Item_GetTileRenderingData, L"CTileItem::GetTileRenderingData")
DEFINE_TILE_TRACE_HOOK(TD_DS_UpdateTileItems, L"CTileItemDataSource::UpdateTileItems")
DEFINE_TILE_TRACE_HOOK(TD_DS_GenerateUpdateNotifications, L"CTileItemDataSource::GenerateUpdateNotifications")
DEFINE_TILE_TRACE_HOOK(TD_Model_RefreshTileData, L"CTileModel::RefreshTileData")
DEFINE_TILE_TRACE_HOOK(TD_Ctrl_FlushPendingTileData, L"CTileController::FlushPendingTileData")
DEFINE_TILE_TRACE_HOOK(TD_Model_GetTileItemPriv, L"CTileModel::_GetTileItem")
// ===== DEAD TILES FIX - CLiveTileDataProvider::SubscribeToTiles =====
// The x64 implementation returns immediately when _zoomLevel (@0x6C) is 1,
// leaving the menu tile view without subscriptions. Force the menu-provider
// path while calling the original function.
typedef HRESULT (__thiscall *TP_SubscribeToTiles_t)(
    void* pThis, UINT cTiles, const void* requests, void* sink, int options);
static TP_SubscribeToTiles_t pOriginal_TP_SubscribeToTiles = nullptr;
static volatile LONG g_subscribeFixCount = 0;
static HRESULT __thiscall Hook_TP_SubscribeToTiles(
    void* pThis,
    UINT cTiles,
    const void* requests,
    void* sink,
    int options)
{
    DWORD* pEnv = pThis ? reinterpret_cast<DWORD*>(reinterpret_cast<char*>(pThis) + 0x6C) : nullptr;
    if (!pEnv || IsBadReadPtr(pEnv, sizeof(DWORD)))
    {
        DS_Log(L"SubscribeToTiles: rejecting invalid this=%p", pThis);
        return E_INVALIDARG;
    }

    const bool fix = *pEnv == 1 && IsStartMenuEnabled();
    if (fix)
        *pEnv = 0;

    HRESULT hr = pOriginal_TP_SubscribeToTiles(
        pThis, cTiles, requests, sink, options);

    if (fix)
        *pEnv = 1;

    const LONG n = InterlockedIncrement(&g_subscribeFixCount);
    if (n <= 20)
    {
        DS_Log(L"SubscribeToTiles: this=%p tiles=%u requests=%p sink=%p "
               L"menu-provider=%d hr=0x%08X",
               pThis,
               cTiles,
               requests,
               sink,
               fix ? 1 : 0,
               static_cast<unsigned int>(hr));
    }
    return hr;
}
DEFINE_TILE_TRACE_HOOK(TP_LoadLogoIfCacheNotUsed, L"CLiveTileDataProvider::_LoadLogoAndDefaultAppSpaceIfCacheNotUsed")
DEFINE_TILE_TRACE_HOOK(TP_PublishAllStartViewCached, L"CLiveTileDataProvider::_PublishAllStartViewCachedData")
DEFINE_TILE_TRACE_HOOK(TP_PublishLogoAndAppSpace, L"CLiveTileDataProvider::_PublishLogoAndAppSpaceCachedData")
DEFINE_TILE_TRACE_HOOK(TP_PublishDefaultCached, L"CLiveTileDataProvider::_PublishDefaultCachedData")
DEFINE_TILE_TRACE_HOOK(TP_StartCachePreLoadResult, L"CLiveTileDataProvider::_StartCachePreLoadResult")
DEFINE_TILE_TRACE_HOOK(TP_DetermineAvailableMain, L"CLiveTileDataProvider::_DetermineAvailableMainCachedData")
DEFINE_TILE_TRACE_HOOK(TP_Dispatcher_Create, L"CLiveTileDataProviderEventDispatcher::Create")
DEFINE_TILE_TRACE_HOOK(TP_Dispatcher_PostEvent, L"CLiveTileDataProviderEventDispatcher::_PostEvent")
DEFINE_TILE_TRACE_HOOK(TP_Dispatcher_WndProc, L"CLiveTileDataProviderEventDispatcher::s_WndProc")


// crappy impl for pearl coloring on menu open

#define PEARL_RT_COLOR_TYPE_START 282

static DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;

struct PearlGateScope
{
    PearlGateScope()  { Adjust(+1); }
    ~PearlGateScope() { Adjust(-1); }
    static void Adjust(int delta)
    {
        if (g_pearlGateTls == TLS_OUT_OF_INDEXES)
            return;
        const DWORD err = GetLastError();
        UINT_PTR v = reinterpret_cast<UINT_PTR>(TlsGetValue(g_pearlGateTls));
        v += delta;
        TlsSetValue(g_pearlGateTls, reinterpret_cast<LPVOID>(v));
        SetLastError(err);
    }
};

static bool InPearlGate()
{
    if (g_pearlGateTls == TLS_OUT_OF_INDEXES)
        return false;
    const DWORD err = GetLastError();
    const bool in = TlsGetValue(g_pearlGateTls) != nullptr;
    SetLastError(err);
    return in;
}

struct PearlLayerRenderData
{
    int f[9];
};
static_assert(sizeof(PearlLayerRenderData) == 36, "PEARL_LAYER_RENDER_DATA size");

typedef DWORD (__fastcall *ImmersiveColor_GetColor_t)(int type);
ImmersiveColor_GetColor_t pOriginalImmersiveColor_GetColor = nullptr;

static bool IsHighContrastActive()
{
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

static DWORD GetRTStartMenuColor()
{
    DWORD value = 0;
    DWORD type = 0;
    DWORD cb = sizeof(value);
    if (SHGetValueW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartMenu\\Colors",
                    L"OverrideColor", &type, &value, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD && cb == sizeof(value))
    {
        return value;
    }
    if (IsHighContrastActive())
        return GetSysColor(COLOR_WINDOW);
    return pOriginalImmersiveColor_GetColor ? pOriginalImmersiveColor_GetColor(0)
                                            : 0xFF000000;
}

DWORD __fastcall Hook_ImmersiveColor_GetColor(int type)
{
    if (type == PEARL_RT_COLOR_TYPE_START && IsStartMenuEnabled() && InPearlGate())
    {
        return GetRTStartMenuColor();
    }
    return pOriginalImmersiveColor_GetColor(type);
}

typedef HRESULT (__fastcall *Pearl_LoadColorizedImage_t)(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp);
Pearl_LoadColorizedImage_t pOriginalPearl_LoadColorizedImage = nullptr;

struct PearlLayer3Entry
{
    int w;
    int h;
    DWORD d7;
    bool valid;
};
static PearlLayer3Entry g_pearlLayer3[8] = {};

static void PearlRememberLayer3(const PearlLayerRenderData* d)
{
    PearlLayer3Entry* freeSlot = nullptr;
    for (PearlLayer3Entry& e : g_pearlLayer3)
    {
        if (e.valid && e.w == d->f[3] && e.h == d->f[4])
        {
            e.d7 = static_cast<DWORD>(d->f[7]);
            return;
        }
        if (!e.valid && !freeSlot)
            freeSlot = &e;
    }
    if (freeSlot)
    {
        freeSlot->w = d->f[3];
        freeSlot->h = d->f[4];
        freeSlot->d7 = static_cast<DWORD>(d->f[7]);
        freeSlot->valid = true;
    }
}

static bool PearlLookupLayer3(const PearlLayerRenderData* d, DWORD* pd7)
{
    for (const PearlLayer3Entry& e : g_pearlLayer3)
    {
        if (e.valid && e.w == d->f[3] && e.h == d->f[4])
        {
            *pd7 = e.d7;
            return true;
        }
    }
    return false;
}

HRESULT __fastcall Hook_Pearl_LoadColorizedImage(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp)
{
    if (pData && IsStartMenuEnabled() && pData->f[6] == 1)
    {
        if (pData->f[8] == 0)
        {
            PearlRememberLayer3(pData);
        }
        else if (pData->f[8] == PEARL_RT_COLOR_TYPE_START)
        {
            DWORD l3 = 0;
            if (PearlLookupLayer3(pData, &l3))
            {
                const DWORD before = static_cast<DWORD>(pData->f[7]);
                const DWORD after = (before & 0x00FF0000u) | (l3 & 0xFF00FFFFu);
                pData->f[7] = static_cast<int>(after);
                static LONG s_logged = 0;
                if (InterlockedIncrement(&s_logged) <= 8)
                {
                    DS_Log(L"Pearl RT: layer5 %dx%d d7 0x%08X -> 0x%08X (layer3 0x%08X)",
                           pData->f[3], pData->f[4], before, after, l3);
                }
            }
            else
            {
                DS_Log(L"Pearl RT: layer5 %dx%d - brak warstwy 3 w cache, bez zmian",
                       pData->f[3], pData->f[4]);
            }
        }
    }
    PearlGateScope gate;
    return pOriginalPearl_LoadColorizedImage(pData, fFlag, phbmp);
}

typedef HRESULT (__fastcall *Pearl_RepaintInternal_t)(
    void* pThis, HDC hdc, int target, const void* pData);
Pearl_RepaintInternal_t pOriginalPearl_RepaintInternal = nullptr;

HRESULT __fastcall Hook_Pearl_RepaintInternal(
    void* pThis, HDC hdc, int target, const void* pData)
{
    PearlGateScope gate;
    return pOriginalPearl_RepaintInternal(pThis, hdc, target, pData);
}

typedef void (__fastcall *Pearl_UpdateLayerVisibility_t)(void* pThis);
Pearl_UpdateLayerVisibility_t pOriginalPearl_UpdateLayerVisibility = nullptr;

void __fastcall Hook_Pearl_UpdateLayerVisibility(void* pThis)
{
    pOriginalPearl_UpdateLayerVisibility(pThis);
    if (pThis && IsStartMenuEnabled() &&
        *reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + 0x1C) == 2)
    {
        *(reinterpret_cast<BYTE*>(pThis) + 0x7C + 0x24 * 1) = 0;
    }
}

// Servicing builds may ship without usable PDBs. These patterns keep opcodes and
// stable immediates while wildcarding branch and RIP-relative displacements.
struct ParsedCodePattern
{
    BYTE values[128];
    BYTE masks[128];
    size_t length;
};

static bool ParseCodePattern(const char* pattern, ParsedCodePattern* parsed)
{
    const char* cursor = pattern;
    size_t length = 0;
    while (cursor && *cursor)
    {
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        if (!*cursor)
            break;
        if (length >= ARRAYSIZE(parsed->values))
            return false;

        if (*cursor == '?')
        {
            parsed->values[length] = 0;
            parsed->masks[length] = 0;
            while (*cursor == '?')
                ++cursor;
        }
        else
        {
            char* end = nullptr;
            const unsigned long value = strtoul(cursor, &end, 16);
            if (end == cursor || value > 0xFF)
                return false;
            parsed->values[length] = static_cast<BYTE>(value);
            parsed->masks[length] = 0xFF;
            cursor = end;
        }
        ++length;
    }
    parsed->length = length;
    return length != 0;
}

static BYTE* FindUniqueCodePattern(
    HMODULE module, const char* pattern, LPCWSTR name)
{
    if (!module || !pattern)
        return nullptr;

    ParsedCodePattern parsed = {};
    if (!ParseCodePattern(pattern, &parsed))
    {
        DS_Log(L"TWinUI pattern parse failed: %s", name);
        return nullptr;
    }

    BYTE* moduleBase = reinterpret_cast<BYTE*>(module);
    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto ntHeaders =
        reinterpret_cast<PIMAGE_NT_HEADERS>(moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    BYTE* match = nullptr;
    size_t matchCount = 0;
    auto section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;

        const size_t sectionSize = section->Misc.VirtualSize
            ? section->Misc.VirtualSize
            : section->SizeOfRawData;
        if (sectionSize < parsed.length)
            continue;

        BYTE* sectionStart = moduleBase + section->VirtualAddress;
        for (size_t offset = 0; offset <= sectionSize - parsed.length; ++offset)
        {
            bool matches = true;
            for (size_t j = 0; j < parsed.length; ++j)
            {
                if ((sectionStart[offset + j] & parsed.masks[j]) !=
                    (parsed.values[j] & parsed.masks[j]))
                {
                    matches = false;
                    break;
                }
            }
            if (!matches)
                continue;

            ++matchCount;
            if (matchCount == 1)
            {
                match = sectionStart + offset;
            }
            else
            {
                DS_Log(L"TWinUI pattern is not unique: %s", name);
                return nullptr;
            }
        }
    }

    if (!match)
        DS_Log(L"TWinUI pattern not found: %s", name);
    return match;
}

static bool ResolveTWinUIFunctionByPattern(
    HMODULE module,
    const char* pattern,
    LPCWSTR name,
    void** function)
{
    if (!function)
        return false;
    if (*function)
        return true;
    *function = FindUniqueCodePattern(module, pattern, name);
    return *function != nullptr;
}

static bool InstallTWinUIHookByPattern(
    HMODULE module,
    const char* pattern,
    LPCWSTR name,
    void* hookFunction,
    void** originalFunction)
{
    if (!originalFunction)
        return false;
    if (*originalFunction)
        return true;

    void* target = FindUniqueCodePattern(module, pattern, name);
    if (!target)
        return false;

    if (!DS_HookFunction(target, hookFunction, originalFunction))
    {
        DS_Log(L"TWinUI pattern hook failed: %s target=%p", name, target);
        return false;
    }

    DS_Log(L"TWinUI pattern hook: %s target=%p", name, target);
    return true;
}

static bool ResolveLauncherSettingsByPattern(HMODULE module)
{
    if (g_pLauncherSettings)
        return true;

    if (!pfnGetScaleFactorForPart)
    {
        const char* pattern =
            "48 89 4C 24 ? 48 83 EC 28 8B 05 ? ? ? ? "
            "B9 64 00 00 00 89 4C 24 ? 85 C0 0F 85 ? ? ? ? "
            "85 D2 0F 85 ? ? ? ? 48 8B 05 ? ? ? ?";
        if (!ResolveTWinUIFunctionByPattern(
                module,
                pattern,
                L"CLauncherSettings::GetScaleFactorForPart",
                reinterpret_cast<void**>(&pfnGetScaleFactorForPart)))
        {
            return false;
        }
    }

    auto function = reinterpret_cast<BYTE*>(pfnGetScaleFactorForPart);
    for (size_t offset = 0; offset + 6 <= 32; ++offset)
    {
        if (function[offset] != 0x8B || function[offset + 1] != 0x05)
            continue;

        INT32 displacement = 0;
        memcpy(&displacement, function + offset + 2, sizeof(displacement));
        BYTE* forcedScaleFactorField =
            function + offset + 6 + displacement;
        constexpr size_t kForcedScaleFactorOffset = 0xC;
        auto settings = reinterpret_cast<LauncherSettings_t*>(
            forcedScaleFactorField - kForcedScaleFactorOffset);

        if (IsBadReadPtr(settings, sizeof(LauncherSettings_t)))
            return false;

        g_pLauncherSettings = settings;
        DS_Log(
            L"TWinUI pattern resolved g_launcherSettings=%p from %p",
            g_pLauncherSettings,
            pfnGetScaleFactorForPart);
        return true;
    }

    DS_Log(L"TWinUI failed to derive g_launcherSettings from scale-factor function");
    return false;
}

static BOOL InitializeTWinUIHooksByPattern(HMODULE hTwinUI)
{
    bool ok = true;

    ok &= ResolveTWinUIFunctionByPattern(
        hTwinUI,
        "48 89 5C 24 ? 57 48 83 EC 40 48 8B 05 ? ? ? ? "
        "48 33 C4 48 89 44 24 ? 8B FA",
        L"GetScaledIntFromResource",
        reinterpret_cast<void**>(&pfnGetScaledIntFromResource));

    ok &= ResolveLauncherSettingsByPattern(hTwinUI);

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 "
        "48 83 EC 40 FF 41",
        L"CImmersiveLauncher::ShowStartView",
        reinterpret_cast<void*>(Hook_CImmersiveLauncher_ShowStartView),
        reinterpret_cast<void**>(&pOriginalCImmersiveLauncher_ShowStartView));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "48 89 5C 24 ? 57 48 83 EC 20 33 D2 48 8B F9 "
        "E8 ? ? ? ? B9 5D 26 00 00",
        L"CSystemTileElement::AdjustNameBaselineLinespacing",
        reinterpret_cast<void*>(Hook_CSystemTileElement_AdjustNameBaselineLinespacing),
        reinterpret_cast<void**>(&pOriginalCSystemTileElement_AdjustNameBaselineLinespacing));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 30 "
        "48 8D 79 ? 45 33 C0",
        L"CImmersiveSystemTile::_Initialize",
        reinterpret_cast<void*>(Hook_CImmersiveSystemTile__Initialize),
        reinterpret_cast<void**>(&pOriginalCImmersiveSystemTile__Initialize));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "FF F5 53 57 41 56 41 57 48 8D 6C 24 D1 "
        "48 81 EC D0 00 00 00 83 79 6C 00 49 8B D9 "
        "4D 8B F0 8B FA 4C 8B F9 74 11 33 C0 "
        "48 81 C4 D0 00 00 00 41 5F 41 5E 5F 5B 5D C3",
        L"CLiveTileDataProvider::SubscribeToTiles",
        reinterpret_cast<void*>(Hook_TP_SubscribeToTiles),
        reinterpret_cast<void**>(&pOriginal_TP_SubscribeToTiles));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "48 8B C4 48 89 58 08 48 89 70 20 44 89 40 18 "
        "88 50 10 55 57 41 54 41 55 41 56 48 8B EC "
        "48 83 EC 40 4C 8B F1 B9 E0 01 00 00 49 8B D9 "
        "E8 ? ? ? ?",
        L"CHostElement::Create",
        reinterpret_cast<void*>(Hook_CHostElement_Create),
        reinterpret_cast<void**>(&pOriginalCHostElement_Create));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "FF F3 48 83 EC 20 8B D9 33 C0 48 8D 4C 24 38 "
        "33 D2 48 89 44 24 38 FF 15 ? ? ? ? 48 8D 4C 24 38 "
        "41 B9 01 00 00 00 45 33 C0 8B D3 FF 15 ? ? ? ?",
        L"CImmersiveColor::GetColor",
        reinterpret_cast<void*>(Hook_ImmersiveColor_GetColor),
        reinterpret_cast<void**>(&pOriginalImmersiveColor_GetColor));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "FF F5 53 56 57 41 56 48 8B EC 48 83 EC 40 "
        "49 83 20 00 33 DB 83 79 18 01 49 8B F0 44 8A F2 "
        "48 8B F9 0F 85 ? ? ? ? E9 ? ? ? ?",
        L"CPearlResourceManager::s_LoadOptionallyColorizedImage",
        reinterpret_cast<void*>(Hook_Pearl_LoadColorizedImage),
        reinterpret_cast<void**>(&pOriginalPearl_LoadColorizedImage));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "4C 8B C9 33 D2 4C 8D 41 7C 4C 8D 15 ? ? ? ? "
        "41 83 79 1C 00 75 0B 41 83 79 18 01 0F 84 ? ? ? ? "
        "49 63 41 1C 48 6B C8 07 48 03 CA 42 8A 84",
        L"CPearlResourceManager::_UpdateLayerVisibility",
        reinterpret_cast<void*>(Hook_Pearl_UpdateLayerVisibility),
        reinterpret_cast<void**>(&pOriginalPearl_UpdateLayerVisibility));

    ok &= InstallTWinUIHookByPattern(
        hTwinUI,
        "FF F5 53 56 57 41 54 41 56 41 57 48 8D 6C 24 ? "
        "48 81 EC B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 "
        "48 89 45 ? 41 8B 41",
        L"CAnimatedPearlContentVisual::_RepaintInternal",
        reinterpret_cast<void*>(Hook_Pearl_RepaintInternal),
        reinterpret_cast<void**>(&pOriginalPearl_RepaintInternal));

    if (!ok)
    {
        DS_Log(L"TWinUI signature fallback failed");
        return FALSE;
    }

    DS_Log(
        L"TWinUI signature fallback ready: ShowStartView=%p AdjustName=%p "
        L"SystemTileInit=%p GetScaledInt=%p GetScaleFactor=%p "
        L"SubscribeToTiles=%p HostElementCreate=%p GetColor=%p LoadColorized=%p "
        L"UpdateLayerVisibility=%p RepaintInternal=%p Settings=%p",
        pOriginalCImmersiveLauncher_ShowStartView,
        pOriginalCSystemTileElement_AdjustNameBaselineLinespacing,
        pOriginalCImmersiveSystemTile__Initialize,
        pfnGetScaledIntFromResource,
        pfnGetScaleFactorForPart,
        pOriginal_TP_SubscribeToTiles,
        pOriginalCHostElement_Create,
        pOriginalImmersiveColor_GetColor,
        pOriginalPearl_LoadColorizedImage,
        pOriginalPearl_UpdateLayerVisibility,
        pOriginalPearl_RepaintInternal,
        g_pLauncherSettings);
    return TRUE;
}

BOOL InitializeTWinUIHooks()
{
    DS_SYMBOL_HOOK rgTWinUIHooks[] =
    {
        { 
            {
                L"public: virtual long __cdecl CImmersiveLauncher::ShowStartView(enum IMMERSIVELAUNCHERSHOWMETHOD,enum IMMERSIVELAUNCHERSHOWFLAGS)"
            },
            &pOriginalCImmersiveLauncher_ShowStartView,
            Hook_CImmersiveLauncher_ShowStartView,
            false
        },
        {
            { L"public: void __cdecl CSystemTileElement::AdjustNameBaselineLinespacing(void)",
              L"private: void __cdecl CSystemTileElement::_AdjustNameBaselineLinespacing(void)" },
            &pOriginalCSystemTileElement_AdjustNameBaselineLinespacing,
            Hook_CSystemTileElement_AdjustNameBaselineLinespacing,
            false
        },
        {
            { L"private: long __cdecl CImmersiveSystemTile::_Initialize(void)" },
            &pOriginalCImmersiveSystemTile__Initialize,
            Hook_CImmersiveSystemTile__Initialize,
            false
        },
        {
            { L"int __cdecl GetScaledIntFromResource(unsigned int,enum DEVICE_SCALE_FACTOR)" },
            &pfnGetScaledIntFromResource,
            nullptr,
            false
        },
        {
            { L"public: enum DEVICE_SCALE_FACTOR __cdecl CLauncherSettings::GetScaleFactorForPart(enum ScalablePart)" },
            &pfnGetScaleFactorForPart,
            nullptr,
            false
        },
        {
            { L"public: static long __cdecl CHostElement::Create(class CImmersiveLauncherApps *,bool,unsigned int,struct IUnknown *,class CHostElement * *)" },
            &pOriginalCHostElement_Create,
            Hook_CHostElement_Create,
            false
        },
        {
            { L"class CLauncherSettings g_launcherSettings" },
            &g_pLauncherSettings,
            nullptr,
            false
        },
        {
            { L"public: virtual long __cdecl CStartMenuHost::CreateControl(struct HWND__ *,struct IListControlHost *,struct IViewSettings *)" },
            &pOriginalStartMenuHost_CreateControl, Hook_StartMenuHost_CreateControl, true
        },
        {
            { L"public: virtual long __cdecl CStartMenuHost::SetItemCollection(struct IItemCollection *)" },
            &pOriginalStartMenuHost_SetItemCollection, Hook_StartMenuHost_SetItemCollection, true
        },
        {
            { L"private: long __cdecl CStartMenuHost::_CreateMvcComponents(void)" },
            &pOriginalStartMenuHost_CreateMvc, Hook_StartMenuHost_CreateMvc, true
        },
        {
            { L"public: long __cdecl CTileItemDataSource::OnCollectionChanged(unsigned int,enum tagCOLLECTION_CHANGE_STATE)" },
            &pOriginal_TT_DS_OnCollectionChanged, Hook_TT_DS_OnCollectionChanged, true
        },
        {
            { L"public: long __cdecl CTileItemDataSource::OnItemsAdded(unsigned int,unsigned int,unsigned int,enum tagCHANGE_INDEX_TYPE)" },
            &pOriginal_TT_DS_OnItemsAdded, Hook_TT_DS_OnItemsAdded, true
        },
        {
            { L"public: virtual long __cdecl CTileItemDataSource::SetCollection(struct IItemCollection *)" },
            &pOriginal_TT_DS_SetCollection, Hook_TT_DS_SetCollection, true
        },
        {
            { L"public: virtual long __cdecl CTileCollection::OnCollectionChange(void)" },
            &pOriginal_TT_Coll_OnCollectionChange, Hook_TT_Coll_OnCollectionChange, true
        },
        {
            { L"public: virtual long __cdecl GridGrouping::CGroupShaper::OnItemsAdded(unsigned __int64,class CTileLayoutData const *)" },
            &pOriginal_TT_Shaper_OnItemsAdded, Hook_TT_Shaper_OnItemsAdded, true
        },
        {
            { L"public: virtual long __cdecl CTileModel::OnItemsAdded(unsigned __int64,class CTileLayoutData const *)" },
            &pOriginal_TT_Model_OnItemsAdded, Hook_TT_Model_OnItemsAdded, true
        },
        {
            { L"public: virtual long __cdecl CTileController::OnTilesAdded(unsigned __int64,class CTileLayoutData const *)" },
            &pOriginal_TT_Ctrl_OnTilesAdded, Hook_TT_Ctrl_OnTilesAdded, true
        },
        {
            { L"public: virtual long __cdecl CTileLayout::AddItem(class CTileLayoutData const &)" },
            &pOriginal_TT_Layout_AddItem, Hook_TT_Layout_AddItem, true
        },
        {
            { L"public: virtual long __cdecl CTileView::AddTile(class CTileLayoutData const &)" },
            &pOriginal_TT_View_AddTile, Hook_TT_View_AddTile, true
        },
        {
            { L"protected: long __cdecl CTileView::_Initialize(void)" },
            &pOriginal_TT_View_Initialize, Hook_TT_View_Initialize, true
        },
        {
            { L"public: virtual long __cdecl CTileView::SetSize(int,int,int,int)" },
            &pOriginal_TT_View_SetSize, Hook_TT_View_SetSize, true
        },
        {
            { L"public: virtual long __cdecl CTileView::UpdateLayout(enum GRID_DUI_LAYOUT_EVENT)" },
            &pOriginal_TT_View_UpdateLayout, Hook_TT_View_UpdateLayout, true
        },
        {
            { L"private: long __cdecl CTileView::_CreateNewTile(unsigned int,struct ITileRenderingDataBase *,class DirectUI::Element * *)" },
            &pOriginal_TT_View_CreateNewTile, Hook_TT_View_CreateNewTile, true
        },
        {
            { L"public: virtual long __cdecl CTileController::PerformRealization(void)" },
            &pOriginal_TT_Ctrl_PerformRealization, Hook_TT_Ctrl_PerformRealization, true
        },
        {
            { L"private: long __cdecl CTileController::_PerformRealization(enum REALIZATION_FLAGS)" },
            &pOriginal_TT_Ctrl_PerformRealizationPriv, Hook_TT_Ctrl_PerformRealizationPriv, true
        },
        {
            { L"private: long __cdecl CTileController::_RealizeTile(unsigned int)" },
            &pOriginal_TT_Ctrl_RealizeTile, Hook_TT_Ctrl_RealizeTile, true
        },
        {
            { L"public: virtual long __cdecl CTileUIFactory::CreateTileElement(struct ITileRenderingDataBase *,int,class DirectUI::Element * *)" },
            &pOriginal_TT_Factory_CreateTileElement, Hook_TT_Factory_CreateTileElement, true
        },
        {
            { L"private: long __cdecl CTileController::_NotifyModelAboutRealization(void)" },
            &pOriginal_TT_Ctrl_NotifyModelAboutRealization, Hook_TT_Ctrl_NotifyModelAboutRealization, true
        },
        {
            { L"private: long __cdecl CTileController::_BuildModelRealizationRequestsForVisibleTiles(class CSimpleHashTable<unsigned int,unsigned int,class CDefaultHashPolicy<unsigned int>,class CDefaultKeyCompare<unsigned int>,class CDefaultResizePolicy,class CDefaultRehashPolicy> *)" },
            &pOriginal_TT_Ctrl_BuildRealizationRequestsVisible, Hook_TT_Ctrl_BuildRealizationRequestsVisible, true
        },
        {
            { L"public: virtual long __cdecl CTileModel::SetRealizedTiles(class CSimpleHashTable<unsigned int,unsigned int,class CDefaultHashPolicy<unsigned int>,class CDefaultKeyCompare<unsigned int>,class CDefaultResizePolicy,class CDefaultRehashPolicy> const &)" },
            &pOriginal_TT_Model_SetRealizedTiles, Hook_TT_Model_SetRealizedTiles, true
        },
        {
            { L"private: long __cdecl CTileModel::_UpdateNewlyRealizedTiles(unsigned __int64,unsigned int *,class CSet<unsigned int> const &,class CSet<unsigned int> *)" },
            &pOriginal_TT_Model_UpdateNewlyRealizedTiles, Hook_TT_Model_UpdateNewlyRealizedTiles, true
        },
        {
            { L"public: virtual long __cdecl CTileController::OnNewlyRealizedTilesUpdated(unsigned __int64,class CTileLayoutData const *)" },
            &pOriginal_TT_Ctrl_OnNewlyRealizedTilesUpdated, Hook_TT_Ctrl_OnNewlyRealizedTilesUpdated, true
        },
        {
            { L"public: virtual long __cdecl CGridModelModify::OnNewlyRealizedTilesUpdated(unsigned __int64,class CTileLayoutData const *)" },
            &pOriginal_TT_Grid_OnNewlyRealizedTilesUpdated, Hook_TT_Grid_OnNewlyRealizedTilesUpdated, true
        },
        {
            { L"private: long __cdecl CTileController::_UpdateTile(unsigned int)" },
            &pOriginal_TT_Ctrl_UpdateTile, Hook_TT_Ctrl_UpdateTile, true
        },
        {
            { L"public: virtual long __cdecl CTileView::OnTilesRealized(enum GRID_REALIZED_SET_OPTIONS)" },
            &pOriginal_TT_View_OnTilesRealized, Hook_TT_View_OnTilesRealized, true
        },
        {
            { L"private: void __cdecl CTileController::_OnVisibleRegionRealized(bool)" },
            &pOriginal_TT_Ctrl_OnVisibleRegionRealized, Hook_TT_Ctrl_OnVisibleRegionRealized, true
        },
        {
            { L"private: long __cdecl CTileView::_InitializeNewTileElement(unsigned int,class DirectUI::Element *,bool)" },
            &pOriginal_TT_View_InitializeNewTileElement, Hook_TT_View_InitializeNewTileElement, true
        },
        {
            { L"CreateLiveTileDataProvider" },
            &pOriginal_TD_CreateLiveTileDataProvider, Hook_TD_CreateLiveTileDataProvider, true
        },
        {
            { L"public: __cdecl CLiveTileDataProvider::CLiveTileDataProvider(enum TILE_HOSTING_ENVIRONMENT,long *)" },
            &pOriginal_TD_LiveProvider_Ctor, Hook_TD_LiveProvider_Ctor, true
        },
        {
            { L"public: virtual long __cdecl CLiveTileDataProvider::UpdateTile(struct IShellItem *,enum TILE_LAYOUT_SIZE const *)" },
            &pOriginal_TD_LiveProvider_UpdateTile, Hook_TD_LiveProvider_UpdateTile, true
        },
        {
            { L"public: virtual long __cdecl CTileModel::OnTileDataAvailable(struct ITileRenderingDataBase *)" },
            &pOriginal_TD_Model_OnTileDataAvailable, Hook_TD_Model_OnTileDataAvailable, true
        },
        {
            { L"public: virtual long __cdecl CTileController::OnTileDataUpdated(struct ITileData *)" },
            &pOriginal_TD_Ctrl_OnTileDataUpdated, Hook_TD_Ctrl_OnTileDataUpdated, true
        },
        {
            { L"private: long __cdecl CTileController::_UpdateTileData(struct ITileData *)" },
            &pOriginal_TD_Ctrl_UpdateTileData, Hook_TD_Ctrl_UpdateTileData, true
        },
        {
            { L"public: virtual long __cdecl CTileItemDataSource::GetTileItem(unsigned int,struct IShellItem **,struct IItem **)" },
            &pOriginal_TD_DS_GetTileItem, Hook_TD_DS_GetTileItem, true
        },
        {
            { L"public: virtual long __cdecl CTileItem::GetTileRenderingData(struct ITileRenderingDataBase **)" },
            &pOriginal_TD_Item_GetTileRenderingData, Hook_TD_Item_GetTileRenderingData, true
        },
        {
            { L"public: virtual long __cdecl CTileItemDataSource::UpdateTileItems(class CSet<unsigned int> const &)" },
            &pOriginal_TD_DS_UpdateTileItems, Hook_TD_DS_UpdateTileItems, true
        },
        {
            { L"public: virtual long __cdecl CTileItemDataSource::GenerateUpdateNotifications(unsigned __int64,unsigned int *)" },
            &pOriginal_TD_DS_GenerateUpdateNotifications, Hook_TD_DS_GenerateUpdateNotifications, true
        },
        {
            { L"public: virtual long __cdecl CTileModel::RefreshTileData(unsigned __int64,unsigned int *)" },
            &pOriginal_TD_Model_RefreshTileData, Hook_TD_Model_RefreshTileData, true
        },
        {
            { L"public: virtual void __cdecl CTileController::FlushPendingTileData(void)" },
            &pOriginal_TD_Ctrl_FlushPendingTileData, Hook_TD_Ctrl_FlushPendingTileData, true
        },
        {
            { L"private: long __cdecl CTileModel::_GetTileItem(unsigned int,struct IShellItem **,struct IItem **)" },
            &pOriginal_TD_Model_GetTileItemPriv, Hook_TD_Model_GetTileItemPriv, true
        },
        {
            { L"public: virtual long __cdecl CLiveTileDataProvider::SubscribeToTiles(unsigned int,struct TILE_SUBSCRIPTION_REQUEST const *,struct ITileRenderingDataSink *,enum TILE_SUBSCRIPTION_OPTIONS)" },
            &pOriginal_TP_SubscribeToTiles, Hook_TP_SubscribeToTiles, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_LoadLogoAndDefaultAppSpaceIfCacheNotUsed(struct TILE_SUBSCRIPTION_REQUEST const *,struct ITileItem *)" },
            &pOriginal_TP_LoadLogoIfCacheNotUsed, Hook_TP_LoadLogoIfCacheNotUsed, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_PublishAllStartViewCachedData(unsigned int,struct TILE_PRELOAD_REQUEST const *,enum PUBLISH_FLAGS)" },
            &pOriginal_TP_PublishAllStartViewCached, Hook_TP_PublishAllStartViewCached, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_PublishLogoAndAppSpaceCachedData(unsigned int,struct TILE_PRELOAD_REQUEST const *)" },
            &pOriginal_TP_PublishLogoAndAppSpace, Hook_TP_PublishLogoAndAppSpace, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_PublishDefaultCachedData(unsigned int,struct TILE_PRELOAD_REQUEST const *)" },
            &pOriginal_TP_PublishDefaultCached, Hook_TP_PublishDefaultCached, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_StartCachePreLoadResult(unsigned int,struct TILE_PRELOAD_REQUEST *)" },
            &pOriginal_TP_StartCachePreLoadResult, Hook_TP_StartCachePreLoadResult, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProvider::_DetermineAvailableMainCachedData(unsigned short const *,struct ITileItem *,enum PUBLISH_FLAGS,enum PUBLISH_FLAGS *)" },
            &pOriginal_TP_DetermineAvailableMain, Hook_TP_DetermineAvailableMain, true
        },
        {
            { L"public: static long __cdecl CLiveTileDataProviderEventDispatcher::Create(class CTileModel *,class CLiveTileDataProviderEventDispatcher * *)" },
            &pOriginal_TP_Dispatcher_Create, Hook_TP_Dispatcher_Create, true
        },
        {
            { L"private: long __cdecl CLiveTileDataProviderEventDispatcher::_PostEvent(struct LIVE_TILE_DATA_PROVIDER_EVENT_DATA *)" },
            &pOriginal_TP_Dispatcher_PostEvent, Hook_TP_Dispatcher_PostEvent, true
        },
        {
            { L"private: static __int64 __cdecl CLiveTileDataProviderEventDispatcher::s_WndProc(struct HWND__*,unsigned int,unsigned __int64,__int64)" },
            &pOriginal_TP_Dispatcher_WndProc, Hook_TP_Dispatcher_WndProc, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnHostShowing(void)" },
            &pOriginal_StartMenuHost_OnHostShowing, Hook_StartMenuHost_OnHostShowing, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnHostShown(void)" },
            &pOriginal_StartMenuHost_OnHostShown, Hook_StartMenuHost_OnHostShown, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnHostHiding(void)" },
            &pOriginal_StartMenuHost_OnHostHiding, Hook_StartMenuHost_OnHostHiding, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnHostHidden(void)" },
            &pOriginal_StartMenuHost_OnHostHidden, Hook_StartMenuHost_OnHostHidden, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnVisibleRealized(void)" },
            &pOriginal_StartMenuHost_OnVisibleRealized, Hook_StartMenuHost_OnVisibleRealized, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::OnFullyRendered(void)" },
            &pOriginal_StartMenuHost_OnFullyRendered, Hook_StartMenuHost_OnFullyRendered, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuHost::ClearTileNotifications(void)" },
            &pOriginal_StartMenuHost_ClearTileNotifications, Hook_StartMenuHost_ClearTileNotifications, true
        },
        {
            { L"public: virtual long __cdecl CStartMenuHost::RefreshContents(void)" },
            &pOriginalStartMenuHost_RefreshContents, Hook_StartMenuHost_RefreshContents, true
        },
        {
            { L"public: static unsigned long __cdecl CImmersiveColor::GetColor(enum IMMERSIVE_COLOR_TYPE)" },
            &pOriginalImmersiveColor_GetColor, Hook_ImmersiveColor_GetColor, true
        },
        {
            { L"private: static long __cdecl CPearlResourceManager::s_LoadOptionallyColorizedImage(struct PEARL_LAYER_RENDER_DATA,bool,struct HBITMAP__ * *)" },
            &pOriginalPearl_LoadColorizedImage, Hook_Pearl_LoadColorizedImage, true
        },
        {
            { L"private: long __cdecl CAnimatedPearlContentVisual::_RepaintInternal(struct HDC__ *,enum PearlVisualRenderTarget,struct PEARL_LAYER_RENDER_DATA const &)" },
            &pOriginalPearl_RepaintInternal, Hook_Pearl_RepaintInternal, true
        },
        {
            { L"private: void __cdecl CPearlResourceManager::_UpdateLayerVisibility(void)" },
            &pOriginalPearl_UpdateLayerVisibility, Hook_Pearl_UpdateLayerVisibility, true
        }
    };
    HMODULE hTwinUI = LoadLibraryExW(L"TWinUI.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hTwinUI)
    {
        DS_Log(L"Failed to load TWinUI.dll");
        return FALSE;
    }
    const BOOL symbolsHooked = DS_HookSymbols(
        hTwinUI, rgTWinUIHooks, ARRAYSIZE(rgTWinUIHooks));
    if (!symbolsHooked)
    {
        DS_Log(L"TWinUI symbols unavailable; trying signature fallback");
        if (!InitializeTWinUIHooksByPattern(hTwinUI))
            return FALSE;
    }
    else if (!pOriginal_TP_SubscribeToTiles ||
             !pOriginalImmersiveColor_GetColor ||
             !pOriginalPearl_LoadColorizedImage ||
             !pOriginalPearl_UpdateLayerVisibility ||
             !pOriginalPearl_RepaintInternal)
    {
        DS_Log(L"TWinUI symbols incomplete; matching missing functions by signature");
        if (!InitializeTWinUIHooksByPattern(hTwinUI))
            return FALSE;
    }
    DS_Log(L"Pearl RT hooks (twinui): GetColor=%p LoadColorizedImage=%p RepaintInternal=%p UpdateLayerVisibility=%p",
           pOriginalImmersiveColor_GetColor, pOriginalPearl_LoadColorizedImage,
           pOriginalPearl_RepaintInternal, pOriginalPearl_UpdateLayerVisibility);
    return TRUE;
}
typedef void (__thiscall *Element_FireEvent_t)(void* pThis, void* pEvent, bool fFull, bool fBubble);
Element_FireEvent_t pOriginalElement_FireEvent = nullptr;

void __thiscall Hook_Element_FireEvent(void* pThis, void* pEvent, bool fFull, bool fBubble)
{
    if (!pThis)
    {
        static volatile LONG s_nullFireEventLogged = 0;
        if (InterlockedIncrement(&s_nullFireEventLogged) <= 10)
        {
            DS_Log(L"Element::FireEvent: blocked call on null element (event=%p).", pEvent);
        }
        return;
    }
    pOriginalElement_FireEvent(pThis, pEvent, fFull, fBubble);
}

BOOL InitializeDUI70Hooks()
{
    DS_SYMBOL_HOOK rgDuiHooks[] = {
        {
            { L"public: long __cdecl DirectUI::DUIXmlParser::CreateElement(unsigned short const *,class DirectUI::Element *,class DirectUI::Element *,unsigned long *,class DirectUI::Element * *)" },
            &pOriginalCDUIResourceManager_CreateElement,
            Hook_CDUIResourceManager_CreateElement,
            false
        },
        {
            { L"public: void __cdecl DirectUI::Element::FireEvent(struct DirectUI::Event *,bool,bool)" },
            &pOriginalElement_FireEvent,
            Hook_Element_FireEvent,
            true
        },
        {
            { L"public: long __cdecl DirectUI::Element::SetClass(unsigned short const *)" },
            &pfnElement_SetClass,
            nullptr,
            false
        },
        {
            { L"public: class DirectUI::DynamicArray<class DirectUI::Element *,0> * __cdecl DirectUI::Element::GetChildren(class DirectUI::Value * *)" },
            &pfnElement_GetChildren,
            nullptr,
            false
        },
        {
            { L"public: void __cdecl DirectUI::Value::Release(void)" },
            &pfnValue_Release,
            nullptr,
            false
        },
        {
            { L"public: long __cdecl DirectUI::RichText::SetBaseline(int)" },
            &pfnRichText_SetBaseline,
            nullptr,
            false
        },
        {
            { L"public: long __cdecl DirectUI::RichText::SetLineSpacing(int)" },
            &pfnRichText_SetLineSpacing,
            nullptr,
            false
        }
    };

    HMODULE hDui70 = LoadLibraryW(L"dui70.dll");
    if (!hDui70)
    {
        DS_Log(L"Initialization Warning: Could not find or access dui70.dll context registry.");
        return FALSE;
    }
    else if (!DS_HookSymbols(hDui70, rgDuiHooks, ARRAYSIZE(rgDuiHooks)))
    {
        DS_Log(L"Initialization Error: Target hooks mapping bindings failed within dui70.dll context frame.");
        return FALSE;
    }
    else
    {
        DS_Log(L"Initialization Success: Standard DirectUI mapping layout variables hooked and online.");
    }
    return TRUE;
}

// tile diagnostics
typedef HRESULT (__thiscall *GridPane_CreatePane_t)(void*, int, int, int, int, int, HWND);
GridPane_CreatePane_t pOriginalGridPane_CreatePane = nullptr;
HRESULT __thiscall Hook_GridPane_CreatePane(void* pThis, int a, int b, int c, int d, int e, HWND hwndParent)
{
    DS_Log(L"GridPane::CreatePane: begin this=%p args=(%d,%d,%d,%d,%d) parent=%p", pThis, a, b, c, d, e, hwndParent);
    g_gridPaneParentHwnd = hwndParent;

    if (IsStartMenuEnabled() && g_pLauncherSettings && hwndParent)
    {
        int scaleBefore = pfnGetScaleFactorForPart ? static_cast<int>(pfnGetScaleFactorForPart(g_pLauncherSettings, 0)) : -1;
        g_pLauncherSettings->_fNeedToReadForcedScaleFactorFromRegistry = true;
        g_pLauncherSettings->_forcedScaleFactorForAllParts = 0;
        g_pLauncherSettings->_hwndForScaleFactors = hwndParent;
        const bool rtl = (GetWindowLongW(hwndParent, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
        reinterpret_cast<unsigned char*>(g_pLauncherSettings)[1] = rtl ? 1 : 0;
        int scaleAfter = pfnGetScaleFactorForPart ? static_cast<int>(pfnGetScaleFactorForPart(g_pLauncherSettings, 0)) : -1;
        DS_Log(L"GridPane::CreatePane: restored RegisterHWNDForScaling/SetRTL hwnd=%p rtl=%d scale before=%d after=%d",
               hwndParent, rtl ? 1 : 0, scaleBefore, scaleAfter);
    }

    g_inGridPaneCreate = true;
    HRESULT hr = pOriginalGridPane_CreatePane(pThis, a, b, c, d, e, hwndParent);
    g_inGridPaneCreate = false;
    DS_Log(L"GridPane::CreatePane: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef HRESULT (__thiscall *GridPane_OnControlHostingReady_t)(void*, void*);
GridPane_OnControlHostingReady_t pOriginalGridPane_OnControlHostingReady = nullptr;
HRESULT __thiscall Hook_GridPane_OnControlHostingReady(void* pThis, void* pGridControl)
{
    DS_Log(L"GridPane::OnControlHostingReady: begin this=%p control=%p", pThis, pGridControl);
    HRESULT hr = pOriginalGridPane_OnControlHostingReady(pThis, pGridControl);
    DS_Log(L"GridPane::OnControlHostingReady: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef void (__thiscall *GridPane_OnControlUIReady_t)(void*);
GridPane_OnControlUIReady_t pOriginalGridPane_OnControlUIReady = nullptr;
void __thiscall Hook_GridPane_OnControlUIReady(void* pThis)
{
    DS_Log(L"GridPane::OnControlUIReady: this=%p", pThis);
    pOriginalGridPane_OnControlUIReady(pThis);
    DumpStartMenuWindowTree(L"OnControlUIReady");
}

typedef HRESULT (__cdecl *GridHostElement_Create_t)(HWND, bool, UINT, void*, void**);
GridHostElement_Create_t pOriginalGridHostElement_Create = nullptr;
HRESULT __cdecl Hook_GridHostElement_Create(HWND hwnd, bool f, UINT n, void* pUnk, void** ppOut)
{
    HRESULT hr = pOriginalGridHostElement_Create(hwnd, f, n, pUnk, ppOut);
    g_gridHostHwnd = hwnd;
    if (SUCCEEDED(hr) && ppOut && *ppOut)
        g_gridHostElement = *ppOut;
    DS_Log(L"CGridHostElement::Create: hwnd=%p hr=0x%08X out=%p",
           hwnd, static_cast<unsigned int>(hr), ppOut ? *ppOut : nullptr);
    return hr;
}

typedef HRESULT (__thiscall *HeaderPane_InitUserTile_t)(void*);
HeaderPane_InitUserTile_t pOriginalHeaderPane_InitUserTile = nullptr;
HRESULT __thiscall Hook_HeaderPane_InitUserTile(void* pThis)
{
    DS_Log(L"HeaderPane::_InitializeUserTile: begin this=%p", pThis);
    HRESULT hr = pOriginalHeaderPane_InitUserTile(pThis);
    DS_Log(L"HeaderPane::_InitializeUserTile: end hr=0x%08X", static_cast<unsigned int>(hr));
    return hr;
}

typedef bool (__fastcall *CPearl_IsBottomCornerPosition_t)(void* pThis);
CPearl_IsBottomCornerPosition_t pOriginalCPearl_IsBottomCornerPosition = nullptr;
bool __fastcall Hook_CPearl_IsBottomCornerPosition(void* pThis)
{
    if (IsStartMenuEnabled())
        return false;
    return pOriginalCPearl_IsBottomCornerPosition(pThis);
}

typedef long (__fastcall *CPearl_UpdateState_t)(void* pThis, int state, bool fAnimate);
CPearl_UpdateState_t pOriginalCPearl_UpdateState = nullptr;

typedef void (__fastcall *CPearl_OnStartMenuShownOnPearl_t)(void* pThis, bool fOpen, HWND* pphwnd);
CPearl_OnStartMenuShownOnPearl_t pOriginalCPearl_OnStartMenuShownOnPearl = nullptr;

static void* g_pearlMenuKeys[8] = {};
static volatile LONG g_pearlMenuOpen[8] = {};

static bool PearlGetMenuOpen(void* pPearl)
{
    for (int i = 0; i < 8; i++)
    {
        if (g_pearlMenuKeys[i] == pPearl)
            return g_pearlMenuOpen[i] != 0;
    }
    return false;
}

static void PearlSetMenuOpen(void* pPearl, bool fOpen)
{
    int freeSlot = -1;
    for (int i = 0; i < 8; i++)
    {
        if (g_pearlMenuKeys[i] == pPearl)
        {
            if (!fOpen && g_pearlMenuOpen[i])
                g_lastMenuCloseTick = GetTickCount64();
            g_pearlMenuOpen[i] = fOpen ? 1 : 0;
            return;
        }
        if (freeSlot < 0 && g_pearlMenuKeys[i] == nullptr)
            freeSlot = i;
    }
    if (freeSlot >= 0)
    {
        g_pearlMenuKeys[freeSlot] = pPearl;
        g_pearlMenuOpen[freeSlot] = fOpen ? 1 : 0;
    }
}

void __fastcall Hook_CPearl_OnStartMenuShownOnPearl(void* pThis, bool fOpen, HWND* pphwnd)
{
    PearlSetMenuOpen(pThis, fOpen);
    static LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 40)
        DS_Log(L"Pearl RT: OnStartMenuShownOnPearl(%d) pearl=%p", fOpen ? 1 : 0, pThis);
    pOriginalCPearl_OnStartMenuShownOnPearl(pThis, fOpen, pphwnd);
}

long __fastcall Hook_CPearl_UpdateState(void* pThis, int state, bool fAnimate)
{
    int effective = state;
    if (IsStartMenuEnabled())
    {
        const bool menuOpen = PearlGetMenuOpen(pThis);
        if (menuOpen && state != 2)
        {
            effective = 2;
        }
        else if (!menuOpen && state == 2)
        {

            effective = 1;
        }
    }
    static LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 80)
    {
        DS_Log(L"Pearl RT: _UpdateState(%d, anim=%d) -> %d (menuOpen=%d)",
               state, fAnimate ? 1 : 0, effective, PearlGetMenuOpen(pThis) ? 1 : 0);
    }
    return pOriginalCPearl_UpdateState(pThis, effective, fAnimate);
}



BOOL InitializeExplorerHooks()
{
    DS_SYMBOL_HOOK rgExplorerHooks[] =
    {
        { 
            { L"public: long __cdecl CPearl::Initialize(struct HWND__ *,struct IStartButtonSite *)" },
            &pOriginalCPearl_Initialize, Hook_CPearl_Initialize, false
        },
        {
            { L"public: void __cdecl CPearl::BuildStartMenu(void)" },
            &pOriginalCPearl_BuildStartMenu, Hook_CPearl_BuildStartMenu, false
        },
        { 
            { L"public: void __cdecl CPearl::DisplayStartMenu(void)" },
            &pOriginalCPearl_DisplayStartMenu, Hook_CPearl_DisplayStartMenu, false
        },
        { 
            { L"public: long __cdecl CPearl::ShowStartPage(void)" },
            &pOriginalCPearl_ShowStartPage, Hook_CPearl_ShowStartPage, false
        },
        { 
            { L"protected: virtual __int64 __cdecl CTray::v_WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64)" },
            &pOriginalCTray_v_WndProc, Hook_CTray_v_WndProc, false
        },
        { 
            { L"public: void __cdecl CPearl::CloseStartMenu(void)" },
            &pOriginalCPearl_CloseStartMenu, Hook_CPearl_CloseStartMenu, false
        },
        { 
            { L"public: void __cdecl CPearl::DestroyStartMenu(void)" },
            &pOriginalCPearl_DestroyStartMenu, Hook_CPearl_DestroyStartMenu, false
        },
        {
            { L"public: long __cdecl CPearl::UpdateTheme(void)" },
            &pOriginalCPearl_UpdateTheme, Hook_CPearl_UpdateTheme, false
        },
        {
            { L"private: bool __cdecl CPearl::_IsBottomCornerPosition(void)const " },
            &pOriginalCPearl_IsBottomCornerPosition, Hook_CPearl_IsBottomCornerPosition, true
        },
        {
            { L"private: long __cdecl CPearl::_UpdateState(enum PEARL_VISUAL_DISPLAY_STATE,bool)" },
            &pOriginalCPearl_UpdateState, Hook_CPearl_UpdateState, true
        },
        {
            { L"public: void __cdecl CPearl::OnStartMenuShownOnPearl(bool,struct HWND__ * *)" },
            &pOriginalCPearl_OnStartMenuShownOnPearl, Hook_CPearl_OnStartMenuShownOnPearl, true
        },
        {
            { L"public: bool __cdecl CPearl::IsStartMenuOpen(void)" },
            &pfnCPearl_IsStartMenuOpen, nullptr, true
        },
        { 
            { L"DesktopV2_Create" },
            &pOriginalDesktopV2_Create, Hook_DesktopV2_Create, false
        },
        { 
            { L"DesktopV2_Build" },
            &pOriginalDesktopV2_Build, Hook_DesktopV2_Build, false
        },
        { 
            { L"private: int __cdecl CDesktopHost::_AddWin32Controls(void)"},
            &pOriginalCDesktopHost__AddWin32Controls, Hook_CDesktopHost__AddWin32Controls, false
        },
        { 
            { L"private: struct HWND__ * __cdecl CDesktopHost::_Create(void)" },
            &pOriginalCDesktopHost_Create, Hook_CDesktopHost_Create, false },
        { 
            { L"private: long __cdecl CDesktopHost::_InitializeSearchPane(void)" },
            &pOriginalCDesktopHost_InitializeSearchPane, Hook_CDesktopHost_InitializeSearchPane, false
        },
        { 
            { L"private: long __cdecl CDesktopHost::_AcquireSearchServices(struct _GUID const &,void * *)" },
            &pOriginalCDesktopHost_AcquireSearchServices, Hook_CDesktopHost_AcquireSearchServices, false
        },
        {
            { L"public: virtual long __cdecl CStartMenuGridPane::CreatePane(int,int,int,int,int,struct HWND__ *)" },
            &pOriginalGridPane_CreatePane, Hook_GridPane_CreatePane, true
        },
        {
            { L"public: virtual long __cdecl CStartMenuGridPane::OnControlHostingReady(struct IStartMenuGridControl *)" },
            &pOriginalGridPane_OnControlHostingReady, Hook_GridPane_OnControlHostingReady, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuGridPane::OnControlUIReady(void)" },
            &pOriginalGridPane_OnControlUIReady, Hook_GridPane_OnControlUIReady, true
        },
        {
            { L"public: static long __cdecl CGridHostElement::Create(struct HWND__ *,bool,unsigned int,struct IUnknown *,class CGridHostElement * *)" },
            &pOriginalGridHostElement_Create, Hook_GridHostElement_Create, true
        },
        {
            { L"private: long __cdecl CStartMenuHeaderPane::_InitializeUserTile(void)" },
            &pOriginalHeaderPane_InitUserTile, Hook_HeaderPane_InitUserTile, true
        },
        {
            { L"public: virtual long __cdecl CStartMenuGridPane::GetControl(struct _GUID const &,void * *)" },
            &pOriginalGridPane_GetControl, Hook_GridPane_GetControl, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuGridPane::OnStartMenuVisibilityChanged(bool)" },
            &pOriginalGridPane_OnVisibilityChanged, Hook_GridPane_OnVisibilityChanged, true
        },
        {
            { L"public: virtual void __cdecl CStartMenuGridPane::OnStartMenuVisibilityChanging(bool)" },
            &pOriginalGridPane_OnVisibilityChanging, Hook_GridPane_OnVisibilityChanging, true
        }
    };
    HMODULE hExplorer = GetModuleHandleW(L"Explorer.exe");
    if (!hExplorer)
    {
        DS_Log(L"Failed to load Explorer.exe");
        return FALSE;
    }
    if (!DS_HookSymbols(hExplorer, rgExplorerHooks, ARRAYSIZE(rgExplorerHooks)))
    {
        DS_Log(L"Failed to hook one or more symbols in Explorer.exe");
    }
    DS_Log(L"Pearl RT hooks (explorer): IsBottomCornerPosition=%p UpdateState=%p OnStartMenuShownOnPearl=%p",
           pOriginalCPearl_IsBottomCornerPosition, pOriginalCPearl_UpdateState,
           pOriginalCPearl_OnStartMenuShownOnPearl);
    return TRUE;
}

static PVOID g_vehHandle = nullptr;
static volatile LONG g_vehCount = 0;

static void LogAddress(const wchar_t* tag, void* addr)
{
    HMODULE hMod = nullptr;
    if (addr &&
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr), &hMod) && hMod)
    {
        wchar_t path[MAX_PATH] = L"?";
        GetModuleFileNameW(hMod, path, ARRAYSIZE(path));
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        DS_Log(L"%s %p = %s+0x%llX", tag, addr, name,
               static_cast<unsigned long long>(
                   reinterpret_cast<char*>(addr) - reinterpret_cast<char*>(hMod)));
    }
    else
    {
        DS_Log(L"%s %p = (unknown module)", tag, addr);
    }
}

static LONG CALLBACK CrashLogVeh(PEXCEPTION_POINTERS ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != 0xC0000374 /* heap corruption */)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedIncrement(&g_vehCount) > 20)
        return EXCEPTION_CONTINUE_SEARCH;

    DS_SetLogEnabled(TRUE); // always capture crash details, even with log=0
    DS_Log(L"=== VEH: exception 0x%08X thread=%lu ===", code, GetCurrentThreadId());
    LogAddress(L"VEH: fault ip", ep->ExceptionRecord->ExceptionAddress);

    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    {
        DS_Log(L"VEH: AV type=%llu (0=read,1=write,8=DEP) target=%p",
               static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0]),
               reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

#ifdef _WIN64
    DS_Log(L"VEH: rax=%p rcx=%p rdx=%p r8=%p r9=%p rsp=%p",
           reinterpret_cast<void*>(ep->ContextRecord->Rax),
           reinterpret_cast<void*>(ep->ContextRecord->Rcx),
           reinterpret_cast<void*>(ep->ContextRecord->Rdx),
           reinterpret_cast<void*>(ep->ContextRecord->R8),
           reinterpret_cast<void*>(ep->ContextRecord->R9),
           reinterpret_cast<void*>(ep->ContextRecord->Rsp));
#endif

    void* frames[32] = {};
    USHORT n = RtlCaptureStackBackTrace(0, ARRAYSIZE(frames), frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        wchar_t tag[16];
        wsprintfW(tag, L"VEH: #%02u", static_cast<unsigned int>(i));
        LogAddress(tag, frames[i]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}


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

static BOOL InstallCoCreateInstanceHook()
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
    DS_Log(L"DirectStart start-menu hooks: init (winmm build)");

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
