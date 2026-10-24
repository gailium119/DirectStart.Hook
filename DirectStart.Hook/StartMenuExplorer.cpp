#include "StartMenuHooks.h"
#include <commctrl.h>

bool g_inGridPaneCreate = false;

void InstallStartMenuWheelForwarding();

typedef void (__fastcall *CPearl_DisplayStartMenu_t)(void* pThis);
CPearl_DisplayStartMenu_t pOriginalCPearl_DisplayStartMenu = nullptr;

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

    InstallStartMenuWheelForwarding();
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

static HWND FindDescendantWindow(HWND parent, const wchar_t* wantedClass)
{
    HWND child = FindWindowExW(parent, nullptr, nullptr, nullptr);
    while (child)
    {
        wchar_t className[64] = {};
        if (GetClassNameW(child, className, _countof(className)) &&
            wcscmp(className, wantedClass) == 0)
        {
            return child;
        }
        HWND found = FindDescendantWindow(child, wantedClass);
        if (found)
        {
            return found;
        }
        child = FindWindowExW(parent, child, nullptr, nullptr);
    }
    return nullptr;
}

static LRESULT CALLBACK MenuWheelSubclass(
    HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR)
{
    if (uMsg == WM_MOUSEWHEEL && IsStartMenuEnabled())
    {
        HWND host = FindWindowW(L"DV2ControlHost", nullptr);
        HWND list = host
            ? FindDescendantWindow(host, L"SysListView32")
            : nullptr;
        if (list)
        {
            const int delta =
                static_cast<short>(HIWORD(wParam));
            if (delta != 0)
            {
                const int lines =
                    (abs(delta) / WHEEL_DELTA) * 3;
                const WPARAM scroll =
                    MAKEWPARAM(delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
                for (int i = 0; i < lines; ++i)
                {
                    SendMessageW(list, WM_VSCROLL, scroll, 0);
                }
            }
            return 0;
        }

        if (hwnd != list)
        {
            const POINT point = {
                static_cast<SHORT>(LOWORD(lParam)),
                static_cast<SHORT>(HIWORD(lParam))};
            HWND target = WindowFromPoint(point);
            if (target && target != hwnd &&
                host && IsChild(host, target))
            {
                return SendMessageW(target, uMsg, wParam, lParam);
            }
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

static BOOL CALLBACK SubclassWheelWindow(HWND hwnd, LPARAM)
{
    SetWindowSubclass(hwnd, MenuWheelSubclass, 0xD15, 0);
    return TRUE;
}

void InstallStartMenuWheelForwarding()
{
    HWND host = FindWindowW(L"DV2ControlHost", nullptr);
    if (!host)
    {
        return;
    }

    SetWindowSubclass(host, MenuWheelSubclass, 0xD15, 0);
    EnumChildWindows(host, SubclassWheelWindow, 0);
    static LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 8)
    {
        DS_Log(L"Start menu wheel forwarding installed");
    }
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
void __fastcall Hook_CPearl_DisplayStartMenu(void* pThis)
{
    DS_Log(L"CPearl::DisplayStartMenu: this=%p popup=%p",
           pThis,
           pThis ? *reinterpret_cast<void**>((char*)pThis + 0xA0) : nullptr);
    pOriginalCPearl_DisplayStartMenu(pThis);
    InstallStartMenuWheelForwarding();
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

typedef bool (__fastcall *CPearl_IsStartMenuOpen_t)(void* pThis);
CPearl_IsStartMenuOpen_t pfnCPearl_IsStartMenuOpen = nullptr;

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
    InstallStartMenuWheelForwarding();
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
    if (!DS_HookSymbols(hExplorer, rgExplorerHooks, _countof(rgExplorerHooks)))
    {
        DS_Log(L"Failed to hook one or more symbols in Explorer.exe");
    }
    DS_Log(L"Pearl RT hooks (explorer): IsBottomCornerPosition=%p UpdateState=%p OnStartMenuShownOnPearl=%p",
           pOriginalCPearl_IsBottomCornerPosition, pOriginalCPearl_UpdateState,
           pOriginalCPearl_OnStartMenuShownOnPearl);

    return TRUE;
}
