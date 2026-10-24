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
#define PEARL_DEFAULT_COLOR 0xFF4B0082u  // dark purple / indigo
#define PEARL_LOGO_RES_ID 11703

DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;
ULONGLONG g_lastMenuCloseTick = 0;
static thread_local bool g_inLogoColorize = false;

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

    // Use a fixed dark-purple default instead of the DWM/immersive color.
    // The HKLM OverrideColor value above still takes precedence for custom
    // colors.
    return PEARL_DEFAULT_COLOR;
}

DWORD __fastcall Hook_ImmersiveColor_GetColor(int type)
{
    if (g_inLogoColorize)
    {
        // The logo bitmap is the only visible part of the button now; color it
        // with the fixed dark purple instead of the per-state theme colors.
        return PEARL_DEFAULT_COLOR;
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

    const bool logo = pData && IsStartMenuEnabled() &&
        pData->renderMode == 1 && pData->resId == PEARL_LOGO_RES_ID;
    const bool previous = g_inLogoColorize;
    g_inLogoColorize = logo;
    const HRESULT hr =
        pOriginalPearl_LoadColorizedImage(pData, fFlag, phbmp);
    g_inLogoColorize = previous;
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

    const bool logo = layer && IsStartMenuEnabled() &&
        layer->renderMode == 1 && layer->resId == PEARL_LOGO_RES_ID;
    const bool previous = g_inLogoColorize;
    g_inLogoColorize = logo;
    const HRESULT hr =
        pOriginalPearl_RepaintInternal(pThis, hdc, target, pData);
    g_inLogoColorize = previous;
    return hr;
}

typedef void (__fastcall *Pearl_UpdateLayerVisibility_t)(void* pThis);
Pearl_UpdateLayerVisibility_t pOriginalPearl_UpdateLayerVisibility = nullptr;

void __fastcall Hook_Pearl_UpdateLayerVisibility(void* pThis)
{
    pOriginalPearl_UpdateLayerVisibility(pThis);

    if (pThis && IsStartMenuEnabled())
    {
        // The desired start button is just the purple Windows glyph on the
        // taskbar. Hide the baseplate/colorization/wash square layers and keep
        // the rest/hover/press logo layers.
        BYTE* layers = reinterpret_cast<BYTE*>(
            reinterpret_cast<char*>(pThis) + 0x7C);
        layers[0x00] = 0; // PVL_BASEPLATE
        layers[0x24] = 0; // PVL_COLORIZATION
        layers[0x48] = 0; // PVL_WASH
        static LONG s_forced = 0;
        if (InterlockedIncrement(&s_forced) <= 8)
        {
            DS_Log(L"Pearl RT: vis forced logo-only");
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



