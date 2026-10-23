#include "StartMenuHooks.h"

HWND g_gridPaneParentHwnd = nullptr;
HWND g_gridHostHwnd = nullptr;
void* g_gridHostElement = nullptr;

IUnknown* g_gridBrowserUnk = nullptr;
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
    GetClassNameW(hwnd, cls, _countof(cls));
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

void DumpStartMenuWindowTree(const wchar_t* reason)
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
        GetModuleFileNameW(hMod, path, _countof(path));
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

LONG CALLBACK CrashLogVeh(PEXCEPTION_POINTERS ep)
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
    USHORT n = RtlCaptureStackBackTrace(0, _countof(frames), frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        wchar_t tag[16];
        wsprintfW(tag, L"VEH: #%02u", static_cast<unsigned int>(i));
        LogAddress(tag, frames[i]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}


