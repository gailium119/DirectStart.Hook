/*
 * StartMenuHooks.h - shared declarations for the split start-menu hook code.
 *
 * The hook implementation is organised in ExplorerPatcher style:
 *   StartMenu.cpp            init, MUI/theme, CoCreateInstance
 *   StartMenuTwinUI.cpp      twinui hooks, tile styling, twinui symbols
 *   StartMenuExplorer.cpp    explorer/tray/pearl host hooks, explorer symbols
 *   StartMenuPearl.cpp       pearl color/animation hooks
 *   StartMenuDui.cpp         dui70 hooks and symbols
 *   StartMenuDiagnostics.cpp window/Apps dumps and crash logging
 */

#pragma once

#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <dwmapi.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "DirectStart.h"

struct LauncherSettings_t
{
    char pad[2];
    bool _fNeedToReadForcedScaleFactorFromRegistry;
    char pad2[9];
    int32_t _forcedScaleFactorForAllParts;
    char pad3[8];
    HWND _hwndForScaleFactors;
};

struct PearlLayerRenderData
{
    int f[9];
};
static_assert(sizeof(PearlLayerRenderData) == 36, "PEARL_LAYER_RENDER_DATA size");

struct DuiElementArray
{
    UINT sizeAndFlags;
#ifdef _WIN64
    UINT alignmentPadding;
#endif
    void* bufferOrFirstItem;
};

typedef HRESULT (__thiscall *Element_SetClass_t)(void* pThis, LPCWSTR pszClass);
typedef DuiElementArray* (__thiscall *Element_GetChildren_t)(void* pThis, void** ppValue);
typedef void (__thiscall *Value_Release_t)(void* pThis);
typedef HRESULT (__thiscall *CDUIResourceManager_CreateElement_t)(
    void* pThis, LPCWSTR pszResID, void* pParent, void* pUnknown,
    DWORD* pdwDeferCookie, void** ppElementOut);
typedef HRESULT (__cdecl *RichText_SetBaseline_t)(void* pThis, int baseline);
typedef HRESULT (__cdecl *RichText_SetLineSpacing_t)(void* pThis, int spacing);
typedef DEVICE_SCALE_FACTOR (__cdecl *CLauncherSettings_GetScaleFactorForPart_t)(
    void* pThis, int part);

typedef DWORD (__fastcall *ImmersiveColor_GetColor_t)(int type);
typedef HRESULT (__fastcall *Pearl_LoadColorizedImage_t)(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp);
typedef HRESULT (__fastcall *Pearl_RepaintInternal_t)(
    void* pThis, HDC hdc, int target, const void* pData);
typedef void (__fastcall *Pearl_UpdateLayerVisibility_t)(void* pThis);
typedef long (__fastcall *CPearl_UpdateState_t)(
    void* pThis, int state, bool fAnimate);
typedef void (__fastcall *CPearl_OnStartMenuShownOnPearl_t)(
    void* pThis, bool fOpen, HWND* pphwnd);

extern HINSTANCE g_hExplorerMui;
extern HINSTANCE g_hTwinUiMui;

extern LauncherSettings_t* g_pLauncherSettings;
extern CLauncherSettings_GetScaleFactorForPart_t pfnGetScaleFactorForPart;

extern HWND g_gridPaneParentHwnd;
extern HWND g_gridHostHwnd;
extern void* g_gridHostElement;
extern bool g_inGridPaneCreate;
extern IUnknown* g_gridBrowserUnk;

extern ULONGLONG g_lastMenuCloseTick;
extern DWORD g_pearlGateTls;
extern volatile LONG g_systemTileStyled;

extern Element_SetClass_t pfnElement_SetClass;
extern Element_GetChildren_t pfnElement_GetChildren;
extern Value_Release_t pfnValue_Release;
extern RichText_SetBaseline_t pfnRichText_SetBaseline;
extern RichText_SetLineSpacing_t pfnRichText_SetLineSpacing;
extern CDUIResourceManager_CreateElement_t pOriginalCDUIResourceManager_CreateElement;

extern ImmersiveColor_GetColor_t pOriginalImmersiveColor_GetColor;
extern Pearl_LoadColorizedImage_t pOriginalPearl_LoadColorizedImage;
extern Pearl_RepaintInternal_t pOriginalPearl_RepaintInternal;
extern Pearl_UpdateLayerVisibility_t pOriginalPearl_UpdateLayerVisibility;
extern CPearl_UpdateState_t pOriginalCPearl_UpdateState;
extern CPearl_OnStartMenuShownOnPearl_t pOriginalCPearl_OnStartMenuShownOnPearl;

inline BOOL IsStartMenuEnabled()
{
    return DS_IsEnabled();
}

extern "C" const IID IID_IStartMenuPopup;

void DumpStartMenuWindowTree(const wchar_t* reason);
void DumpDuiTree(void* pRoot);

bool PearlGetMenuOpen(void* pPearl);
void PearlSetMenuOpen(void* pPearl, bool fOpen);

BOOL InitializeTWinUIHooks();
BOOL InitializeExplorerHooks();
BOOL InitializeDUI70Hooks();

LONG CALLBACK CrashLogVeh(PEXCEPTION_POINTERS ep);

HRESULT WINAPI Hook_CoCreateInstance(
    REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
    REFIID riid, LPVOID* ppv);
BOOL InstallCoCreateInstanceHook();

HRESULT __fastcall Hook_CImmersiveLauncher_ShowStartView(
    void* pThis, __int32 mMethod, __int32 mFlags);
void __fastcall Hook_CSystemTileElement_AdjustNameBaselineLinespacing(void* pThis);
HRESULT __fastcall Hook_CImmersiveSystemTile__Initialize(void* pThis);
HRESULT Hook_CHostElement_Create(
    void* pLauncherApps, bool punkSite, unsigned int ppeHost,
    IUnknown* a4, void** fDblBuffer);
HRESULT __thiscall Hook_TP_SubscribeToTiles(
    void* pThis, UINT cTiles, const void* requests, void* sink, int options);

HRESULT __thiscall Hook_CDUIResourceManager_CreateElement(
    void* pThis, LPCWSTR pszResID, void* pParent, void* pUnknown,
    DWORD* pdwDeferCookie, void** ppElementOut);
void __thiscall Hook_Element_FireEvent(
    void* pThis, void* pEvent, bool fFull, bool fBubble);

DWORD __fastcall Hook_ImmersiveColor_GetColor(int type);
HRESULT __fastcall Hook_Pearl_LoadColorizedImage(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp);
HRESULT __fastcall Hook_Pearl_RepaintInternal(
    void* pThis, HDC hdc, int target, const void* pData);
void __fastcall Hook_Pearl_UpdateLayerVisibility(void* pThis);
void __fastcall Hook_CPearl_OnStartMenuShownOnPearl(
    void* pThis, bool fOpen, HWND* pphwnd);
long __fastcall Hook_CPearl_UpdateState(
    void* pThis, int state, bool fAnimate);
