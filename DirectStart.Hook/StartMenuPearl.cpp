#include "StartMenuHooks.h"

// crappy impl for pearl coloring on menu open

#define PEARL_RT_COLOR_TYPE_START 282

DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;
ULONGLONG g_lastMenuCloseTick = 0;
static thread_local bool g_inPearlImage = false;

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
    // Color 282 is used both for the accent-colored start-button background
    // and for the logo image. s_LoadOptionallyColorizedImage marks the logo
    // image load with g_inPearlImage, so the logo keeps its own white tint
    // while the background still receives the accent color here.
    if (type == PEARL_RT_COLOR_TYPE_START && IsStartMenuEnabled() &&
        InPearlGate() && !g_inPearlImage)
    {
        return GetRTStartMenuColor();
    }
    return pOriginalImmersiveColor_GetColor(type);
}

typedef HRESULT (__fastcall *Pearl_LoadColorizedImage_t)(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp);
Pearl_LoadColorizedImage_t pOriginalPearl_LoadColorizedImage = nullptr;

HRESULT __fastcall Hook_Pearl_LoadColorizedImage(
    PearlLayerRenderData* pData, bool fFlag, HBITMAP* phbmp)
{
    if (pData && IsStartMenuEnabled() && pData->f[6] == 1)
    {
        if (pData->f[8] == PEARL_RT_COLOR_TYPE_START)
        {
            // The 32x32 type-282 layer is the Windows logo tile. Keep its
            // original high byte (state flag/opacity) and force the glyph tint
            // to white; the accent square is supplied by GetColor(282) above.
            const DWORD before = static_cast<DWORD>(pData->f[7]);
            const DWORD after =
                (before & 0xFF000000u) | 0x00FFFFFFu;
            pData->f[7] = static_cast<int>(after);
            static LONG s_logged = 0;
            if (InterlockedIncrement(&s_logged) <= 8)
            {
                DS_Log(L"Pearl RT: logo layer %dx%d '%08X' -> '%08X' (white)",
                       pData->f[3], pData->f[4], before, after);
            }
        }
    }

    const bool pearlImage =
        pData && pData->f[8] == PEARL_RT_COLOR_TYPE_START;
    const bool previousPearlImage = g_inPearlImage;
    g_inPearlImage = pearlImage;

    PearlGateScope gate;
    const HRESULT hr =
        pOriginalPearl_LoadColorizedImage(pData, fFlag, phbmp);

    g_inPearlImage = previousPearlImage;
    return hr;
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



