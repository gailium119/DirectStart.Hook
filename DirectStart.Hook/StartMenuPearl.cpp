#include "StartMenuHooks.h"
#include <intrin.h>

/*
 * The colorized-image/layer-visibility hacks are left as pass-through: they
 * filled transparent pixels and hid a pearl layer, which tinted the Windows
 * logo and produced a visible inner square.
 *
 * The start-button background color is still supplied through
 * CImmersiveColor::GetColor(282); that is what the OS uses for the accent
 * square, and without it the square falls back to the theme's gray.
 */

#define PEARL_RT_COLOR_TYPE_START 282
// Both paths consume the value as COLORREF (0x00BBGGRR). The previous
// repaint value 0xFF0000FF was read as BGR and rendered red; use the same
// BGR blue for both. #0000FF -> R=00, G=00, B=FF.
#define PEARL_COLOR_BGR  0x00FF0000u
#define PEARL_COLOR_ARGB 0x00FF0000u
#define PEARL_LOGO_RES_ID 11703

DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;
ULONGLONG g_lastMenuCloseTick = 0;
static thread_local bool g_inPearlLoad = false;

static_assert(sizeof(PearlLayerRenderData) == 36, "PEARL_LAYER_RENDER_DATA size");

typedef DWORD (__fastcall *ImmersiveColor_GetColor_t)(int type);
ImmersiveColor_GetColor_t pOriginalImmersiveColor_GetColor = nullptr;

static DWORD GetRTStartMenuColor()
{
    DWORD value = 0;
    DWORD type = 0;
    DWORD cb = sizeof(value);
    if (SHGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartMenu\\Colors",
            L"OverrideColor",
            &type,
            &value,
            &cb) == ERROR_SUCCESS &&
        type == REG_DWORD &&
        cb == sizeof(value))
    {
        return value;
    }

    // ARGB for the repaint/fill path. The loader path below converts to BGR.
    return PEARL_COLOR_ARGB;
}

DWORD __fastcall Hook_ImmersiveColor_GetColor(int type)
{
    if (g_inPearlLoad &&
        (type == 279 || type == 280 || type == PEARL_RT_COLOR_TYPE_START))
    {
        return PEARL_COLOR_BGR;
    }

    DWORD result = pOriginalImmersiveColor_GetColor(type);
    if (type == PEARL_RT_COLOR_TYPE_START && IsStartMenuEnabled())
    {
        result = GetRTStartMenuColor();
        static LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 16)
        {
            DS_Log(L"Pearl RT: GetColor(282) -> 0x%08X caller=%p",
                   result, _ReturnAddress());
        }
    }
    return result;
}

typedef HRESULT (__fastcall *Pearl_LoadColorizedImage_t)(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp);
Pearl_LoadColorizedImage_t pOriginalPearl_LoadColorizedImage = nullptr;

HRESULT __fastcall Hook_Pearl_LoadColorizedImage(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp)
{
    static LONG s_entry = 0;
    if (InterlockedIncrement(&s_entry) <= 32)
    {
        if (pData)
        {
            DS_Log(L"Pearl RT: LoadColorized mode=%d res=%u needA=%u colorize=%u clr=%d size=%dx%d",
                   pData->renderMode,
                   pData->resId,
                   pData->fNeedAlpha,
                   pData->fColorize,
                   pData->clrDesired,
                   pData->layout.rc.right - pData->layout.rc.left,
                   pData->layout.rc.bottom - pData->layout.rc.top);
        }
        else
        {
            DS_Log(L"Pearl RT: LoadColorized pData=null");
        }
    }

    const bool previous = g_inPearlLoad;
    g_inPearlLoad = IsStartMenuEnabled() != 0;
    const HRESULT hr =
        pOriginalPearl_LoadColorizedImage(pData, fFlag, phbmp);
    g_inPearlLoad = previous;
    return hr;
}

typedef HRESULT (__fastcall *Pearl_RepaintInternal_t)(
    void* pThis, HDC hdc, int target, const void* pData);
Pearl_RepaintInternal_t pOriginalPearl_RepaintInternal = nullptr;

HRESULT __fastcall Hook_Pearl_RepaintInternal(
    void* pThis, HDC hdc, int target, const void* pData)
{
    auto* layer = const_cast<PearlLayerRenderData*>(
        reinterpret_cast<const PearlLayerRenderData*>(pData));
    static LONG s_logged = 0;
    if (layer && InterlockedIncrement(&s_logged) <= 16)
    {
        DS_Log(L"Pearl RT: repaint mode=%d res=%u needA=%u colorize=%u clr=%d",
               layer->renderMode,
               layer->resId,
               layer->fNeedAlpha,
               layer->fColorize,
               layer->clrDesired);
    }

    return pOriginalPearl_RepaintInternal(pThis, hdc, target, pData);
}

typedef void (__fastcall *Pearl_UpdateLayerVisibility_t)(void* pThis);
Pearl_UpdateLayerVisibility_t pOriginalPearl_UpdateLayerVisibility = nullptr;

void __fastcall Hook_Pearl_UpdateLayerVisibility(void* pThis)
{
    pOriginalPearl_UpdateLayerVisibility(pThis);

    if (pThis && IsStartMenuEnabled())
    {
        const int state = *reinterpret_cast<int*>(
            reinterpret_cast<char*>(pThis) + 0x1C);
        BYTE* layers = reinterpret_cast<BYTE*>(
            reinterpret_cast<char*>(pThis) + 0x7C);
        if (state == 1)
        {
            // Hover should be the blue glyph only. The baseplate/colorization
            // layers were drawing the blue square around the transparent logo
            // center, and HOVERSELECTED added the extra frame.
            layers[0x00] = 0; // PVL_BASEPLATE
            layers[0x24] = 0; // PVL_COLORIZATION
            layers[0x48] = 0; // PVL_WASH
            layers[0x90] = 0; // PVL_HOVERSELECTED
            layers[0xB4] = 1; // PVL_PRESS
            static LONG s_hoverFixed = 0;
            if (InterlockedIncrement(&s_hoverFixed) <= 8)
            {
                DS_Log(L"Pearl RT: vis hover logo-only");
            }
        }
    }

    static LONG s_logged = 0;
    if (pThis && InterlockedIncrement(&s_logged) <= 16)
    {
        const int state = *reinterpret_cast<int*>(
            reinterpret_cast<char*>(pThis) + 0x1C);
        const BYTE* layers = reinterpret_cast<BYTE*>(
            reinterpret_cast<char*>(pThis) + 0x7C);
        DS_Log(L"Pearl RT: vis state=%d L0=%d L1=%d L2=%d L3=%d L4=%d L5=%d",
               state,
               layers[0x00],
               layers[0x24],
               layers[0x48],
               layers[0x6C],
               layers[0x90],
               layers[0xB4]);
    }
}

// Servicing builds may ship without usable PDBs. These patterns keep opcodes and
// stable immediates while wildcarding branch and RIP-relative displacements.
typedef long (__fastcall *CPearl_UpdateState_t)(void* pThis, int state, bool fAnimate);
CPearl_UpdateState_t pOriginalCPearl_UpdateState = nullptr;

typedef void (__fastcall *CPearl_OnStartMenuShownOnPearl_t)(void* pThis, bool fOpen, HWND* pphwnd);
CPearl_OnStartMenuShownOnPearl_t pOriginalCPearl_OnStartMenuShownOnPearl = nullptr;

static void* g_pearlMenuKeys[8] = {};
static volatile LONG g_pearlMenuOpen[8] = {};

bool PearlGetMenuOpen(void* pPearl)
{
    for (int i = 0; i < 8; i++)
    {
        if (g_pearlMenuKeys[i] == pPearl)
            return g_pearlMenuOpen[i] != 0;
    }
    return false;
}

void PearlSetMenuOpen(void* pPearl, bool fOpen)
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

    // The pearl bitmaps are often built before d3d11 is loaded, so the
    // s_LoadOptionallyColorizedImage hook never sees them. Refresh the theme
    // once after all hooks are live; this rebuilds the pearl resources and
    // lets the logo-resource fColorize fix apply.
    static LONG s_themeRefreshed = 0;
    if (pThis && IsStartMenuEnabled() &&
        InterlockedExchange(&s_themeRefreshed, 1) == 0 &&
        pOriginalCPearl_UpdateTheme)
    {
        DS_Log(L"Pearl RT: refreshing pearl theme for logo colorize fix");
        pOriginalCPearl_UpdateTheme(pThis);
    }

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



