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

DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;
ULONGLONG g_lastMenuCloseTick = 0;

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
    return pOriginalImmersiveColor_GetColor
        ? pOriginalImmersiveColor_GetColor(0)
        : 0xFF000000;
}

DWORD __fastcall Hook_ImmersiveColor_GetColor(int type)
{
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

    if (pData && IsStartMenuEnabled() && pData->renderMode == 1 &&
        pData->clrDesired == PEARL_RT_COLOR_TYPE_START && pData->fColorize)
    {
        // Bitmap layers are the logo/glyph art, not the accent background.
        // Colorizing them recolors the transparent pixels as well. Keep the
        // original resource id and alpha request, and only disable colorize so
        // the white logo and its transparency are rendered unchanged.
        pData->fColorize = 0;
        static LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 8)
        {
            DS_Log(L"Pearl RT: bitmap resId=%u needAlpha=%u colorize=0 clr=%d",
                   pData->resId,
                   pData->fNeedAlpha,
                   pData->clrDesired);
        }
    }
    return pOriginalPearl_LoadColorizedImage(pData, fFlag, phbmp);
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

    if (layer && IsStartMenuEnabled() && layer->renderMode == 1 &&
        layer->clrDesired == PEARL_RT_COLOR_TYPE_START && layer->fColorize)
    {
        layer->fColorize = 0;
    }

    return pOriginalPearl_RepaintInternal(pThis, hdc, target, pData);
}

typedef void (__fastcall *Pearl_UpdateLayerVisibility_t)(void* pThis);
Pearl_UpdateLayerVisibility_t pOriginalPearl_UpdateLayerVisibility = nullptr;

void __fastcall Hook_Pearl_UpdateLayerVisibility(void* pThis)
{
    pOriginalPearl_UpdateLayerVisibility(pThis);

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



