#include "StartMenuHooks.h"
#include "PatternScan.h"

typedef HRESULT (__thiscall *Element_SetClass_t)(void* pThis, LPCWSTR pszClass);
Element_SetClass_t pfnElement_SetClass = nullptr;

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

void DumpDuiTree(void* pRoot)
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

volatile LONG g_systemTileStyled = 0;

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
    if (SUCCEEDED(hrClass))
    {
        InterlockedExchange(&g_systemTileStyled, 1);
    }
    return hr;
}

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
    int cchResult = LoadStringW(g_hTwinUiMui, uResId, szValue, _countof(szValue));
    if (SUCCEEDED(ResultFromWin32Count(cchResult, _countof(szValue))))
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
        int len = g_hTwinUiMui ? LoadStringW(g_hTwinUiMui, id, text, _countof(text)) : -1;
        DS_Log(L"twinui string %u: len=%d text=\"%s\"", id, len, text);
    }
    for (UINT id = 12000; id <= 12030; ++id)
    {
        WCHAR text[160] = {};
        int len = g_hTwinUiMui ? LoadStringW(g_hTwinUiMui, id, text, _countof(text)) : -1;
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

static BYTE* FindUniqueCodePattern(
    HMODULE module, const char* pattern, LPCWSTR name)
{
    SIZE_T matchCount = 0;
    void* match = VnPatternFindUnique(module, pattern, &matchCount);
    if (matchCount == 0)
    {
        DS_Log(L"TWinUI pattern not found: %s", name);
    }
    else if (matchCount > 1)
    {
        DS_Log(
            L"TWinUI pattern is not unique (%llu matches): %s",
            static_cast<unsigned long long>(matchCount),
            name);
    }
    return static_cast<BYTE*>(match);
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


// CStartMenuHost hooks live here because the TWinUI symbol table uses them.
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
        hTwinUI, rgTWinUIHooks, _countof(rgTWinUIHooks));
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
    DS_Log(L"Pearl RT build: 2026-10-24 rest-white-v8");
    DS_Log(L"Pearl RT hooks (twinui): GetColor=%p LoadColorizedImage=%p RepaintInternal=%p UpdateLayerVisibility=%p",
           pOriginalImmersiveColor_GetColor, pOriginalPearl_LoadColorizedImage,
           pOriginalPearl_RepaintInternal, pOriginalPearl_UpdateLayerVisibility);
    return TRUE;
}
