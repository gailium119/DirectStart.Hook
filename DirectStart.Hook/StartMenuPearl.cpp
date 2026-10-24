#include "StartMenuHooks.h"

// crappy impl for pearl coloring on menu open

#define PEARL_RT_COLOR_TYPE_START 282

DWORD g_pearlGateTls = TLS_OUT_OF_INDEXES;
ULONGLONG g_lastMenuCloseTick = 0;

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
    // Do not override the immersive color globally. The same color type (282)
    // is also queried for the start-button logo, so replacing it here also
    // tinted the white Windows glyph. The pearl layer color is handled in
    // Hook_Pearl_LoadColorizedImage instead.
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

static bool PearlIsNeutralColor(DWORD color)
{
    const int red = static_cast<int>((color >> 16) & 0xFF);
    const int green = static_cast<int>((color >> 8) & 0xFF);
    const int blue = static_cast<int>(color & 0xFF);
    const int maximum = red > green ? (red > blue ? red : blue)
                                    : (green > blue ? green : blue);
    const int minimum = red < green ? (red < blue ? red : blue)
                                    : (green < blue ? green : blue);
    // Bright grayscale = the Windows logo / foreground glyph. Near-black is
    // left to the accent path (it is usually the pearl background).
    return maximum - minimum <= 4 && maximum >= 32;
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
            DWORD accent = 0;
            bool haveAccent = PearlLookupLayer3(pData, &accent);
            if (!haveAccent)
            {
                accent = GetRTStartMenuColor();
                haveAccent = accent != 0;
            }

            const DWORD before = static_cast<DWORD>(pData->f[7]);
            if (haveAccent && PearlIsNeutralColor(before))
            {
                // Neutral layers are the Windows logo / foreground glyph, not
                // the accent-colored background. Leave them alone so the logo
                // keeps its white/gray color instead of being tinted.
                static LONG s_neutralLogged = 0;
                if (InterlockedIncrement(&s_neutralLogged) <= 8)
                {
                    DS_Log(L"Pearl RT: layer5 %dx%d neutral 0x%08X preserved",
                           pData->f[3], pData->f[4], before);
                }
            }
            else if (haveAccent)
            {
                // Hover and pressed are separate pearl layers (type 282), but
                // they must render with the same start-button background
                // color. Use the normal layer's color for both.
                const DWORD after = accent;
                pData->f[7] = static_cast<int>(after);
                static LONG s_logged = 0;
                if (InterlockedIncrement(&s_logged) <= 8)
                {
                    DS_Log(L"Pearl RT: layer5 %dx%d unified 0x%08X -> 0x%08X (accent 0x%08X)",
                           pData->f[3], pData->f[4], before, after, accent);
                }
            }
            else
            {
                DS_Log(L"Pearl RT: layer5 %dx%d - no accent color, unchanged",
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



